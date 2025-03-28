/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2015 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef wasmast_h
#define wasmast_h

#include "mozilla/Variant.h"

#include "ds/LifoAlloc.h"
#include "js/HashTable.h"
#include "js/Vector.h"
#include "wasm/WasmTypes.h"
#include "wasm/WasmUtility.h"

namespace js {
namespace wasm {

const uint32_t AstNoIndex = UINT32_MAX;
const unsigned AST_LIFO_DEFAULT_CHUNK_SIZE = 4096;

/*****************************************************************************/
// wasm AST

class AstExpr;

template <class T>
using AstVector = mozilla::Vector<T, 0, LifoAllocPolicy<Fallible>>;

template <class K, class V, class HP>
using AstHashMap = HashMap<K, V, HP, LifoAllocPolicy<Fallible>>;

using mozilla::Variant;

using AstBoolVector = AstVector<bool>;

class AstName {
  const char16_t* begin_;
  const char16_t* end_;

 public:
  template <size_t Length>
  explicit AstName(const char16_t (&str)[Length])
      : begin_(str), end_(str + Length - 1) {
    MOZ_ASSERT(str[Length - 1] == u'\0');
  }

  AstName(const char16_t* begin, size_t length)
      : begin_(begin), end_(begin + length) {}
  AstName() : begin_(nullptr), end_(nullptr) {}
  const char16_t* begin() const { return begin_; }
  const char16_t* end() const { return end_; }
  size_t length() const { return end_ - begin_; }
  bool empty() const { return begin_ == nullptr; }

  bool operator==(AstName rhs) const {
    if (length() != rhs.length()) {
      return false;
    }
    if (begin() == rhs.begin()) {
      return true;
    }
    return EqualChars(begin(), rhs.begin(), length());
  }
  bool operator!=(AstName rhs) const { return !(*this == rhs); }
};

class AstRef {
  AstName name_;
  uint32_t index_;

 public:
  AstRef() : index_(AstNoIndex) { MOZ_ASSERT(isInvalid()); }
  explicit AstRef(AstName name) : name_(name), index_(AstNoIndex) {
    MOZ_ASSERT(!isInvalid());
  }
  explicit AstRef(uint32_t index) : index_(index) { MOZ_ASSERT(!isInvalid()); }
  bool isInvalid() const { return name_.empty() && index_ == AstNoIndex; }
  AstName name() const { return name_; }
  bool isIndex() const { return index_ != AstNoIndex; }
  size_t index() const {
    MOZ_ASSERT(index_ != AstNoIndex);
    return index_;
  }
  void setIndex(uint32_t index) {
    MOZ_ASSERT(index_ == AstNoIndex);
    index_ = index;
  }
  bool operator==(AstRef rhs) const {
    return name_ == rhs.name_ && index_ == rhs.index_;
  }
  bool operator!=(AstRef rhs) const { return !(*this == rhs); }
};

class AstValType {
  // When this type is resolved, which_ becomes IsValType.

  enum { IsValType, IsAstRef } which_;
  ValType type_;
  AstRef ref_;

 public:
  AstValType() : which_(IsValType) {}  // type_ is then !isValid()

  explicit AstValType(ValType type) : which_(IsValType), type_(type) {}

  explicit AstValType(AstRef ref) {
    if (ref.name().empty()) {
      which_ = IsValType;
      type_ = RefType::fromTypeIndex(ref.index());
    } else {
      which_ = IsAstRef;
      ref_ = ref;
    }
  }

#ifdef ENABLE_WASM_GC
  bool isNarrowType() const {
    if (which_ == IsAstRef) {
      return true;
    }
    // This test is just heuristic; to do better (ie to tell whether it is a
    // struct type) we need a type environment, but we have none.
    //
    // In most cases, we won't have a typeIndex here anyway, because types will
    // not have been resolved.
    return type_.isAnyRef() || type_.isTypeIndex();
  }
#endif

  bool isValid() const { return !(which_ == IsValType && !type_.isValid()); }

  bool isResolved() const { return which_ == IsValType; }

  AstRef& asRef() { return ref_; }

  void resolve() {
    MOZ_ASSERT(which_ == IsAstRef);
    which_ = IsValType;
    type_ = RefType::fromTypeIndex(ref_.index());
  }

  ValType::Kind code() const {
    if (which_ == IsValType) {
      return type_.kind();
    }
    return ValType::Ref;
  }

  ValType type() const {
    MOZ_ASSERT(which_ == IsValType);
    return type_;
  }

  bool operator==(const AstValType& that) const {
    if (which_ != that.which_) {
      return false;
    }
    if (which_ == IsValType) {
      return type_ == that.type_;
    }
    return ref_ == that.ref_;
  }

  bool operator!=(const AstValType& that) const { return !(*this == that); }
};

struct AstBlockType {
  enum class Which {
    VoidToVoid,    // [] -> []
    VoidToSingle,  // [] -> T
    Func           // ft: index of [T,..] -> [T,..]
  };

 private:
  Which which_;
  union {
    AstValType voidToSingle_;
    AstRef func_;
  };

 public:
  AstBlockType() : which_(Which::VoidToVoid) {}
  Which which() const { return which_; }
  AstValType& voidToSingleType() {
    MOZ_ASSERT(which_ == Which::VoidToSingle);
    return voidToSingle_;
  }
  void setVoidToSingle(AstValType t) {
    which_ = Which::VoidToSingle;
    voidToSingle_ = t;
  }
  AstRef& funcType() {
    MOZ_ASSERT(which_ == Which::Func);
    return func_;
  }
  void setFunc(AstRef t) {
    which_ = Which::Func;
    func_ = t;
  }
};

struct AstNameHasher {
  using Lookup = const AstName;
  static js::HashNumber hash(Lookup l) {
    return mozilla::HashString(l.begin(), l.length());
  }
  static bool match(const AstName key, Lookup lookup) { return key == lookup; }
};

using AstNameMap = AstHashMap<AstName, uint32_t, AstNameHasher>;

using AstValTypeVector = AstVector<AstValType>;
using AstExprVector = AstVector<AstExpr*>;
using AstNameVector = AstVector<AstName>;
using AstRefVector = AstVector<AstRef>;

struct AstBase {
  void* operator new(size_t numBytes, LifoAlloc& astLifo) noexcept(true) {
    return astLifo.alloc(numBytes);
  }
};

struct AstNode {
  void* operator new(size_t numBytes, LifoAlloc& astLifo) noexcept(true) {
    return astLifo.alloc(numBytes);
  }
};

class AstFuncType;
class AstStructType;

class AstTypeDef : public AstNode {
 protected:
  enum class Which { IsFuncType, IsStructType };

