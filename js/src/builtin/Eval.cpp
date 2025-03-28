/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/Eval.h"

#include "mozilla/HashFunctions.h"
#include "mozilla/Range.h"

#include "ds/LifoAlloc.h"
#include "frontend/BytecodeCompilation.h"
#include "frontend/CompilationInfo.h"
#include "gc/HashUtil.h"
#include "js/SourceText.h"
#include "js/StableStringChars.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"
#include "vm/JSONParser.h"

#include "debugger/DebugAPI-inl.h"
#include "vm/Interpreter-inl.h"

using namespace js;

using mozilla::AddToHash;
using mozilla::HashString;
using mozilla::RangedPtr;

using JS::AutoCheckCannotGC;
using JS::AutoStableStringChars;
using JS::CompileOptions;
using JS::SourceOwnership;
using JS::SourceText;

// We should be able to assert this for *any* fp->environmentChain().
static void AssertInnerizedEnvironmentChain(JSContext* cx, JSObject& env) {
#ifdef DEBUG
  RootedObject obj(cx);
  for (obj = &env; obj; obj = obj->enclosingEnvironment()) {
    MOZ_ASSERT(!IsWindowProxy(obj));
  }
#endif
}

static bool IsEvalCacheCandidate(JSScript* script) {
  if (!script->isDirectEvalInFunction()) {
    return false;
  }

  // Make sure there are no inner objects (which may be used directly by script
  // and clobbered) or inner functions (which may have wrong scope).
  for (JS::GCCellPtr gcThing : script->gcthings()) {
    if (gcThing.is<JSObject>()) {
      return false;
    }
  }

  return true;
}

/* static */
HashNumber EvalCacheHashPolicy::hash(const EvalCacheLookup& l) {
  HashNumber hash = HashStringChars(l.str);
  return AddToHash(hash, l.callerScript.get(), l.pc);
}

/* static */
bool EvalCacheHashPolicy::match(const EvalCacheEntry& cacheEntry,
                                const EvalCacheLookup& l) {
  MOZ_ASSERT(IsEvalCacheCandidate(cacheEntry.script));

  return EqualStrings(cacheEntry.str, l.str) &&
         cacheEntry.callerScript == l.callerScript && cacheEntry.pc == l.pc;
}

// Add the script to the eval cache when EvalKernel is finished
class EvalScriptGuard {
  JSContext* cx_;
  Rooted<JSScript*> script_;

  /* These fields are only valid if lookup_.str is non-nullptr. */
  EvalCacheLookup lookup_;
  mozilla::Maybe<DependentAddPtr<EvalCache>> p_;

  RootedLinearString lookupStr_;

 public:
  explicit EvalScriptGuard(JSContext* cx)
      : cx_(cx), script_(cx), lookup_(cx), lookupStr_(cx) {}

  ~EvalScriptGuard() {
    if (script_ && !cx_->isExceptionPending()) {
      script_->cacheForEval();
      EvalCacheEntry cacheEntry = {lookupStr_, script_, lookup_.callerScript,
                                   lookup_.pc};
      lookup_.str = lookupStr_;
      if (lookup_.str && IsEvalCacheCandidate(script_)) {
        // Ignore failure to add cache entry.
        if (!p_->add(cx_, cx_->caches().evalCache, lookup_, cacheEntry)) {
          cx_->recoverFromOutOfMemory();
        }
      }
    }
  }

  void lookupInEvalCache(JSLinearString* str, JSScript* callerScript,
                         jsbytecode* pc) {
    lookupStr_ = str;
    lookup_.str = str;
    lookup_.callerScript = callerScript;
    lookup_.pc = pc;
    p_.emplace(cx_, cx_->caches().evalCache, lookup_);
    if (*p_) {
      script_ = (*p_)->script;
      p_->remove(cx_, cx_->caches().evalCache, lookup_);
    }
  }

  void setNewScript(JSScript* script) {
    // JSScript::fullyInitFromStencil has already called js_CallNewScriptHook.
    MOZ_ASSERT(!script_ && script);
    script_ = script;
  }

  bool foundScript() { return !!script_; }

  HandleScript script() {
    MOZ_ASSERT(script_);
    return script_;
  }
};

enum EvalJSONResult { EvalJSON_Failure, EvalJSON_Success, EvalJSON_NotJSON };