 private:
  Which which_;

 public:
  explicit AstTypeDef(Which which) : which_(which) {}

  bool isFuncType() const { return which_ == Which::IsFuncType; }
  bool isStructType() const { return which_ == Which::IsStructType; }
  inline AstFuncType& asFuncType();
  inline AstStructType& asStructType();
  inline const AstFuncType& asFuncType() const;
  inline const AstStructType& asStructType() const;
};

class AstFuncType : public AstTypeDef {
  AstName name_;
  AstValTypeVector args_;
  AstValTypeVector results_;

 public:
  explicit AstFuncType(LifoAlloc& lifo)
      : AstTypeDef(Which::IsFuncType), args_(lifo), results_(lifo) {}
  AstFuncType(AstValTypeVector&& args, AstValTypeVector&& results)
      : AstTypeDef(Which::IsFuncType),
        args_(std::move(args)),
        results_(std::move(results)) {}
  AstFuncType(AstName name, AstFuncType&& rhs)
      : AstTypeDef(Which::IsFuncType),
        name_(name),
        args_(std::move(rhs.args_)),
        results_(std::move(rhs.results_)) {}
  const AstValTypeVector& args() const { return args_; }
  AstValTypeVector& args() { return args_; }
  const AstValTypeVector& results() const { return results_; }
  AstValTypeVector& results() { return results_; }
  AstName name() const { return name_; }
  bool operator==(const AstFuncType& rhs) const {
    return EqualContainers(args(), rhs.args()) &&
           EqualContainers(results(), rhs.results());
  }

  using Lookup = const AstFuncType&;
  static HashNumber hash(Lookup ft) {
    HashNumber hn = 0;
    for (const AstValType& vt : ft.args()) {
      hn = mozilla::AddToHash(hn, uint32_t(vt.code()));
    }
    for (const AstValType& vt : ft.results()) {
      hn = mozilla::AddToHash(hn, uint32_t(vt.code()));
    }
    return hn;
  }
  static bool match(const AstFuncType* lhs, Lookup rhs) { return *lhs == rhs; }
};

class AstStructType : public AstTypeDef {
  AstName name_;
  AstNameVector fieldNames_;
  AstBoolVector fieldMutability_;
  AstValTypeVector fieldTypes_;

 public:
  explicit AstStructType(LifoAlloc& lifo)
      : AstTypeDef(Which::IsStructType),
        fieldNames_(lifo),
        fieldMutability_(lifo),
        fieldTypes_(lifo) {}
  AstStructType(AstNameVector&& names, AstBoolVector&& mutability,
                AstValTypeVector&& types)
      : AstTypeDef(Which::IsStructType),
        fieldNames_(std::move(names)),
        fieldMutability_(std::move(mutability)),
        fieldTypes_(std::move(types)) {}
  AstStructType(AstName name, AstStructType&& rhs)
      : AstTypeDef(Which::IsStructType),
        name_(name),
        fieldNames_(std::move(rhs.fieldNames_)),
        fieldMutability_(std::move(rhs.fieldMutability_)),
        fieldTypes_(std::move(rhs.fieldTypes_)) {}
  AstName name() const { return name_; }
  const AstNameVector& fieldNames() const { return fieldNames_; }
  const AstBoolVector& fieldMutability() const { return fieldMutability_; }
  const AstValTypeVector& fieldTypes() const { return fieldTypes_; }
  AstValTypeVector& fieldTypes() { return fieldTypes_; }
};

inline AstFuncType& AstTypeDef::asFuncType() {
  MOZ_ASSERT(isFuncType());
  return *static_cast<AstFuncType*>(this);
}

inline AstStructType& AstTypeDef::asStructType() {
  MOZ_ASSERT(isStructType());
  return *static_cast<AstStructType*>(this);
}

inline const AstFuncType& AstTypeDef::asFuncType() const {
  MOZ_ASSERT(isFuncType());
  return *static_cast<const AstFuncType*>(this);
}

inline const AstStructType& AstTypeDef::asStructType() const {
  MOZ_ASSERT(isStructType());
  return *static_cast<const AstStructType*>(this);
}

enum class AstExprKind {
  AtomicCmpXchg,
  AtomicLoad,
  AtomicRMW,
  AtomicStore,
  BinaryOperator,
  Block,
  Branch,
  BranchTable,
  Call,
  CallIndirect,
  ComparisonOperator,
  Const,
  ConversionOperator,
  DataOrElemDrop,
  Drop,
  First,
  GetGlobal,
  GetLocal,
  If,
  Load,
  MemFill,
  MemOrTableCopy,
  MemOrTableInit,
  MemoryGrow,
  MemorySize,
  Nop,
  Pop,
  RefFunc,
  RefNull,
  Return,
  SetGlobal,
  SetLocal,
#ifdef ENABLE_WASM_GC
  StructNew,
  StructGet,
  StructSet,
  StructNarrow,
#endif
#ifdef ENABLE_WASM_REFTYPES
  TableFill,
  TableGet,
  TableGrow,
  TableSet,
  TableSize,
#endif
  TeeLocal,
  Store,
  Select,
  UnaryOperator,
  Unreachable,
  Wait,
  Wake,
  Fence
};

class AstExpr : public AstNode {
  const AstExprKind kind_;

 protected:
  explicit AstExpr(AstExprKind kind) : kind_(kind) {}

 public:
  AstExprKind kind() const { return kind_; }

  template <class T>
  T& as() {
    MOZ_ASSERT(kind() == T::Kind);
    return static_cast<T&>(*this);
  }
};

struct AstNop : AstExpr {
  static const AstExprKind Kind = AstExprKind::Nop;
  AstNop() : AstExpr(AstExprKind::Nop) {}
};

struct AstUnreachable : AstExpr {
  static const AstExprKind Kind = AstExprKind::Unreachable;
  AstUnreachable() : AstExpr(AstExprKind::Unreachable) {}
};

class AstDrop : public AstExpr {
  AstExpr& value_;

 public:
  static const AstExprKind Kind = AstExprKind::Drop;
  explicit AstDrop(AstExpr& value)
      : AstExpr(AstExprKind::Drop), value_(value) {}
  AstExpr& value() const { return value_; }
};

class AstSelect : public AstExpr {
  AstExpr* condition_;
  AstExpr* op1_;
  AstExpr* op2_;
  AstValTypeVector result_;

 public:
  static const AstExprKind Kind = AstExprKind::Select;
  AstSelect(AstExpr* condition, AstExpr* op1, AstExpr* op2,
            AstValTypeVector&& result)
      : AstExpr(Kind),
        condition_(condition),
        op1_(op1),
        op2_(op2),
        result_(std::move(result)) {}

  AstExpr* condition() const { return condition_; }
  AstExpr* op1() const { return op1_; }
  AstExpr* op2() const { return op2_; }
  AstValTypeVector& result() { return result_; }
  const AstValTypeVector& result() const { return result_; }
};

class AstConst : public AstExpr {
  const LitVal val_;

 public:
  static const AstExprKind Kind = AstExprKind::Const;
  explicit AstConst(LitVal val) : AstExpr(Kind), val_(val) {}
  LitVal val() const { return val_; }
};

class AstGetLocal : public AstExpr {
  AstRef local_;

 public:
  static const AstExprKind Kind = AstExprKind::GetLocal;
  explicit AstGetLocal(AstRef local) : AstExpr(Kind), local_(local) {}
  AstRef& local() { return local_; }
};

class AstSetLocal : public AstExpr {
  AstRef local_;
  AstExpr& value_;

 public:
  static const AstExprKind Kind = AstExprKind::SetLocal;
  AstSetLocal(AstRef local, AstExpr& value)
      : AstExpr(Kind), local_(local), value_(value) {}
  AstRef& local() { return local_; }
  AstExpr& value() const { return value_; }
};

class AstGetGlobal : public AstExpr {
  AstRef global_;

 public:
  static const AstExprKind Kind = AstExprKind::GetGlobal;
  explicit AstGetGlobal(AstRef global) : AstExpr(Kind), global_(global) {}
  AstRef& global() { return global_; }
};

class AstSetGlobal : public AstExpr {
  AstRef global_;
  AstExpr& value_;

 public:
  static const AstExprKind Kind = AstExprKind::SetGlobal;
  AstSetGlobal(AstRef global, AstExpr& value)
      : AstExpr(Kind), global_(global), value_(value) {}
  AstRef& global() { return global_; }
  AstExpr& value() const { return value_; }
};

class AstTeeLocal : public AstExpr {
  AstRef local_;
  AstExpr& value_;

 public:
  static const AstExprKind Kind = AstExprKind::TeeLocal;
  AstTeeLocal(AstRef local, AstExpr& value)
      : AstExpr(Kind), local_(local), value_(value) {}
  AstRef& local() { return local_; }
  AstExpr& value() const { return value_; }
};

class AstBlock : public AstExpr {
  Opcode op_;
  AstName name_;
  AstExprVector exprs_;
  AstBlockType type_;

 public:
  static const AstExprKind Kind = AstExprKind::Block;
  explicit AstBlock(Opcode op, AstBlockType type, AstName name,
                    AstExprVector&& exprs)
      : AstExpr(Kind),
        op_(op),
        name_(name),
        exprs_(std::move(exprs)),
        type_(type) {}

  Opcode op() const { return op_; }
  AstName name() const { return name_; }
  const AstExprVector& exprs() const { return exprs_; }
  AstBlockType& type() { return type_; }
};

class AstBranch : public AstExpr {
  Opcode op_;
  AstRef target_;
  AstExprVector values_;  // Includes the condition, for br_if

 public:
  static const AstExprKind Kind = AstExprKind::Branch;
  explicit AstBranch(Opcode op, AstRef target, AstExprVector&& values)
      : AstExpr(Kind), op_(op), target_(target), values_(std::move(values)) {}

  Opcode op() const { return op_; }
  AstRef& target() { return target_; }
  AstExprVector& values() { return values_; }
};

class AstCall : public AstExpr {
  Opcode op_;
  AstRef func_;
  AstExprVector args_;

 public:
  static const AstExprKind Kind = AstExprKind::Call;
  AstCall(Opcode op, AstRef func, AstExprVector&& args)
      : AstExpr(Kind), op_(op), func_(func), args_(std::move(args)) {}

  Opcode op() const { return op_; }
  AstRef& func() { return func_; }
  const AstExprVector& args() const { return args_; }
};

class AstCallIndirect : public AstExpr {
  AstRef targetTable_;
  AstRef funcType_;
  AstExprVector args_;
  AstExpr* index_;

 public:
  static const AstExprKind Kind = AstExprKind::CallIndirect;
  AstCallIndirect(AstRef targetTable, AstRef funcType, AstExprVector&& args,
                  AstExpr* index)
      : AstExpr(Kind),
        targetTable_(targetTable),
        funcType_(funcType),
        args_(std::move(args)),
        index_(index) {}
  AstRef& targetTable() { return targetTable_; }
  AstRef& funcType() { return funcType_; }
  const AstExprVector& args() const { return args_; }
  AstExpr* index() const { return index_; }
};

class AstReturn : public AstExpr {
  AstExpr* maybeExpr_;

 public:
  static const AstExprKind Kind = AstExprKind::Return;
  explicit AstReturn(AstExpr* maybeExpr)
      : AstExpr(Kind), maybeExpr_(maybeExpr) {}
  AstExpr* maybeExpr() const { return maybeExpr_; }
};

class AstIf : public AstExpr {
  AstExpr* cond_;
  AstName name_;
  AstExprVector thenExprs_;
  AstExprVector elseExprs_;
  AstBlockType type_;