template <typename CharT>
static bool EvalStringMightBeJSON(const mozilla::Range<const CharT> chars) {
  // If the eval string starts with '(' or '[' and ends with ')' or ']', it
  // may be JSON.  Try the JSON parser first because it's much faster.  If
  // the eval string isn't JSON, JSON parsing will probably fail quickly, so
  // little time will be lost.
  size_t length = chars.length();
  if (length < 2) {
    return false;
  }

  // It used to be that strings in JavaScript forbid U+2028 LINE SEPARATOR
  // and U+2029 PARAGRAPH SEPARATOR, so something like
  //
  //   eval("['" + "\u2028" + "']");
  //
  // i.e. an array containing a string with a line separator in it, *would*
  // be JSON but *would not* be valid JavaScript.  Handing such a string to
  // the JSON parser would then fail to recognize a syntax error.  As of
  // <https://tc39.github.io/proposal-json-superset/> JavaScript strings may
  // contain these two code points, so it's safe to JSON-parse eval strings
  // that contain them.

  CharT first = chars[0], last = chars[length - 1];
  return (first == '[' && last == ']') || (first == '(' && last == ')');
}

template <typename CharT>
static EvalJSONResult ParseEvalStringAsJSON(
    JSContext* cx, const mozilla::Range<const CharT> chars,
    MutableHandleValue rval) {
  size_t len = chars.length();
  MOZ_ASSERT((chars[0] == '(' && chars[len - 1] == ')') ||
             (chars[0] == '[' && chars[len - 1] == ']'));

  auto jsonChars = (chars[0] == '[') ? chars
                                     : mozilla::Range<const CharT>(
                                           chars.begin().get() + 1U, len - 2);

  Rooted<JSONParser<CharT>> parser(
      cx, JSONParser<CharT>(cx, jsonChars,
                            JSONParserBase::ParseType::AttemptForEval));
  if (!parser.parse(rval)) {
    return EvalJSON_Failure;
  }

  return rval.isUndefined() ? EvalJSON_NotJSON : EvalJSON_Success;
}

static EvalJSONResult TryEvalJSON(JSContext* cx, JSLinearString* str,
                                  MutableHandleValue rval) {
  if (str->hasLatin1Chars()) {
    AutoCheckCannotGC nogc;
    if (!EvalStringMightBeJSON(str->latin1Range(nogc))) {
      return EvalJSON_NotJSON;
    }
  } else {
    AutoCheckCannotGC nogc;
    if (!EvalStringMightBeJSON(str->twoByteRange(nogc))) {
      return EvalJSON_NotJSON;
    }
  }

  AutoStableStringChars linearChars(cx);
  if (!linearChars.init(cx, str)) {
    return EvalJSON_Failure;
  }

  return linearChars.isLatin1()
             ? ParseEvalStringAsJSON(cx, linearChars.latin1Range(), rval)
             : ParseEvalStringAsJSON(cx, linearChars.twoByteRange(), rval);
}

enum EvalType { DIRECT_EVAL, INDIRECT_EVAL };