 public:
  static const AstExprKind Kind = AstExprKind::If;
  AstIf(AstBlockType type, AstExpr* cond, AstName name,
        AstExprVector&& thenExprs, AstExprVector&& elseExprs)
      : AstExpr(Kind),
        cond_(cond),
        name_(name),
        thenExprs_(std::move(thenExprs)),
        elseExprs_(std::move(elseExprs)),
        type_(type) {}

  AstExpr& cond() const { return *cond_; }
  const AstExprVector& thenExprs() const { return thenExprs_; }
  bool hasElse() const { return elseExprs_.length(); }
  const AstExprVector& elseExprs() const {
    MOZ_ASSERT(hasElse());
    return elseExprs_;
  }
  AstName name() const { return name_; }
  AstBlockType& type() { return type_; }
};

class AstLoadStoreAddress {
  AstExpr* base_;
  int32_t flags_;
  int32_t offset_;

 public:
  explicit AstLoadStoreAddress(AstExpr* base, int32_t flags, int32_t offset)
      : base_(base), flags_(flags), offset_(offset) {}

  AstExpr& base() const { return *base_; }
  int32_t flags() const { return flags_; }
  int32_t offset() const { return offset_; }
};

class AstLoad : public AstExpr {
  Opcode op_;
  AstLoadStoreAddress address_;

 public:
  static const AstExprKind Kind = AstExprKind::Load;
  explicit AstLoad(Opcode op, const AstLoadStoreAddress& address)
      : AstExpr(Kind), op_(op), address_(address) {}

  Opcode op() const { return op_; }
  const AstLoadStoreAddress& address() const { return address_; }
};

class AstStore : public AstExpr {
  Opcode op_;
  AstLoadStoreAddress address_;
  AstExpr* value_;

 public:
  static const AstExprKind Kind = AstExprKind::Store;
  explicit AstStore(Opcode op, const AstLoadStoreAddress& address,
                    AstExpr* value)
      : AstExpr(Kind), op_(op), address_(address), value_(value) {}

  Opcode op() const { return op_; }
  const AstLoadStoreAddress& address() const { return address_; }
  AstExpr& value() const { return *value_; }
};

class AstAtomicCmpXchg : public AstExpr {
  Opcode op_;
  AstLoadStoreAddress address_;
  AstExpr* expected_;
  AstExpr* replacement_;

 public:
  static const AstExprKind Kind = AstExprKind::AtomicCmpXchg;
  explicit AstAtomicCmpXchg(Opcode op, const AstLoadStoreAddress& address,
                            AstExpr* expected, AstExpr* replacement)
      : AstExpr(Kind),
        op_(op),
        address_(address),
        expected_(expected),
        replacement_(replacement) {}

  Opcode op() const { return op_; }
  const AstLoadStoreAddress& address() const { return address_; }
  AstExpr& expected() const { return *expected_; }
  AstExpr& replacement() const { return *replacement_; }
};

class AstAtomicLoad : public AstExpr {
  Opcode op_;
  AstLoadStoreAddress address_;

 public:
  static const AstExprKind Kind = AstExprKind::AtomicLoad;
  explicit AstAtomicLoad(Opcode op, const AstLoadStoreAddress& address)
      : AstExpr(Kind), op_(op), address_(address) {}

  Opcode op() const { return op_; }
  const AstLoadStoreAddress& address() const { return address_; }
};

class AstAtomicRMW : public AstExpr {
  Opcode op_;
  AstLoadStoreAddress address_;
  AstExpr* value_;

 public:
  static const AstExprKind Kind = AstExprKind::AtomicRMW;
  explicit AstAtomicRMW(Opcode op, const AstLoadStoreAddress& address,
                        AstExpr* value)
      : AstExpr(Kind), op_(op), address_(address), value_(value) {}

  Opcode op() const { return op_; }
  const AstLoadStoreAddress& address() const { return address_; }
  AstExpr& value() const { return *value_; }
};

class AstAtomicStore : public AstExpr {
  Opcode op_;
  AstLoadStoreAddress address_;
  AstExpr* value_;

 public:
  static const AstExprKind Kind = AstExprKind::AtomicStore;
  explicit AstAtomicStore(Opcode op, const AstLoadStoreAddress& address,
                          AstExpr* value)
      : AstExpr(Kind), op_(op), address_(address), value_(value) {}

  Opcode op() const { return op_; }
  const AstLoadStoreAddress& address() const { return address_; }
  AstExpr& value() const { return *value_; }
};

class AstWait : public AstExpr {
  Opcode op_;
  AstLoadStoreAddress address_;
  AstExpr* expected_;
  AstExpr* timeout_;

 public:
  static const AstExprKind Kind = AstExprKind::Wait;
  explicit AstWait(Opcode op, const AstLoadStoreAddress& address,
                   AstExpr* expected, AstExpr* timeout)
      : AstExpr(Kind),
        op_(op),
        address_(address),
        expected_(expected),
        timeout_(timeout) {}

  Opcode op() const { return op_; }
  const AstLoadStoreAddress& address() const { return address_; }
  AstExpr& expected() const { return *expected_; }
  AstExpr& timeout() const { return *timeout_; }
};

class AstWake : public AstExpr {
  AstLoadStoreAddress address_;
  AstExpr* count_;

 public:
  static const AstExprKind Kind = AstExprKind::Wake;
  explicit AstWake(const AstLoadStoreAddress& address, AstExpr* count)
      : AstExpr(Kind), address_(address), count_(count) {}

  const AstLoadStoreAddress& address() const { return address_; }
  AstExpr& count() const { return *count_; }
};

struct AstFence : AstExpr {
  static const AstExprKind Kind = AstExprKind::Fence;
  AstFence() : AstExpr(AstExprKind::Fence) {}
};

class AstMemOrTableCopy : public AstExpr {
  bool isMem_;
  AstRef destTable_;
  AstExpr* dest_;
  AstRef srcTable_;
  AstExpr* src_;
  AstExpr* len_;

 public:
  static const AstExprKind Kind = AstExprKind::MemOrTableCopy;
  explicit AstMemOrTableCopy(bool isMem, AstRef destTable, AstExpr* dest,
                             AstRef srcTable, AstExpr* src, AstExpr* len)
      : AstExpr(Kind),
        isMem_(isMem),
        destTable_(destTable),
        dest_(dest),
        srcTable_(srcTable),
        src_(src),
        len_(len) {}

  bool isMem() const { return isMem_; }
  AstRef& destTable() {
    MOZ_ASSERT(!isMem());
    return destTable_;
  }
  AstExpr& dest() const { return *dest_; }
  AstRef& srcTable() {
    MOZ_ASSERT(!isMem());
    return srcTable_;
  }
  AstExpr& src() const { return *src_; }
  AstExpr& len() const { return *len_; }
};

class AstDataOrElemDrop : public AstExpr {
  bool isData_;
  uint32_t segIndex_;

 public:
  static const AstExprKind Kind = AstExprKind::DataOrElemDrop;
  explicit AstDataOrElemDrop(bool isData, uint32_t segIndex)
      : AstExpr(Kind), isData_(isData), segIndex_(segIndex) {}

  bool isData() const { return isData_; }
  uint32_t segIndex() const { return segIndex_; }
};

class AstMemFill : public AstExpr {
  AstExpr* start_;
  AstExpr* val_;
  AstExpr* len_;

 public:
  static const AstExprKind Kind = AstExprKind::MemFill;
  explicit AstMemFill(AstExpr* start, AstExpr* val, AstExpr* len)
      : AstExpr(Kind), start_(start), val_(val), len_(len) {}

  AstExpr& start() const { return *start_; }
  AstExpr& val() const { return *val_; }
  AstExpr& len() const { return *len_; }
};

class AstMemOrTableInit : public AstExpr {
  bool isMem_;
  uint32_t segIndex_;
  AstRef target_;
  AstExpr* dst_;
  AstExpr* src_;
  AstExpr* len_;

 public:
  static const AstExprKind Kind = AstExprKind::MemOrTableInit;
  explicit AstMemOrTableInit(bool isMem, uint32_t segIndex, AstRef target,
                             AstExpr* dst, AstExpr* src, AstExpr* len)
      : AstExpr(Kind),
        isMem_(isMem),
        segIndex_(segIndex),
        target_(target),
        dst_(dst),
        src_(src),
        len_(len) {}

  bool isMem() const { return isMem_; }
  uint32_t segIndex() const { return segIndex_; }
  AstRef& target() { return target_; }
  AstRef& targetTable() {
    MOZ_ASSERT(!isMem());
    return target_;
  }
  AstRef& targetMemory() {
    MOZ_ASSERT(isMem());
    return target_;
  }
  AstExpr& dst() const { return *dst_; }
  AstExpr& src() const { return *src_; }
  AstExpr& len() const { return *len_; }
};

#ifdef ENABLE_WASM_REFTYPES
class AstTableFill : public AstExpr {
  AstRef targetTable_;
  AstExpr* start_;
  AstExpr* val_;
  AstExpr* len_;

 public:
  static const AstExprKind Kind = AstExprKind::TableFill;
  explicit AstTableFill(AstRef targetTable, AstExpr* start, AstExpr* val,
                        AstExpr* len)
      : AstExpr(Kind),
        targetTable_(targetTable),
        start_(start),
        val_(val),
        len_(len) {}

  AstRef& targetTable() { return targetTable_; }
  AstExpr& start() const { return *start_; }
  AstExpr& val() const { return *val_; }
  AstExpr& len() const { return *len_; }
};

class AstTableGet : public AstExpr {
  AstRef targetTable_;
  AstExpr* index_;

 public:
  static const AstExprKind Kind = AstExprKind::TableGet;
  explicit AstTableGet(AstRef targetTable, AstExpr* index)
      : AstExpr(Kind), targetTable_(targetTable), index_(index) {}

  AstRef& targetTable() { return targetTable_; }
  AstExpr& index() const { return *index_; }
};

class AstTableGrow : public AstExpr {
  AstRef targetTable_;
  AstExpr* initValue_;
  AstExpr* delta_;

 public:
  static const AstExprKind Kind = AstExprKind::TableGrow;
  AstTableGrow(AstRef targetTable, AstExpr* initValue, AstExpr* delta)
      : AstExpr(Kind),
        targetTable_(targetTable),
        initValue_(initValue),
        delta_(delta) {}

  AstRef& targetTable() { return targetTable_; }
  AstExpr& initValue() const { return *initValue_; }
  AstExpr& delta() const { return *delta_; }
};

class AstTableSet : public AstExpr {
  AstRef targetTable_;
  AstExpr* index_;
  AstExpr* value_;

 public:
  static const AstExprKind Kind = AstExprKind::TableSet;
  AstTableSet(AstRef targetTable, AstExpr* index, AstExpr* value)
      : AstExpr(Kind),
        targetTable_(targetTable),
        index_(index),
        value_(value) {}

  AstRef& targetTable() { return targetTable_; }
  AstExpr& index() const { return *index_; }
  AstExpr& value() const { return *value_; }
};

class AstTableSize : public AstExpr {
  AstRef targetTable_;

 public:
  static const AstExprKind Kind = AstExprKind::TableSize;
  explicit AstTableSize(AstRef targetTable)
      : AstExpr(Kind), targetTable_(targetTable) {}

  AstRef& targetTable() { return targetTable_; }
};
#endif  // ENABLE_WASM_REFTYPES

#ifdef ENABLE_WASM_GC
class AstStructNew : public AstExpr {
  AstRef structType_;
  AstExprVector fieldValues_;

 public:
  static const AstExprKind Kind = AstExprKind::StructNew;
  AstStructNew(AstRef structType, AstExprVector&& fieldVals)
      : AstExpr(Kind),
        structType_(structType),
        fieldValues_(std::move(fieldVals)) {}
  AstRef& structType() { return structType_; }
  const AstExprVector& fieldValues() const { return fieldValues_; }
};

class AstStructGet : public AstExpr {
  AstRef structType_;
  AstRef fieldName_;
  AstExpr* ptr_;

 public:
  static const AstExprKind Kind = AstExprKind::StructGet;
  AstStructGet(AstRef structType, AstRef fieldName, AstExpr* ptr)
      : AstExpr(Kind),
        structType_(structType),
        fieldName_(fieldName),
        ptr_(ptr) {}
  AstRef& structType() { return structType_; }
  AstRef& fieldName() { return fieldName_; }
  AstExpr& ptr() const { return *ptr_; }
};

class AstStructSet : public AstExpr {
  AstRef structType_;
  AstRef fieldName_;
  AstExpr* ptr_;
  AstExpr* value_;