// 18.2.1.1 PerformEval
//
// Common code implementing direct and indirect eval.
//
// Evaluate call.argv[2], if it is a string, in the context of the given calling
// frame, with the provided scope chain, with the semantics of either a direct
// or indirect eval (see ES5 10.4.2).  If this is an indirect eval, env
// must be a global object.
//
// On success, store the completion value in call.rval and return true.
static bool EvalKernel(JSContext* cx, HandleValue v, EvalType evalType,
                       AbstractFramePtr caller, HandleObject env,
                       jsbytecode* pc, MutableHandleValue vp) {
  MOZ_ASSERT((evalType == INDIRECT_EVAL) == !caller);
  MOZ_ASSERT((evalType == INDIRECT_EVAL) == !pc);
  MOZ_ASSERT_IF(evalType == INDIRECT_EVAL, IsGlobalLexicalEnvironment(env));
  AssertInnerizedEnvironmentChain(cx, *env);

  // Step 2.
  if (!v.isString()) {
    vp.set(v);
    return true;
  }

  // Steps 3-4.
  RootedString str(cx, v.toString());
  if (!GlobalObject::isRuntimeCodeGenEnabled(cx, str, cx->global())) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_CSP_BLOCKED_EVAL);
    return false;
  }

  // Step 5 ff.

  // Per ES5, indirect eval runs in the global scope. (eval is specified this
  // way so that the compiler can make assumptions about what bindings may or
  // may not exist in the current frame if it doesn't see 'eval'.)
  MOZ_ASSERT_IF(evalType != DIRECT_EVAL,
                cx->global() == &env->as<LexicalEnvironmentObject>().global());

  RootedLinearString linearStr(cx, str->ensureLinear(cx));
  if (!linearStr) {
    return false;
  }

  RootedScript callerScript(cx, caller ? caller.script() : nullptr);
  EvalJSONResult ejr = TryEvalJSON(cx, linearStr, vp);
  if (ejr != EvalJSON_NotJSON) {
    return ejr == EvalJSON_Success;
  }

  EvalScriptGuard esg(cx);

  if (evalType == DIRECT_EVAL && caller.isFunctionFrame()) {
    esg.lookupInEvalCache(linearStr, callerScript, pc);
  }

  if (!esg.foundScript()) {
    RootedScript maybeScript(cx);
    unsigned lineno;
    const char* filename;
    bool mutedErrors;
    uint32_t pcOffset;
    if (evalType == DIRECT_EVAL) {
      DescribeScriptedCallerForDirectEval(cx, callerScript, pc, &filename,
                                          &lineno, &pcOffset, &mutedErrors);
      maybeScript = callerScript;
    } else {
      DescribeScriptedCallerForCompilation(cx, &maybeScript, &filename, &lineno,
                                           &pcOffset, &mutedErrors);
    }

    const char* introducerFilename = filename;
    if (maybeScript && maybeScript->scriptSource()->introducerFilename()) {
      introducerFilename = maybeScript->scriptSource()->introducerFilename();
    }

    RootedScope enclosing(cx);
    if (evalType == DIRECT_EVAL) {
      enclosing = callerScript->innermostScope(pc);
    } else {
      enclosing = &cx->global()->emptyGlobalScope();
    }

    CompileOptions options(cx);
    options.setIsRunOnce(true)
        .setNoScriptRval(false)
        .setMutedErrors(mutedErrors)
        .setScriptOrModule(maybeScript);

    if (evalType == DIRECT_EVAL && IsStrictEvalPC(pc)) {
      options.setForceStrictMode();
    }

    if (introducerFilename) {
      options.setFileAndLine(filename, 1);
      options.setIntroductionInfo(introducerFilename, "eval", lineno,
                                  maybeScript, pcOffset);
    } else {
      options.setFileAndLine("eval", 1);
      options.setIntroductionType("eval");
    }

    AutoStableStringChars linearChars(cx);
    if (!linearChars.initTwoByte(cx, linearStr)) {
      return false;
    }

    SourceText<char16_t> srcBuf;

    const char16_t* chars = linearChars.twoByteRange().begin().get();
    SourceOwnership ownership = linearChars.maybeGiveOwnershipToCaller()
                                    ? SourceOwnership::TakeOwnership
                                    : SourceOwnership::Borrowed;
    if (!srcBuf.init(cx, chars, linearStr->length(), ownership)) {
      return false;
    }

    LifoAllocScope allocScope(&cx->tempLifoAlloc());
    frontend::CompilationInfo compilationInfo(cx, allocScope, options);
    if (!compilationInfo.init(cx)) {
      return false;
    }

    frontend::EvalSharedContext evalsc(cx, env, compilationInfo, enclosing,
                                       compilationInfo.directives);
    RootedScript compiled(
        cx, frontend::CompileEvalScript(compilationInfo, evalsc, env, srcBuf));
    if (!compiled) {
      return false;
    }

    esg.setNewScript(compiled);
  }

  // Look up the newTarget from the frame iterator.
  Value newTargetVal = NullValue();
  return ExecuteKernel(cx, esg.script(), *env, newTargetVal,
                       NullFramePtr() /* evalInFrame */, vp.address());
}