 public:
  static const AstExprKind Kind = AstExprKind::StructSet;
  AstStructSet(AstRef structType, AstRef fieldName, AstExpr* ptr,
               AstExpr* value)
      : AstExpr(Kind),
        structType_(structType),
        fieldName_(fieldName),
        ptr_(ptr),
        value_(value) {}
  AstRef& structType() { return structType_; }
  AstRef& fieldName() { return fieldName_; }
  AstExpr& ptr() const { return *ptr_; }
  AstExpr& value() const { return *value_; }
};

class AstStructNarrow : public AstExpr {
  AstValType inputStruct_;
  AstValType outputStruct_;
  AstExpr* ptr_;

 public:
  static const AstExprKind Kind = AstExprKind::StructNarrow;
  AstStructNarrow(AstValType inputStruct, AstValType outputStruct, AstExpr* ptr)
      : AstExpr(Kind),
        inputStruct_(inputStruct),
        outputStruct_(outputStruct),
        ptr_(ptr) {}
  AstValType& inputStruct() { return inputStruct_; }
  AstValType& outputStruct() { return outputStruct_; }
  AstExpr& ptr() const { return *ptr_; }
};
#endif

class AstMemorySize final : public AstExpr {
 public:
  static const AstExprKind Kind = AstExprKind::MemorySize;
  explicit AstMemorySize() : AstExpr(Kind) {}
};

class AstMemoryGrow final : public AstExpr {
  AstExpr* operand_;

 public:
  static const AstExprKind Kind = AstExprKind::MemoryGrow;
  explicit AstMemoryGrow(AstExpr* operand) : AstExpr(Kind), operand_(operand) {}

  AstExpr* operand() const { return operand_; }
};

class AstBranchTable : public AstExpr {
  AstRef default_;
  AstRefVector table_;
  AstExprVector values_;

 public:
  static const AstExprKind Kind = AstExprKind::BranchTable;
  explicit AstBranchTable(AstRef def, AstRefVector&& table,
                          AstExprVector&& values)
      : AstExpr(Kind),
        default_(def),
        table_(std::move(table)),
        values_(std::move(values)) {}
  AstRef& def() { return default_; }
  AstRefVector& table() { return table_; }
  AstExprVector& values() { return values_; }
};

class AstFunc : public AstNode {
  AstName name_;
  AstRef funcType_;
  AstValTypeVector vars_;
  AstNameVector localNames_;
  AstExprVector body_;

 public:
  AstFunc(AstName name, AstRef ft, AstValTypeVector&& vars,
          AstNameVector&& locals, AstExprVector&& body)
      : name_(name),
        funcType_(ft),
        vars_(std::move(vars)),
        localNames_(std::move(locals)),
        body_(std::move(body)) {}
  AstRef& funcType() { return funcType_; }
  const AstValTypeVector& vars() const { return vars_; }
  AstValTypeVector& vars() { return vars_; }
  const AstNameVector& locals() const { return localNames_; }
  const AstExprVector& body() const { return body_; }
  AstName name() const { return name_; }
};

class AstGlobal : public AstNode {
  AstName name_;
  bool isMutable_;
  AstValType type_;
  Maybe<AstExpr*> init_;

 public:
  AstGlobal() : isMutable_(false), type_(ValType()) {}

  explicit AstGlobal(AstName name, AstValType type, bool isMutable,
                     const Maybe<AstExpr*>& init = Maybe<AstExpr*>())
      : name_(name), isMutable_(isMutable), type_(type), init_(init) {}

  AstName name() const { return name_; }
  bool isMutable() const { return isMutable_; }
  ValType type() const { return type_.type(); }
  AstValType& type() { return type_; }

  bool hasInit() const { return !!init_; }
  AstExpr& init() const {
    MOZ_ASSERT(hasInit());
    return **init_;
  }
};

using AstGlobalVector = AstVector<AstGlobal*>;

class AstImport : public AstNode {
  AstName name_;
  AstName module_;
  AstName field_;
  DefinitionKind kind_;

  AstRef funcType_;
  Limits limits_;
  TableKind tableKind_;
  AstGlobal global_;

 public:
  AstImport(AstName name, AstName module, AstName field, AstRef funcType)
      : name_(name),
        module_(module),
        field_(field),
        kind_(DefinitionKind::Function),
        funcType_(funcType),
        tableKind_(TableKind::NullRef) {}
  AstImport(AstName name, AstName module, AstName field, DefinitionKind kind,
            const Limits& limits)
      : name_(name),
        module_(module),
        field_(field),
        kind_(kind),
        limits_(limits),
        tableKind_(TableKind::NullRef) {
    MOZ_ASSERT(kind != DefinitionKind::Table,
               "A table must have a meaningful DefinitionKind");
  }
  AstImport(AstName name, AstName module, AstName field, const Limits& limits,
            TableKind tableKind)
      : name_(name),
        module_(module),
        field_(field),
        kind_(DefinitionKind::Table),
        limits_(limits),
        tableKind_(tableKind) {}
  AstImport(AstName name, AstName module, AstName field,
            const AstGlobal& global)
      : name_(name),
        module_(module),
        field_(field),
        kind_(DefinitionKind::Global),
        tableKind_(TableKind::NullRef),
        global_(global) {}

  AstName name() const { return name_; }
  AstName module() const { return module_; }
  AstName field() const { return field_; }

  DefinitionKind kind() const { return kind_; }
  TableKind tableKind() const {
    MOZ_ASSERT(kind_ == DefinitionKind::Table);
    return tableKind_;
  }
  AstRef& funcType() {
    MOZ_ASSERT(kind_ == DefinitionKind::Function);
    return funcType_;
  }
  Limits limits() const {
    MOZ_ASSERT(kind_ == DefinitionKind::Memory ||
               kind_ == DefinitionKind::Table);
    return limits_;
  }
  const AstGlobal& global() const {
    MOZ_ASSERT(kind_ == DefinitionKind::Global);
    return global_;
  }
  AstGlobal& global() {
    MOZ_ASSERT(kind_ == DefinitionKind::Global);
    return global_;
  }
};

class AstExport : public AstNode {
  AstName name_;
  DefinitionKind kind_;
  AstRef ref_;

 public:
  AstExport(AstName name, DefinitionKind kind, AstRef ref)
      : name_(name), kind_(kind), ref_(ref) {}
  explicit AstExport(AstName name, DefinitionKind kind)
      : name_(name), kind_(kind) {}
  AstName name() const { return name_; }
  DefinitionKind kind() const { return kind_; }
  AstRef& ref() { return ref_; }
};

class AstDataSegment : public AstNode {
  AstExpr* offsetIfActive_;
  AstNameVector fragments_;

 public:
  AstDataSegment(AstExpr* offsetIfActive, AstNameVector&& fragments)
      : offsetIfActive_(offsetIfActive), fragments_(std::move(fragments)) {}

  AstExpr* offsetIfActive() const { return offsetIfActive_; }
  const AstNameVector& fragments() const { return fragments_; }
};

using AstDataSegmentVector = AstVector<AstDataSegment*>;

struct AstNullValue {};
typedef Variant<AstRef, AstNullValue> AstElem;
using AstElemVector = AstVector<AstElem>;

enum class AstElemSegmentKind {
  Active,
  Passive,
  Declared,
};

class AstElemSegment : public AstNode {
  AstElemSegmentKind kind_;
  AstRef targetTable_;
  AstExpr* offsetIfActive_;
  ValType elemType_;
  AstElemVector elems_;

 public:
  AstElemSegment(AstElemSegmentKind kind, AstRef targetTable,
                 AstExpr* offsetIfActive, ValType elemType,
                 AstElemVector&& elems)
      : kind_(kind),
        targetTable_(targetTable),
        offsetIfActive_(offsetIfActive),
        elemType_(elemType),
        elems_(std::move(elems)) {}

  AstElemSegmentKind kind() const { return kind_; }
  AstRef targetTable() const { return targetTable_; }
  AstRef& targetTableRef() { return targetTable_; }
  AstExpr* offsetIfActive() const { return offsetIfActive_; }
  ValType elemType() const { return elemType_; }
  AstElemVector& elems() { return elems_; }
  const AstElemVector& elems() const { return elems_; }
};

using AstElemSegmentVector = AstVector<AstElemSegment*>;

class AstStartFunc : public AstNode {
  AstRef func_;

 public:
  explicit AstStartFunc(AstRef func) : func_(func) {}

  AstRef& func() { return func_; }
};

struct AstMemory {
  AstName name;
  Limits limits;
  bool imported;

  AstMemory(const Limits& limits, bool imported, AstName name = AstName())
      : name(name), limits(limits), imported(imported) {}
};

struct AstTable {
  AstName name;
  Limits limits;
  TableKind tableKind;
  bool imported;

  AstTable(const Limits& limits, TableKind tableKind, bool imported,
           AstName name = AstName())
      : name(name), limits(limits), tableKind(tableKind), imported(imported) {}
};

class AstModule : public AstNode {
 public:
  using FuncVector = AstVector<AstFunc*>;
  using ImportVector = AstVector<AstImport*>;
  using ExportVector = AstVector<AstExport*>;
  using TypeDefVector = AstVector<AstTypeDef*>;
  using NameVector = AstVector<AstName>;
  using AstMemoryVector = AstVector<AstMemory>;
  using AstTableVector = AstVector<AstTable>;

 private:
  typedef AstHashMap<AstFuncType*, uint32_t, AstFuncType> FuncTypeMap;

  LifoAlloc& lifo_;
  TypeDefVector types_;
  FuncTypeMap funcTypeMap_;
  ImportVector imports_;
  NameVector funcImportNames_;
  AstTableVector tables_;
  AstMemoryVector memories_;
#ifdef ENABLE_WASM_GC
  uint32_t gcFeatureOptIn_;
#endif
  Maybe<uint32_t> dataCount_;
  ExportVector exports_;
  Maybe<AstStartFunc> startFunc_;
  FuncVector funcs_;
  AstDataSegmentVector dataSegments_;
  AstElemSegmentVector elemSegments_;
  AstGlobalVector globals_;

  size_t numGlobalImports_;