bool js::DirectEvalStringFromIon(JSContext* cx, HandleObject env,
                                 HandleScript callerScript,
                                 HandleValue newTargetValue, HandleString str,
                                 jsbytecode* pc, MutableHandleValue vp) {
  AssertInnerizedEnvironmentChain(cx, *env);

  if (!GlobalObject::isRuntimeCodeGenEnabled(cx, str, cx->global())) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_CSP_BLOCKED_EVAL);
    return false;
  }

  // ES5 15.1.2.1 steps 2-8.

  RootedLinearString linearStr(cx, str->ensureLinear(cx));
  if (!linearStr) {
    return false;
  }

  EvalJSONResult ejr = TryEvalJSON(cx, linearStr, vp);
  if (ejr != EvalJSON_NotJSON) {
    return ejr == EvalJSON_Success;
  }

  EvalScriptGuard esg(cx);

  esg.lookupInEvalCache(linearStr, callerScript, pc);

  if (!esg.foundScript()) {
    const char* filename;
    unsigned lineno;
    bool mutedErrors;
    uint32_t pcOffset;
    DescribeScriptedCallerForDirectEval(cx, callerScript, pc, &filename,
                                        &lineno, &pcOffset, &mutedErrors);

    const char* introducerFilename = filename;
    if (callerScript->scriptSource()->introducerFilename()) {
      introducerFilename = callerScript->scriptSource()->introducerFilename();
    }

    RootedScope enclosing(cx, callerScript->innermostScope(pc));

    CompileOptions options(cx);
    options.setIsRunOnce(true);
    options.setNoScriptRval(false);
    options.setMutedErrors(mutedErrors);

    if (IsStrictEvalPC(pc)) {
      options.setForceStrictMode();
    }

    if (introducerFilename) {
      options.setFileAndLine(filename, 1);
      options.setIntroductionInfo(introducerFilename, "eval", lineno,
                                  callerScript, pcOffset);
    } else {
      options.setFileAndLine("eval", 1);
      options.setIntroductionType("eval");
    }

    AutoStableStringChars linearChars(cx);
    if (!linearChars.initTwoByte(cx, linearStr)) {
      return false;
    }

    SourceText<char16_t> srcBuf;

    const char16_t* chars = linearChars.twoByteRange().begin().get();
    SourceOwnership ownership = linearChars.maybeGiveOwnershipToCaller()
                                    ? SourceOwnership::TakeOwnership
                                    : SourceOwnership::Borrowed;
    if (!srcBuf.init(cx, chars, linearStr->length(), ownership)) {
      return false;
    }

    LifoAllocScope allocScope(&cx->tempLifoAlloc());
    frontend::CompilationInfo compilationInfo(cx, allocScope, options);
    if (!compilationInfo.init(cx)) {
      return false;
    }

    frontend::EvalSharedContext evalsc(cx, env, compilationInfo, enclosing,
                                       compilationInfo.directives);
    JSScript* compiled =
        frontend::CompileEvalScript(compilationInfo, evalsc, env, srcBuf);
    if (!compiled) {
      return false;
    }

    esg.setNewScript(compiled);
  }

  return ExecuteKernel(cx, esg.script(), *env, newTargetValue,
                       NullFramePtr() /* evalInFrame */, vp.address());
}

bool js::IndirectEval(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  RootedObject globalLexical(cx, &cx->global()->lexicalEnvironment());

  // Note we'll just pass |undefined| here, then return it directly (or throw
  // if runtime codegen is disabled), if no argument is provided.
  return EvalKernel(cx, args.get(0), INDIRECT_EVAL, NullFramePtr(),
                    globalLexical, nullptr, args.rval());
}

bool js::DirectEval(JSContext* cx, HandleValue v, MutableHandleValue vp) {
  // Direct eval can assume it was called from an interpreted or baseline frame.
  ScriptFrameIter iter(cx);
  AbstractFramePtr caller = iter.abstractFramePtr();

  MOZ_ASSERT(JSOp(*iter.pc()) == JSOp::Eval ||
             JSOp(*iter.pc()) == JSOp::StrictEval ||
             JSOp(*iter.pc()) == JSOp::SpreadEval ||
             JSOp(*iter.pc()) == JSOp::StrictSpreadEval);
  MOZ_ASSERT(caller.realm() == caller.script()->realm());

  RootedObject envChain(cx, caller.environmentChain());
  return EvalKernel(cx, v, DIRECT_EVAL, caller, envChain, iter.pc(), vp);
}

bool js::IsAnyBuiltinEval(JSFunction* fun) {
  return fun->maybeNative() == IndirectEval;
}

static bool ExecuteInExtensibleLexicalEnvironment(JSContext* cx,
                                                  HandleScript scriptArg,
                                                  HandleObject env) {
  CHECK_THREAD(cx);
  cx->check(env);
  MOZ_ASSERT(IsExtensibleLexicalEnvironment(env));
  MOZ_RELEASE_ASSERT(scriptArg->hasNonSyntacticScope());

  RootedScript script(cx, scriptArg);
  if (script->realm() != cx->realm()) {
    script = CloneGlobalScript(cx, ScopeKind::NonSyntactic, script);
    if (!script) {
      return false;
    }

    DebugAPI::onNewScript(cx, script);
  }

  RootedValue rval(cx);
  return ExecuteKernel(cx, script, *env, UndefinedValue(),
                       NullFramePtr() /* evalInFrame */, rval.address());
}