 public:
  explicit AstModule(LifoAlloc& lifo)
      : lifo_(lifo),
        types_(lifo),
        funcTypeMap_(lifo),
        imports_(lifo),
        funcImportNames_(lifo),
        tables_(lifo),
        memories_(lifo),
#ifdef ENABLE_WASM_GC
        gcFeatureOptIn_(0),
#endif
        exports_(lifo),
        funcs_(lifo),
        dataSegments_(lifo),
        elemSegments_(lifo),
        globals_(lifo),
        numGlobalImports_(0) {
  }
  bool addMemory(AstName name, const Limits& memory) {
    return memories_.append(AstMemory(memory, false, name));
  }
  bool hasMemory() const { return !!memories_.length(); }
  const AstMemoryVector& memories() const { return memories_; }
#ifdef ENABLE_WASM_GC
  bool addGcFeatureOptIn(uint32_t version) {
    gcFeatureOptIn_ = version;
    return true;
  }
  uint32_t gcFeatureOptIn() const { return gcFeatureOptIn_; }
#endif
  bool initDataCount(uint32_t dataCount) {
    MOZ_ASSERT(dataCount_.isNothing());
    dataCount_.emplace(dataCount);
    return true;
  }
  Maybe<uint32_t> dataCount() const { return dataCount_; }
  bool addTable(AstName name, const Limits& table, TableKind tableKind) {
    return tables_.append(AstTable(table, tableKind, false, name));
  }
  bool hasTable() const { return !!tables_.length(); }
  const AstTableVector& tables() const { return tables_; }
  bool append(AstDataSegment* seg) { return dataSegments_.append(seg); }
  const AstDataSegmentVector& dataSegments() const { return dataSegments_; }
  bool append(AstElemSegment* seg) { return elemSegments_.append(seg); }
  const AstElemSegmentVector& elemSegments() const { return elemSegments_; }
  bool hasStartFunc() const { return !!startFunc_; }
  bool setStartFunc(AstStartFunc startFunc) {
    if (startFunc_) {
      return false;
    }
    startFunc_.emplace(startFunc);
    return true;
  }
  AstStartFunc& startFunc() { return *startFunc_; }
  bool declare(AstFuncType&& funcType, uint32_t* funcTypeIndex) {
    FuncTypeMap::AddPtr p = funcTypeMap_.lookupForAdd(funcType);
    if (p) {
      *funcTypeIndex = p->value();
      return true;
    }
    *funcTypeIndex = types_.length();
    auto* lifoFuncType =
        new (lifo_) AstFuncType(AstName(), std::move(funcType));
    return lifoFuncType && types_.append(lifoFuncType) &&
           funcTypeMap_.add(p, static_cast<AstFuncType*>(types_.back()),
                            *funcTypeIndex);
  }
  bool append(AstFuncType* funcType) {
    uint32_t funcTypeIndex = types_.length();
    if (!types_.append(funcType)) {
      return false;
    }
    FuncTypeMap::AddPtr p = funcTypeMap_.lookupForAdd(*funcType);
    return p || funcTypeMap_.add(p, funcType, funcTypeIndex);
  }
  const TypeDefVector& types() const { return types_; }
  bool append(AstFunc* func) { return funcs_.append(func); }
  const FuncVector& funcs() const { return funcs_; }
  bool append(AstStructType* str) { return types_.append(str); }
  bool append(AstTypeDef* td) {
    if (td->isFuncType()) {
      return append(&td->asFuncType());
    }
    if (td->isStructType()) {
      return append(&td->asStructType());
    }
    MOZ_CRASH("Bad type");
  }
  bool append(AstImport* imp) {
    switch (imp->kind()) {
      case DefinitionKind::Function:
        if (!funcImportNames_.append(imp->name())) {
          return false;
        }
        break;
      case DefinitionKind::Table:
        if (!tables_.append(AstTable(imp->limits(), imp->tableKind(), true))) {
          return false;
        }
        break;
      case DefinitionKind::Memory:
        if (!memories_.append(AstMemory(imp->limits(), true))) {
          return false;
        }
        break;
      case DefinitionKind::Global:
        numGlobalImports_++;
        break;
    }
    return imports_.append(imp);
  }
  const ImportVector& imports() const { return imports_; }
  const NameVector& funcImportNames() const { return funcImportNames_; }
  size_t numFuncImports() const { return funcImportNames_.length(); }
  bool append(AstExport* exp) { return exports_.append(exp); }
  const ExportVector& exports() const { return exports_; }
  bool append(AstGlobal* glob) { return globals_.append(glob); }
  const AstGlobalVector& globals() const { return globals_; }
  size_t numGlobalImports() const { return numGlobalImports_; }
};

class AstUnaryOperator final : public AstExpr {
  Opcode op_;
  AstExpr* operand_;

 public:
  static const AstExprKind Kind = AstExprKind::UnaryOperator;
  explicit AstUnaryOperator(Opcode op, AstExpr* operand)
      : AstExpr(Kind), op_(op), operand_(operand) {}

  Opcode op() const { return op_; }
  AstExpr* operand() const { return operand_; }
};

class AstBinaryOperator final : public AstExpr {
  Opcode op_;
  AstExpr* lhs_;
  AstExpr* rhs_;

 public:
  static const AstExprKind Kind = AstExprKind::BinaryOperator;
  explicit AstBinaryOperator(Opcode op, AstExpr* lhs, AstExpr* rhs)
      : AstExpr(Kind), op_(op), lhs_(lhs), rhs_(rhs) {}

  Opcode op() const { return op_; }
  AstExpr* lhs() const { return lhs_; }
  AstExpr* rhs() const { return rhs_; }
};

class AstComparisonOperator final : public AstExpr {
  Opcode op_;
  AstExpr* lhs_;
  AstExpr* rhs_;

 public:
  static const AstExprKind Kind = AstExprKind::ComparisonOperator;
  explicit AstComparisonOperator(Opcode op, AstExpr* lhs, AstExpr* rhs)
      : AstExpr(Kind), op_(op), lhs_(lhs), rhs_(rhs) {}

  Opcode op() const { return op_; }
  AstExpr* lhs() const { return lhs_; }
  AstExpr* rhs() const { return rhs_; }
};

class AstConversionOperator final : public AstExpr {
  Opcode op_;
  AstExpr* operand_;

 public:
  static const AstExprKind Kind = AstExprKind::ConversionOperator;
  explicit AstConversionOperator(Opcode op, AstExpr* operand)
      : AstExpr(Kind), op_(op), operand_(operand) {}

  Opcode op() const { return op_; }
  AstExpr* operand() const { return operand_; }
};

class AstRefFunc final : public AstExpr {
  AstRef func_;

 public:
  static const AstExprKind Kind = AstExprKind::RefFunc;
  explicit AstRefFunc(AstRef func) : AstExpr(Kind), func_(func) {}

  AstRef& func() { return func_; }
};

class AstRefNull final : public AstExpr {
 public:
  static const AstExprKind Kind = AstExprKind::RefNull;
  AstRefNull() : AstExpr(Kind) {}
};

// This is an artificial AST node which can fill operand slots in an AST
// constructed from parsing or decoding stack-machine code that doesn't have
// an inherent AST structure.
class AstPop final : public AstExpr {
 public:
  static const AstExprKind Kind = AstExprKind::Pop;
  AstPop() : AstExpr(Kind) {}
};

// This is an artificial AST node which can be used to represent some forms
// of stack-machine code in an AST form. It is similar to Block, but returns the
// value of its first operand, rather than the last.
class AstFirst : public AstExpr {
  AstExprVector exprs_;

 public:
  static const AstExprKind Kind = AstExprKind::First;
  explicit AstFirst(AstExprVector&& exprs)
      : AstExpr(Kind), exprs_(std::move(exprs)) {}

  AstExprVector& exprs() { return exprs_; }
  const AstExprVector& exprs() const { return exprs_; }
};

}  // namespace wasm
}  // namespace js

#endif  // namespace wasmast_h