JS_FRIEND_API bool js::ExecuteInFrameScriptEnvironment(
    JSContext* cx, HandleObject objArg, HandleScript scriptArg,
    MutableHandleObject envArg) {
  RootedObject varEnv(cx, NonSyntacticVariablesObject::create(cx));
  if (!varEnv) {
    return false;
  }

  RootedObjectVector envChain(cx);
  if (!envChain.append(objArg)) {
    return false;
  }

  RootedObject env(cx);
  if (!js::CreateObjectsForEnvironmentChain(cx, envChain, varEnv, &env)) {
    return false;
  }

  // Create lexical environment with |this| == objArg, which should be a Gecko
  // MessageManager.
  // NOTE: This is required behavior for Gecko FrameScriptLoader, where some
  // callers try to bind methods from the message manager in their scope chain
  // to |this|, and will fail if it is not bound to a message manager.
  ObjectRealm& realm = ObjectRealm::get(varEnv);
  env =
      realm.getOrCreateNonSyntacticLexicalEnvironment(cx, env, varEnv, objArg);
  if (!env) {
    return false;
  }

  if (!ExecuteInExtensibleLexicalEnvironment(cx, scriptArg, env)) {
    return false;
  }

  envArg.set(env);
  return true;
}

JS_FRIEND_API JSObject* js::NewJSMEnvironment(JSContext* cx) {
  RootedObject varEnv(cx, NonSyntacticVariablesObject::create(cx));
  if (!varEnv) {
    return nullptr;
  }

  // Force LexicalEnvironmentObject to be created.
  ObjectRealm& realm = ObjectRealm::get(varEnv);
  MOZ_ASSERT(!realm.getNonSyntacticLexicalEnvironment(varEnv));
  if (!realm.getOrCreateNonSyntacticLexicalEnvironment(cx, varEnv)) {
    return nullptr;
  }

  return varEnv;
}

JS_FRIEND_API bool js::ExecuteInJSMEnvironment(JSContext* cx,
                                               HandleScript scriptArg,
                                               HandleObject varEnv) {
  RootedObjectVector emptyChain(cx);
  return ExecuteInJSMEnvironment(cx, scriptArg, varEnv, emptyChain);
}

JS_FRIEND_API bool js::ExecuteInJSMEnvironment(JSContext* cx,
                                               HandleScript scriptArg,
                                               HandleObject varEnv,
                                               HandleObjectVector targetObj) {
  cx->check(varEnv);
  MOZ_ASSERT(
      ObjectRealm::get(varEnv).getNonSyntacticLexicalEnvironment(varEnv));
  MOZ_DIAGNOSTIC_ASSERT(scriptArg->noScriptRval());

  RootedObject env(cx, JS_ExtensibleLexicalEnvironment(varEnv));

  // If the Gecko subscript loader specifies target objects, we need to add
  // them to the environment. These are added after the NSVO environment.
  if (!targetObj.empty()) {
    // The environment chain will be as follows:
    //      GlobalObject / BackstagePass
    //      LexicalEnvironmentObject[this=global]
    //      NonSyntacticVariablesObject (the JSMEnvironment)
    //      LexicalEnvironmentObject[this=nsvo]
    //      WithEnvironmentObject[target=targetObj]
    //      LexicalEnvironmentObject[this=targetObj] (*)
    //
    //  (*) This environment intercepts JSOp::GlobalThis.

    // Wrap the target objects in WithEnvironments.
    if (!js::CreateObjectsForEnvironmentChain(cx, targetObj, env, &env)) {
      return false;
    }

    // See CreateNonSyntacticEnvironmentChain
    if (!JSObject::setQualifiedVarObj(cx, env)) {
      return false;
    }

    // Create an extensible LexicalEnvironmentObject for target object
    env = ObjectRealm::get(env).getOrCreateNonSyntacticLexicalEnvironment(cx,
                                                                          env);
    if (!env) {
      return false;
    }
  }

  return ExecuteInExtensibleLexicalEnvironment(cx, scriptArg, env);
}

JS_FRIEND_API JSObject* js::GetJSMEnvironmentOfScriptedCaller(JSContext* cx) {
  FrameIter iter(cx);
  if (iter.done()) {
    return nullptr;
  }

  // WASM frames don't always provide their environment, but we also shouldn't
  // expect to see any calling into here.
  MOZ_RELEASE_ASSERT(!iter.isWasm());

  RootedObject env(cx, iter.environmentChain(cx));
  while (env && !env->is<NonSyntacticVariablesObject>()) {
    env = env->enclosingEnvironment();
  }

  return env;
}

JS_FRIEND_API bool js::IsJSMEnvironment(JSObject* obj) {
  // NOTE: This also returns true if the NonSyntacticVariablesObject was
  // created for reasons other than the JSM loader.
  return obj->is<NonSyntacticVariablesObject>();
}
