/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "IDBObjectStore.h"

#include <numeric>
#include <utility>

#include "FileInfo.h"
#include "IDBCursorType.h"
#include "IDBDatabase.h"
#include "IDBEvents.h"
#include "IDBFactory.h"
#include "IDBIndex.h"
#include "IDBKeyRange.h"
#include "IDBMutableFile.h"
#include "IDBRequest.h"
#include "IDBTransaction.h"
#include "IndexedDatabase.h"
#include "IndexedDatabaseInlines.h"
#include "IndexedDatabaseManager.h"
#include "KeyPath.h"
#include "ProfilerHelpers.h"
#include "ReportInternalError.h"
#include "js/Array.h"  // JS::GetArrayLength, JS::IsArrayObject
#include "js/Class.h"
#include "js/Date.h"
#include "js/StructuredClone.h"
#include "mozilla/EndianUtils.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/SystemGroup.h"
#include "mozilla/dom/BindingUtils.h"
#include "mozilla/dom/BlobBinding.h"
#include "mozilla/dom/File.h"
#include "mozilla/dom/FileBlobImpl.h"
#include "mozilla/dom/IDBMutableFileBinding.h"
#include "mozilla/dom/IDBObjectStoreBinding.h"
#include "mozilla/dom/MemoryBlobImpl.h"
#include "mozilla/dom/StreamBlobImpl.h"
#include "mozilla/dom/StructuredCloneHolder.h"
#include "mozilla/dom/StructuredCloneTags.h"
#include "mozilla/dom/WorkerPrivate.h"
#include "mozilla/dom/WorkerScope.h"
#include "mozilla/dom/indexedDB/PBackgroundIDBSharedTypes.h"
#include "mozilla/ipc/BackgroundChild.h"
#include "mozilla/ipc/PBackgroundSharedTypes.h"
#include "nsCOMPtr.h"
#include "nsQueryObject.h"
#include "nsStreamUtils.h"
#include "nsStringStream.h"

// Include this last to avoid path problems on Windows.
#include "ActorsChild.h"

namespace mozilla {
namespace dom {

using namespace mozilla::dom::indexedDB;
using namespace mozilla::dom::quota;
using namespace mozilla::ipc;

namespace {

IndexUpdateInfo MakeIndexUpdateInfo(const int64_t aIndexID, const Key& aKey,
                                    const nsCString& aLocale,
                                    ErrorResult* const aRv) {
  IndexUpdateInfo indexUpdateInfo;
  indexUpdateInfo.indexId() = aIndexID;
  indexUpdateInfo.value() = aKey;
  if (!aLocale.IsEmpty()) {
    const auto result =
        aKey.ToLocaleAwareKey(indexUpdateInfo.localizedValue(), aLocale, *aRv);
    if (result.Is(Invalid, *aRv)) {
      aRv->Throw(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
    }
  }
  return indexUpdateInfo;
}

}  // namespace

struct IDBObjectStore::StructuredCloneWriteInfo {
  JSAutoStructuredCloneBuffer mCloneBuffer;
  nsTArray<StructuredCloneFile> mFiles;
  IDBDatabase* mDatabase;
  uint64_t mOffsetToKeyProp;

  explicit StructuredCloneWriteInfo(IDBDatabase* aDatabase)
      : mCloneBuffer(JS::StructuredCloneScope::DifferentProcessForIndexedDB,
                     nullptr, nullptr),
        mDatabase(aDatabase),
        mOffsetToKeyProp(0) {
    MOZ_ASSERT(aDatabase);

    MOZ_COUNT_CTOR(StructuredCloneWriteInfo);
  }

  StructuredCloneWriteInfo(StructuredCloneWriteInfo&& aCloneWriteInfo) noexcept
      : mCloneBuffer(std::move(aCloneWriteInfo.mCloneBuffer)),
        mDatabase(aCloneWriteInfo.mDatabase),
        mOffsetToKeyProp(aCloneWriteInfo.mOffsetToKeyProp) {
    MOZ_ASSERT(mDatabase);

    MOZ_COUNT_CTOR(StructuredCloneWriteInfo);

    mFiles.SwapElements(aCloneWriteInfo.mFiles);
    aCloneWriteInfo.mOffsetToKeyProp = 0;
  }

  MOZ_COUNTED_DTOR(StructuredCloneWriteInfo)
};

// Used by ValueWrapper::Clone to hold strong references to any blob-like
// objects through the clone process.  This is necessary because:
// - The structured clone process may trigger content code via getters/other
//   which can potentially cause existing strong references to be dropped,
//   necessitating the clone to hold its own strong references.
// - The structured clone can abort partway through, so it's necessary to track
//   what strong references have been acquired so that they can be freed even
//   if a de-serialization does not occur.
struct IDBObjectStore::StructuredCloneInfo {
  nsTArray<StructuredCloneFile> mFiles;
};

namespace {

struct MOZ_STACK_CLASS MutableFileData final {
  nsString type;
  nsString name;

  MOZ_COUNTED_DEFAULT_CTOR(MutableFileData)

  MOZ_COUNTED_DTOR(MutableFileData)
};

struct MOZ_STACK_CLASS BlobOrFileData final {
  uint32_t tag;
  uint64_t size;
  nsString type;
  nsString name;
  int64_t lastModifiedDate;

  BlobOrFileData() : tag(0), size(0), lastModifiedDate(INT64_MAX) {
    MOZ_COUNT_CTOR(BlobOrFileData);
  }

  MOZ_COUNTED_DTOR(BlobOrFileData)
};

struct MOZ_STACK_CLASS WasmModuleData final {
  uint32_t bytecodeIndex;
  uint32_t compiledIndex;
  uint32_t flags;

  explicit WasmModuleData(uint32_t aFlags)
      : bytecodeIndex(0), compiledIndex(0), flags(aFlags) {
    MOZ_COUNT_CTOR(WasmModuleData);
  }

  MOZ_COUNTED_DTOR(WasmModuleData)
};

struct MOZ_STACK_CLASS GetAddInfoClosure final {
  IDBObjectStore::StructuredCloneWriteInfo& mCloneWriteInfo;
  JS::Handle<JS::Value> mValue;

  GetAddInfoClosure(IDBObjectStore::StructuredCloneWriteInfo& aCloneWriteInfo,
                    JS::Handle<JS::Value> aValue)
      : mCloneWriteInfo(aCloneWriteInfo), mValue(aValue) {
    MOZ_COUNT_CTOR(GetAddInfoClosure);
  }

  MOZ_COUNTED_DTOR(GetAddInfoClosure)
};

RefPtr<IDBRequest> GenerateRequest(JSContext* aCx,
                                   IDBObjectStore* aObjectStore) {
  MOZ_ASSERT(aObjectStore);
  aObjectStore->AssertIsOnOwningThread();

  IDBTransaction* const transaction = aObjectStore->Transaction();

  return IDBRequest::Create(aCx, aObjectStore, transaction->Database(),
                            transaction);
}

bool StructuredCloneWriteCallback(JSContext* aCx,
                                  JSStructuredCloneWriter* aWriter,
                                  JS::Handle<JSObject*> aObj,
                                  bool* aSameProcessRequired, void* aClosure) {
  MOZ_ASSERT(aCx);
  MOZ_ASSERT(aWriter);
  MOZ_ASSERT(aClosure);

  auto* const cloneWriteInfo =
      static_cast<IDBObjectStore::StructuredCloneWriteInfo*>(aClosure);

  if (JS_GetClass(aObj) == IDBObjectStore::DummyPropClass()) {
    MOZ_ASSERT(!cloneWriteInfo->mOffsetToKeyProp);
    cloneWriteInfo->mOffsetToKeyProp = js::GetSCOffset(aWriter);

    uint64_t value = 0;
    // Omit endian swap
    return JS_WriteBytes(aWriter, &value, sizeof(value));
  }

  // UNWRAP_OBJECT calls might mutate this.
  JS::Rooted<JSObject*> obj(aCx, aObj);

  IDBMutableFile* mutableFile;
  if (NS_SUCCEEDED(UNWRAP_OBJECT(IDBMutableFile, &obj, mutableFile))) {
    if (cloneWriteInfo->mDatabase->IsFileHandleDisabled()) {
      return false;
    }

    IDBDatabase* const database = mutableFile->Database();
    MOZ_ASSERT(database);

    // Throw when trying to store IDBMutableFile objects that live in a
    // different database.
    if (database != cloneWriteInfo->mDatabase) {
      MOZ_ASSERT(!SameCOMIdentity(database, cloneWriteInfo->mDatabase));

      if (database->Name() != cloneWriteInfo->mDatabase->Name()) {
        return false;
      }

      nsCString fileOrigin, databaseOrigin;
      PersistenceType filePersistenceType, databasePersistenceType;

      if (NS_WARN_IF(NS_FAILED(
              database->GetQuotaInfo(fileOrigin, &filePersistenceType)))) {
        return false;
      }

      if (NS_WARN_IF(NS_FAILED(cloneWriteInfo->mDatabase->GetQuotaInfo(
              databaseOrigin, &databasePersistenceType)))) {
        return false;
      }

      if (filePersistenceType != databasePersistenceType ||
          fileOrigin != databaseOrigin) {
        return false;
      }
    }

    if (cloneWriteInfo->mFiles.Length() > size_t(UINT32_MAX)) {
      MOZ_ASSERT(false, "Fix the structured clone data to use a bigger type!");
      return false;
    }

    const uint32_t index = cloneWriteInfo->mFiles.Length();

    const NS_ConvertUTF16toUTF8 convType(mutableFile->Type());
    const uint32_t convTypeLength =
        NativeEndian::swapToLittleEndian(convType.Length());

    const NS_ConvertUTF16toUTF8 convName(mutableFile->Name());
    const uint32_t convNameLength =
        NativeEndian::swapToLittleEndian(convName.Length());

    if (!JS_WriteUint32Pair(aWriter, SCTAG_DOM_MUTABLEFILE, uint32_t(index)) ||
        !JS_WriteBytes(aWriter, &convTypeLength, sizeof(uint32_t)) ||
        !JS_WriteBytes(aWriter, convType.get(), convType.Length()) ||
        !JS_WriteBytes(aWriter, &convNameLength, sizeof(uint32_t)) ||
        !JS_WriteBytes(aWriter, convName.get(), convName.Length())) {
      return false;
    }

    const DebugOnly<StructuredCloneFile*> newFile =
        cloneWriteInfo->mFiles.EmplaceBack(mutableFile);
    MOZ_ASSERT(newFile);

    return true;
  }

  {
    Blob* blob = nullptr;
    if (NS_SUCCEEDED(UNWRAP_OBJECT(Blob, &obj, blob))) {
      ErrorResult rv;
      const uint64_t nativeEndianSize = blob->GetSize(rv);
      MOZ_ASSERT(!rv.Failed());

      const uint64_t size = NativeEndian::swapToLittleEndian(nativeEndianSize);

      nsString type;
      blob->GetType(type);

      const NS_ConvertUTF16toUTF8 convType(type);
      const uint32_t convTypeLength =
          NativeEndian::swapToLittleEndian(convType.Length());

      if (cloneWriteInfo->mFiles.Length() > size_t(UINT32_MAX)) {
        MOZ_ASSERT(false,
                   "Fix the structured clone data to use a bigger type!");
        return false;
      }

      const uint32_t index = cloneWriteInfo->mFiles.Length();

      if (!JS_WriteUint32Pair(aWriter,
                              blob->IsFile() ? SCTAG_DOM_FILE : SCTAG_DOM_BLOB,
                              index) ||
          !JS_WriteBytes(aWriter, &size, sizeof(size)) ||
          !JS_WriteBytes(aWriter, &convTypeLength, sizeof(convTypeLength)) ||
          !JS_WriteBytes(aWriter, convType.get(), convType.Length())) {
        return false;
      }

      const RefPtr<File> file = blob->ToFile();
      if (file) {
        ErrorResult rv;
        const int64_t nativeEndianLastModifiedDate = file->GetLastModified(rv);
        MOZ_ALWAYS_TRUE(!rv.Failed());

        const int64_t lastModifiedDate =
            NativeEndian::swapToLittleEndian(nativeEndianLastModifiedDate);

        nsString name;
        file->GetName(name);

        const NS_ConvertUTF16toUTF8 convName(name);
        const uint32_t convNameLength =
            NativeEndian::swapToLittleEndian(convName.Length());

        if (!JS_WriteBytes(aWriter, &lastModifiedDate,
                           sizeof(lastModifiedDate)) ||
            !JS_WriteBytes(aWriter, &convNameLength, sizeof(convNameLength)) ||
            !JS_WriteBytes(aWriter, convName.get(), convName.Length())) {
          return false;
        }
      }

      const DebugOnly<StructuredCloneFile*> newFile =
          cloneWriteInfo->mFiles.EmplaceBack(StructuredCloneFile::eBlob, blob);
      MOZ_ASSERT(newFile);

      return true;
    }
  }

  return StructuredCloneHolder::WriteFullySerializableObjects(aCx, aWriter,
                                                              aObj);
}

bool CopyingStructuredCloneWriteCallback(JSContext* aCx,
                                         JSStructuredCloneWriter* aWriter,
                                         JS::Handle<JSObject*> aObj,
                                         bool* aSameProcessRequired,
                                         void* aClosure) {
  MOZ_ASSERT(aCx);
  MOZ_ASSERT(aWriter);
  MOZ_ASSERT(aClosure);

  auto* const cloneInfo =
      static_cast<IDBObjectStore::StructuredCloneInfo*>(aClosure);

  // UNWRAP_OBJECT calls might mutate this.
  JS::Rooted<JSObject*> obj(aCx, aObj);

  {
    Blob* blob = nullptr;
    if (NS_SUCCEEDED(UNWRAP_OBJECT(Blob, &obj, blob))) {
      if (cloneInfo->mFiles.Length() > size_t(UINT32_MAX)) {
        MOZ_ASSERT(false,
                   "Fix the structured clone data to use a bigger type!");
        return false;
      }

      const uint32_t index = cloneInfo->mFiles.Length();

      if (!JS_WriteUint32Pair(aWriter,
                              blob->IsFile() ? SCTAG_DOM_FILE : SCTAG_DOM_BLOB,
                              index)) {
        return false;
      }

      const DebugOnly<StructuredCloneFile*> newFile =
          cloneInfo->mFiles.EmplaceBack(StructuredCloneFile::eBlob, blob);
      MOZ_ASSERT(newFile);

      return true;
    }
  }

  {
    IDBMutableFile* mutableFile;
    if (NS_SUCCEEDED(UNWRAP_OBJECT(IDBMutableFile, &obj, mutableFile))) {
      if (cloneInfo->mFiles.Length() > size_t(UINT32_MAX)) {
        MOZ_ASSERT(false,
                   "Fix the structured clone data to use a bigger type!");
        return false;
      }

      const uint32_t index = cloneInfo->mFiles.Length();

      if (!JS_WriteUint32Pair(aWriter, SCTAG_DOM_MUTABLEFILE, index)) {
        return false;
      }

      const DebugOnly<StructuredCloneFile*> newFile =
          cloneInfo->mFiles.EmplaceBack(mutableFile);
      MOZ_ASSERT(newFile);

      return true;
    }
  }

  return StructuredCloneHolder::WriteFullySerializableObjects(aCx, aWriter,
                                                              aObj);
}

nsresult GetAddInfoCallback(JSContext* aCx, void* aClosure) {
  static const JSStructuredCloneCallbacks kStructuredCloneCallbacks = {
      nullptr /* read */,          StructuredCloneWriteCallback /* write */,
      nullptr /* reportError */,   nullptr /* readTransfer */,
      nullptr /* writeTransfer */, nullptr /* freeTransfer */,
      nullptr /* canTransfer */,   nullptr /* sabCloned */
  };

  MOZ_ASSERT(aCx);

  auto* const data = static_cast<GetAddInfoClosure*>(aClosure);
  MOZ_ASSERT(data);

  data->mCloneWriteInfo.mOffsetToKeyProp = 0;

  if (!data->mCloneWriteInfo.mCloneBuffer.write(aCx, data->mValue,
                                                &kStructuredCloneCallbacks,
                                                &data->mCloneWriteInfo)) {
    return NS_ERROR_DOM_DATA_CLONE_ERR;
  }

  return NS_OK;
}

bool StructuredCloneReadString(JSStructuredCloneReader* aReader,
                               nsCString& aString) {
  uint32_t length;
  if (!JS_ReadBytes(aReader, &length, sizeof(uint32_t))) {
    NS_WARNING("Failed to read length!");
    return false;
  }
  length = NativeEndian::swapFromLittleEndian(length);

  if (!aString.SetLength(length, fallible)) {
    NS_WARNING("Out of memory?");
    return false;
  }
  char* const buffer = aString.BeginWriting();

  if (!JS_ReadBytes(aReader, buffer, length)) {
    NS_WARNING("Failed to read type!");
    return false;
  }

  return true;
}

bool ReadFileHandle(JSStructuredCloneReader* aReader,
                    MutableFileData* aRetval) {
  static_assert(SCTAG_DOM_MUTABLEFILE == 0xFFFF8004, "Update me!");
  MOZ_ASSERT(aReader && aRetval);

  nsCString type;
  if (!StructuredCloneReadString(aReader, type)) {
    return false;
  }
  CopyUTF8toUTF16(type, aRetval->type);

  nsCString name;
  if (!StructuredCloneReadString(aReader, name)) {
    return false;
  }
  CopyUTF8toUTF16(name, aRetval->name);

  return true;
}

bool ReadBlobOrFile(JSStructuredCloneReader* aReader, uint32_t aTag,
                    BlobOrFileData* aRetval) {
  static_assert(SCTAG_DOM_BLOB == 0xffff8001 &&
                    SCTAG_DOM_FILE_WITHOUT_LASTMODIFIEDDATE == 0xffff8002 &&
                    SCTAG_DOM_FILE == 0xffff8005,
                "Update me!");

  MOZ_ASSERT(aReader);
  MOZ_ASSERT(aTag == SCTAG_DOM_FILE ||
             aTag == SCTAG_DOM_FILE_WITHOUT_LASTMODIFIEDDATE ||
             aTag == SCTAG_DOM_BLOB);
  MOZ_ASSERT(aRetval);

  aRetval->tag = aTag;

  uint64_t size;
  if (NS_WARN_IF(!JS_ReadBytes(aReader, &size, sizeof(uint64_t)))) {
    return false;
  }

  aRetval->size = NativeEndian::swapFromLittleEndian(size);

  nsCString type;
  if (NS_WARN_IF(!StructuredCloneReadString(aReader, type))) {
    return false;
  }

  CopyUTF8toUTF16(type, aRetval->type);

  // Blobs are done.
  if (aTag == SCTAG_DOM_BLOB) {
    return true;
  }

  MOZ_ASSERT(aTag == SCTAG_DOM_FILE ||
             aTag == SCTAG_DOM_FILE_WITHOUT_LASTMODIFIEDDATE);

  int64_t lastModifiedDate;
  if (aTag == SCTAG_DOM_FILE_WITHOUT_LASTMODIFIEDDATE) {
    lastModifiedDate = INT64_MAX;
  } else {
    if (NS_WARN_IF(!JS_ReadBytes(aReader, &lastModifiedDate,
                                 sizeof(lastModifiedDate)))) {
      return false;
    }
    lastModifiedDate = NativeEndian::swapFromLittleEndian(lastModifiedDate);
  }

  aRetval->lastModifiedDate = lastModifiedDate;

  nsCString name;
  if (NS_WARN_IF(!StructuredCloneReadString(aReader, name))) {
    return false;
  }

  CopyUTF8toUTF16(name, aRetval->name);

  return true;
}

bool ReadWasmModule(JSStructuredCloneReader* aReader, WasmModuleData* aRetval) {
  static_assert(SCTAG_DOM_WASM_MODULE == 0xFFFF8006, "Update me!");
  MOZ_ASSERT(aReader && aRetval);

  uint32_t bytecodeIndex;
  uint32_t compiledIndex;
  if (NS_WARN_IF(!JS_ReadUint32Pair(aReader, &bytecodeIndex, &compiledIndex))) {
    return false;
  }

  aRetval->bytecodeIndex = bytecodeIndex;
  aRetval->compiledIndex = compiledIndex;

  return true;
}

template <typename T>
bool WrapAsJSObject(JSContext* const aCx, T& aBaseObject,
                    JS::MutableHandle<JSObject*> aResult) {
  JS::Rooted<JS::Value> wrappedValue(aCx);
  if (!ToJSValue(aCx, aBaseObject, &wrappedValue)) {
    return false;
  }

  aResult.set(&wrappedValue.toObject());
  return true;
}

template <typename T>
JSObject* WrapAsJSObject(JSContext* const aCx, T& aBaseObject) {
  JS::Rooted<JSObject*> result(aCx);
  const bool res = WrapAsJSObject(aCx, aBaseObject, &result);
  return res ? static_cast<JSObject*>(result) : nullptr;
}

class ValueDeserializationHelper {
 public:
  static bool CreateAndWrapMutableFile(JSContext* aCx,
                                       StructuredCloneFile& aFile,
                                       const MutableFileData& aData,
                                       JS::MutableHandle<JSObject*> aResult) {
    MOZ_ASSERT(aCx);

    // If we have eBlob, we are in an IDB SQLite schema upgrade where we don't
    // care about a real 'MutableFile', but we just care of having a proper
    // |mType| flag.
    if (aFile.Type() == StructuredCloneFile::eBlob) {
      aFile.MutateType(StructuredCloneFile::eMutableFile);

      // Just make a dummy object.
      JS::Rooted<JSObject*> obj(aCx, JS_NewPlainObject(aCx));

      if (NS_WARN_IF(!obj)) {
        return false;
      }

      aResult.set(obj);
      return true;
    }

    MOZ_ASSERT(aFile.Type() == StructuredCloneFile::eMutableFile);

    if (!aFile.HasMutableFile() || !NS_IsMainThread()) {
      return false;
    }

    aFile.MutableMutableFile().SetLazyData(aData.name, aData.type);

    return WrapAsJSObject(aCx, aFile.MutableMutableFile(), aResult);
  }

  static bool CreateAndWrapBlobOrFile(JSContext* aCx, IDBDatabase* aDatabase,
                                      const StructuredCloneFile& aFile,
                                      const BlobOrFileData& aData,
                                      JS::MutableHandle<JSObject*> aResult) {
    MOZ_ASSERT(aCx);
    MOZ_ASSERT(aData.tag == SCTAG_DOM_FILE ||
               aData.tag == SCTAG_DOM_FILE_WITHOUT_LASTMODIFIEDDATE ||
               aData.tag == SCTAG_DOM_BLOB);
    MOZ_ASSERT(aFile.Type() == StructuredCloneFile::eBlob);

    const auto blob = [&aFile, aDatabase, aCx]() -> RefPtr<Blob> {
      if (aFile.HasBlob()) {
        return aFile.BlobPtr();
      }

      // It can happen that this IDB is chrome code, so there is no parent, but
      // still we want to set a correct parent for the new File object.
      const auto global = [aDatabase, aCx]() -> nsCOMPtr<nsIGlobalObject> {
        if (NS_IsMainThread()) {
          if (aDatabase && aDatabase->GetParentObject()) {
            return aDatabase->GetParentObject();
          }
          return xpc::CurrentNativeGlobal(aCx);
        }
        const WorkerPrivate* const workerPrivate =
            GetCurrentThreadWorkerPrivate();
        MOZ_ASSERT(workerPrivate);

        WorkerGlobalScope* const globalScope = workerPrivate->GlobalScope();
        MOZ_ASSERT(globalScope);

        return do_QueryObject(globalScope);
      }();

      MOZ_ASSERT(global);

      /* If we are creating an index, we do not have an mBlob but do have an
       * FileInfo.  Unlike other index or upgrade cases, we do need a
       * real-looking Blob/File instance because the index's key path can
       * reference their properties.  Rather than create a fake-looking object,
       * create a real Blob. */
      const nsCOMPtr<nsIFile> file =
          FileInfo::GetFileForFileInfo(aFile.FileInfo());
      if (!file) {
        return nullptr;
      }

      const auto impl = MakeRefPtr<FileBlobImpl>(file);
      impl->SetFileId(aFile.FileInfo().Id());
      return File::Create(global, impl);
    }();
    if (NS_WARN_IF(!blob)) {
      return false;
    }

    if (aData.tag == SCTAG_DOM_BLOB) {
      blob->Impl()->SetLazyData(VoidString(), aData.type, aData.size,
                                INT64_MAX);
      MOZ_ASSERT(!blob->IsFile());

      // ActorsParent sends here a kind of half blob and half file wrapped into
      // a DOM File object. DOM File and DOM Blob are a WebIDL wrapper around a
      // BlobImpl object. SetLazyData() has just changed the BlobImpl to be a
      // Blob (see the previous assert), but 'blob' still has the WebIDL DOM
      // File wrapping.
      // Before exposing it to content, we must recreate a DOM Blob object.

      const RefPtr<Blob> exposedBlob =
          Blob::Create(blob->GetParentObject(), blob->Impl());
      if (NS_WARN_IF(!exposedBlob)) {
        return false;
      }

      return WrapAsJSObject(aCx, exposedBlob, aResult);
    }

    blob->Impl()->SetLazyData(aData.name, aData.type, aData.size,
                              aData.lastModifiedDate * PR_USEC_PER_MSEC);

    MOZ_ASSERT(blob->IsFile());
    const RefPtr<File> file = blob->ToFile();
    MOZ_ASSERT(file);

    return WrapAsJSObject(aCx, file, aResult);
  }

  static bool CreateAndWrapWasmModule(JSContext* aCx,
                                      const StructuredCloneFile& aFile,
                                      const WasmModuleData& aData,
                                      JS::MutableHandle<JSObject*> aResult) {
    MOZ_ASSERT(aCx);
    MOZ_ASSERT(aFile.Type() == StructuredCloneFile::eWasmBytecode);
    MOZ_ASSERT(!aFile.HasBlob());

    // Just create a plain object here, support for de-serialization of
    // WebAssembly.Modules has been removed in bug 1561876. Full removal is
    // tracked in bug 1487479.

    JS::Rooted<JSObject*> obj(aCx, JS_NewPlainObject(aCx));
    if (NS_WARN_IF(!obj)) {
      return false;
    }

    aResult.set(obj);
    return true;
  }
};
}  // namespace

namespace indexedDB {
JSObject* CommonStructuredCloneReadCallback(
    JSContext* aCx, JSStructuredCloneReader* aReader,
    const JS::CloneDataPolicy& aCloneDataPolicy, uint32_t aTag, uint32_t aData,
    StructuredCloneReadInfo* aCloneReadInfo, IDBDatabase* aDatabase) {
  // We need to statically assert that our tag values are what we expect
  // so that if people accidentally change them they notice.
  static_assert(SCTAG_DOM_BLOB == 0xffff8001 &&
                    SCTAG_DOM_FILE_WITHOUT_LASTMODIFIEDDATE == 0xffff8002 &&
                    SCTAG_DOM_MUTABLEFILE == 0xffff8004 &&
                    SCTAG_DOM_FILE == 0xffff8005 &&
                    SCTAG_DOM_WASM_MODULE == 0xffff8006,
                "You changed our structured clone tag values and just ate "
                "everyone's IndexedDB data.  I hope you are happy.");

  if (aTag == SCTAG_DOM_FILE_WITHOUT_LASTMODIFIEDDATE ||
      aTag == SCTAG_DOM_BLOB || aTag == SCTAG_DOM_FILE ||
      aTag == SCTAG_DOM_MUTABLEFILE || aTag == SCTAG_DOM_WASM_MODULE) {
    JS::Rooted<JSObject*> result(aCx);

    if (aTag == SCTAG_DOM_WASM_MODULE) {
      WasmModuleData data(aData);
      if (NS_WARN_IF(!ReadWasmModule(aReader, &data))) {
        return nullptr;
      }

      MOZ_ASSERT(data.compiledIndex == data.bytecodeIndex + 1);
      MOZ_ASSERT(!data.flags);

      const auto& files = aCloneReadInfo->Files();
      if (data.bytecodeIndex >= files.Length() ||
          data.compiledIndex >= files.Length()) {
        MOZ_ASSERT(false, "Bad index value!");
        return nullptr;
      }

      const StructuredCloneFile& file = files[data.bytecodeIndex];

      if (NS_WARN_IF(!ValueDeserializationHelper::CreateAndWrapWasmModule(
              aCx, file, data, &result))) {
        return nullptr;
      }

      return result;
    }

    if (aData >= aCloneReadInfo->Files().Length()) {
      MOZ_ASSERT(false, "Bad index value!");
      return nullptr;
    }

    StructuredCloneFile& file = aCloneReadInfo->MutableFile(aData);

    if (aTag == SCTAG_DOM_MUTABLEFILE) {
      MutableFileData data;
      if (NS_WARN_IF(!ReadFileHandle(aReader, &data))) {
        return nullptr;
      }

      if (NS_WARN_IF(!ValueDeserializationHelper::CreateAndWrapMutableFile(
              aCx, file, data, &result))) {
        return nullptr;
      }

      return result;
    }

    BlobOrFileData data;
    if (NS_WARN_IF(!ReadBlobOrFile(aReader, aTag, &data))) {
      return nullptr;
    }

    if (NS_WARN_IF(!ValueDeserializationHelper::CreateAndWrapBlobOrFile(
            aCx, aDatabase, file, data, &result))) {
      return nullptr;
    }

    return result;
  }

  return StructuredCloneHolder::ReadFullySerializableObjects(aCx, aReader,
                                                             aTag);
}
}  // namespace indexedDB

namespace {
JSObject* CopyingStructuredCloneReadCallback(
    JSContext* aCx, JSStructuredCloneReader* aReader,
    const JS::CloneDataPolicy& aCloneDataPolicy, uint32_t aTag, uint32_t aData,
    void* aClosure) {
  MOZ_ASSERT(aTag != SCTAG_DOM_FILE_WITHOUT_LASTMODIFIEDDATE);

  if (aTag == SCTAG_DOM_BLOB || aTag == SCTAG_DOM_FILE ||
      aTag == SCTAG_DOM_MUTABLEFILE) {
    auto* const cloneInfo =
        static_cast<IDBObjectStore::StructuredCloneInfo*>(aClosure);

    if (aData >= cloneInfo->mFiles.Length()) {
      MOZ_ASSERT(false, "Bad index value!");
      return nullptr;
    }

    StructuredCloneFile& file = cloneInfo->mFiles[aData];

    switch (static_cast<StructuredCloneTags>(aTag)) {
      case SCTAG_DOM_BLOB:
        MOZ_ASSERT(file.Type() == StructuredCloneFile::eBlob);
        MOZ_ASSERT(!file.Blob().IsFile());

        return WrapAsJSObject(aCx, file.MutableBlob());

      case SCTAG_DOM_FILE: {
        MOZ_ASSERT(file.Type() == StructuredCloneFile::eBlob);

        JS::Rooted<JSObject*> result(aCx);

        {
          // Create a scope so ~RefPtr fires before returning an unwrapped
          // JS::Value.
          const RefPtr<Blob> blob = file.BlobPtr();
          MOZ_ASSERT(blob->IsFile());

          const RefPtr<File> file = blob->ToFile();
          MOZ_ASSERT(file);

          if (!WrapAsJSObject(aCx, file, &result)) {
            return nullptr;
          }
        }

        return result;
      }

      case SCTAG_DOM_MUTABLEFILE:
        MOZ_ASSERT(file.Type() == StructuredCloneFile::eMutableFile);

        return WrapAsJSObject(aCx, file.MutableMutableFile());

      default:
        // This cannot be reached due to the if condition before.
        break;
    }
  }

  return StructuredCloneHolder::ReadFullySerializableObjects(aCx, aReader,
                                                             aTag);
}

}  // namespace

const JSClass IDBObjectStore::sDummyPropJSClass = {
    "IDBObjectStore Dummy", 0 /* flags */
};

IDBObjectStore::IDBObjectStore(IDBTransaction* aTransaction,
                               ObjectStoreSpec* aSpec)
    : mTransaction(aTransaction),
      mCachedKeyPath(JS::UndefinedValue()),
      mSpec(aSpec),
      mId(aSpec->metadata().id()),
      mRooted(false) {
  MOZ_ASSERT(aTransaction);
  aTransaction->AssertIsOnOwningThread();
  MOZ_ASSERT(aSpec);
}

IDBObjectStore::~IDBObjectStore() {
  AssertIsOnOwningThread();

  if (mRooted) {
    mCachedKeyPath.setUndefined();
    mozilla::DropJSObjects(this);
  }
}

// static
RefPtr<IDBObjectStore> IDBObjectStore::Create(IDBTransaction* aTransaction,
                                              ObjectStoreSpec& aSpec) {
  MOZ_ASSERT(aTransaction);
  aTransaction->AssertIsOnOwningThread();

  return new IDBObjectStore(aTransaction, &aSpec);
}

// static
void IDBObjectStore::AppendIndexUpdateInfo(
    const int64_t aIndexID, const KeyPath& aKeyPath, const bool aMultiEntry,
    const nsCString& aLocale, JSContext* const aCx, JS::Handle<JS::Value> aVal,
    nsTArray<IndexUpdateInfo>* const aUpdateInfoArray, ErrorResult* const aRv) {
  // This precondition holds when `aVal` is the result of a structured clone.
  js::AutoAssertNoContentJS noContentJS(aCx);

  if (!aMultiEntry) {
    Key key;
    *aRv = aKeyPath.ExtractKey(aCx, aVal, key);

    // If an index's keyPath doesn't match an object, we ignore that object.
    if (aRv->ErrorCodeIs(NS_ERROR_DOM_INDEXEDDB_DATA_ERR) || key.IsUnset()) {
      aRv->SuppressException();
      return;
    }

    if (aRv->Failed()) {
      return;
    }

    *aUpdateInfoArray->AppendElement() =
        MakeIndexUpdateInfo(aIndexID, key, aLocale, aRv);
    return;
  }

  JS::Rooted<JS::Value> val(aCx);
  if (NS_FAILED(aKeyPath.ExtractKeyAsJSVal(aCx, aVal, val.address()))) {
    return;
  }

  bool isArray;
  if (NS_WARN_IF(!JS::IsArrayObject(aCx, val, &isArray))) {
    IDB_REPORT_INTERNAL_ERR();
    aRv->Throw(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
    return;
  }
  if (isArray) {
    JS::Rooted<JSObject*> array(aCx, &val.toObject());
    uint32_t arrayLength;
    if (NS_WARN_IF(!JS::GetArrayLength(aCx, array, &arrayLength))) {
      IDB_REPORT_INTERNAL_ERR();
      aRv->Throw(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
      return;
    }

    for (uint32_t arrayIndex = 0; arrayIndex < arrayLength; arrayIndex++) {
      JS::RootedId indexId(aCx);
      if (NS_WARN_IF(!JS_IndexToId(aCx, arrayIndex, &indexId))) {
        IDB_REPORT_INTERNAL_ERR();
        aRv->Throw(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
        return;
      }

      bool hasOwnProperty;
      if (NS_WARN_IF(
              !JS_HasOwnPropertyById(aCx, array, indexId, &hasOwnProperty))) {
        IDB_REPORT_INTERNAL_ERR();
        aRv->Throw(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
        return;
      }

      if (!hasOwnProperty) {
        continue;
      }

      JS::RootedValue arrayItem(aCx);
      if (NS_WARN_IF(!JS_GetPropertyById(aCx, array, indexId, &arrayItem))) {
        IDB_REPORT_INTERNAL_ERR();
        aRv->Throw(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
        return;
      }

      Key value;
      const auto result = value.SetFromJSVal(aCx, arrayItem, *aRv);
      if (!result.Is(Ok, *aRv) || value.IsUnset()) {
        // Not a value we can do anything with, ignore it.
        aRv->SuppressException();
        continue;
      }

      *aUpdateInfoArray->AppendElement() =
          MakeIndexUpdateInfo(aIndexID, value, aLocale, aRv);
      if (aRv->Failed()) {
        return;
      }
    }
  } else {
    Key value;
    const auto result = value.SetFromJSVal(aCx, val, *aRv);
    if (!result.Is(Ok, *aRv) || value.IsUnset()) {
      // Not a value we can do anything with, ignore it.
      aRv->SuppressException();
      return;
    }

    *aUpdateInfoArray->AppendElement() =
        MakeIndexUpdateInfo(aIndexID, value, aLocale, aRv);
  }
}

// static
void IDBObjectStore::ClearCloneReadInfo(StructuredCloneReadInfo& aReadInfo) {
  // This is kind of tricky, we only want to release stuff on the main thread,
  // but we can end up being called on other threads if we have already been
  // cleared on the main thread.
  if (!aReadInfo.HasFiles()) {
    return;
  }

  aReadInfo.ReleaseFiles();
}

// static
bool IDBObjectStore::DeserializeValue(
    JSContext* aCx, StructuredCloneReadInfoChild&& aCloneReadInfo,
    JS::MutableHandle<JS::Value> aValue) {
  MOZ_ASSERT(aCx);

  if (!aCloneReadInfo.Data().Size()) {
    aValue.setUndefined();
    return true;
  }

  MOZ_ASSERT(!(aCloneReadInfo.Data().Size() % sizeof(uint64_t)));

  static const JSStructuredCloneCallbacks callbacks = {
      StructuredCloneReadCallback<StructuredCloneReadInfoChild>,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr};

  // FIXME: Consider to use StructuredCloneHolder here and in other
  //        deserializing methods.
  return JS_ReadStructuredClone(
      aCx, aCloneReadInfo.Data(), JS_STRUCTURED_CLONE_VERSION,
      JS::StructuredCloneScope::DifferentProcessForIndexedDB, aValue,
      JS::CloneDataPolicy(), &callbacks, &aCloneReadInfo);
}

#ifdef DEBUG

void IDBObjectStore::AssertIsOnOwningThread() const {
  MOZ_ASSERT(mTransaction);
  mTransaction->AssertIsOnOwningThread();
}

#endif  // DEBUG

void IDBObjectStore::GetAddInfo(JSContext* aCx, ValueWrapper& aValueWrapper,
                                JS::Handle<JS::Value> aKeyVal,
                                StructuredCloneWriteInfo& aCloneWriteInfo,
                                Key& aKey,
                                nsTArray<IndexUpdateInfo>& aUpdateInfoArray,
                                ErrorResult& aRv) {
  // Return DATA_ERR if a key was passed in and this objectStore uses inline
  // keys.
  if (!aKeyVal.isUndefined() && HasValidKeyPath()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_DATA_ERR);
    return;
  }

  const bool isAutoIncrement = AutoIncrement();

  if (!HasValidKeyPath()) {
    // Out-of-line keys must be passed in.
    const auto result = aKey.SetFromJSVal(aCx, aKeyVal, aRv);
    if (!result.Is(Ok, aRv)) {
      if (result.Is(Invalid, aRv)) {
        aRv.Throw(NS_ERROR_DOM_INDEXEDDB_DATA_ERR);
      }
      return;
    }
  } else if (!isAutoIncrement) {
    if (!aValueWrapper.Clone(aCx)) {
      aRv.Throw(NS_ERROR_DOM_DATA_CLONE_ERR);
      return;
    }

    aRv = GetKeyPath().ExtractKey(aCx, aValueWrapper.Value(), aKey);
    if (aRv.Failed()) {
      return;
    }
  }

  // Return DATA_ERR if no key was specified this isn't an autoIncrement
  // objectStore.
  if (aKey.IsUnset() && !isAutoIncrement) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_DATA_ERR);
    return;
  }

  // Figure out indexes and the index values to update here.

  if (mSpec->indexes().Length() && !aValueWrapper.Clone(aCx)) {
    aRv.Throw(NS_ERROR_DOM_DATA_CLONE_ERR);
    return;
  }

  {
    const nsTArray<IndexMetadata>& indexes = mSpec->indexes();
    const uint32_t idxCount = indexes.Length();

    aUpdateInfoArray.SetCapacity(idxCount);  // Pretty good estimate

    for (uint32_t idxIndex = 0; idxIndex < idxCount; idxIndex++) {
      const IndexMetadata& metadata = indexes[idxIndex];

      AppendIndexUpdateInfo(metadata.id(), metadata.keyPath(),
                            metadata.multiEntry(), metadata.locale(), aCx,
                            aValueWrapper.Value(), &aUpdateInfoArray, &aRv);
      if (NS_WARN_IF(aRv.Failed())) {
        return;
      }
    }
  }

  if (isAutoIncrement && HasValidKeyPath()) {
    if (!aValueWrapper.Clone(aCx)) {
      aRv.Throw(NS_ERROR_DOM_DATA_CLONE_ERR);
      return;
    }

    GetAddInfoClosure data(aCloneWriteInfo, aValueWrapper.Value());

    MOZ_ASSERT(aKey.IsUnset());

    aRv = GetKeyPath().ExtractOrCreateKey(aCx, aValueWrapper.Value(), aKey,
                                          &GetAddInfoCallback, &data);
  } else {
    GetAddInfoClosure data(aCloneWriteInfo, aValueWrapper.Value());

    aRv = GetAddInfoCallback(aCx, &data);
  }
}

RefPtr<IDBRequest> IDBObjectStore::AddOrPut(JSContext* aCx,
                                            ValueWrapper& aValueWrapper,
                                            JS::Handle<JS::Value> aKey,
                                            bool aOverwrite, bool aFromCursor,
                                            ErrorResult& aRv) {
  AssertIsOnOwningThread();
  MOZ_ASSERT(aCx);
  MOZ_ASSERT_IF(aFromCursor, aOverwrite);

  if (mTransaction->GetMode() == IDBTransaction::Mode::Cleanup ||
      mDeletedSpec) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_NOT_ALLOWED_ERR);
    return nullptr;
  }

  if (!mTransaction->IsActive()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  if (!mTransaction->IsWriteAllowed()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_READ_ONLY_ERR);
    return nullptr;
  }

  Key key;
  StructuredCloneWriteInfo cloneWriteInfo(mTransaction->Database());
  nsTArray<IndexUpdateInfo> updateInfo;

  {
    const auto autoStateRestore =
        mTransaction->TemporarilyTransitionToInactive();
    GetAddInfo(aCx, aValueWrapper, aKey, cloneWriteInfo, key, updateInfo, aRv);
  }

  if (aRv.Failed()) {
    return nullptr;
  }

  if (!mTransaction->IsActive()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  // Check the size limit of the serialized message which mainly consists of
  // a StructuredCloneBuffer, an encoded object key, and the encoded index keys.
  // kMaxIDBMsgOverhead covers the minor stuff not included in this calculation
  // because the precise calculation would slow down this AddOrPut operation.
  static const size_t kMaxIDBMsgOverhead = 1024 * 1024;  // 1MB
  const uint32_t maximalSizeFromPref =
      IndexedDatabaseManager::MaxSerializedMsgSize();
  MOZ_ASSERT(maximalSizeFromPref > kMaxIDBMsgOverhead);
  const size_t kMaxMessageSize = maximalSizeFromPref - kMaxIDBMsgOverhead;

  const size_t indexUpdateInfoSize =
      std::accumulate(updateInfo.cbegin(), updateInfo.cend(), 0u,
                      [](size_t old, const IndexUpdateInfo& updateInfo) {
                        return old + updateInfo.value().GetBuffer().Length() +
                               updateInfo.localizedValue().GetBuffer().Length();
                      });

  const size_t messageSize = cloneWriteInfo.mCloneBuffer.data().Size() +
                             key.GetBuffer().Length() + indexUpdateInfoSize;

  if (messageSize > kMaxMessageSize) {
    IDB_REPORT_INTERNAL_ERR();
    aRv.ThrowUnknownError(
        nsPrintfCString("The serialized value is too large"
                        " (size=%zu bytes, max=%zu bytes).",
                        messageSize, kMaxMessageSize));
    return nullptr;
  }

  ObjectStoreAddPutParams commonParams;
  commonParams.objectStoreId() = Id();
  commonParams.cloneInfo().data().data =
      std::move(cloneWriteInfo.mCloneBuffer.data());
  commonParams.cloneInfo().offsetToKeyProp() = cloneWriteInfo.mOffsetToKeyProp;
  commonParams.key() = key;
  commonParams.indexUpdateInfos().SwapElements(updateInfo);

  // Convert any blobs or mutable files into FileAddInfo.
  nsTArray<StructuredCloneFile>& files = cloneWriteInfo.mFiles;

  if (!files.IsEmpty()) {
    const uint32_t count = files.Length();

    FallibleTArray<FileAddInfo> fileAddInfos;
    if (NS_WARN_IF(!fileAddInfos.SetCapacity(count, fallible))) {
      aRv = NS_ERROR_OUT_OF_MEMORY;
      return nullptr;
    }

    IDBDatabase* const database = mTransaction->Database();

    for (uint32_t index = 0; index < count; index++) {
      StructuredCloneFile& file = files[index];

      FileAddInfo* const fileAddInfo = fileAddInfos.AppendElement(fallible);
      MOZ_ASSERT(fileAddInfo);

      switch (file.Type()) {
        case StructuredCloneFile::eBlob: {
          MOZ_ASSERT(file.HasBlob());
          MOZ_ASSERT(!file.HasMutableFile());

          PBackgroundIDBDatabaseFileChild* const fileActor =
              database->GetOrCreateFileActorForBlob(file.MutableBlob());
          if (NS_WARN_IF(!fileActor)) {
            IDB_REPORT_INTERNAL_ERR();
            aRv = NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
            return nullptr;
          }

          fileAddInfo->file() = fileActor;
          fileAddInfo->type() = StructuredCloneFile::eBlob;

          break;
        }

        case StructuredCloneFile::eMutableFile: {
          MOZ_ASSERT(file.HasMutableFile());
          MOZ_ASSERT(!file.HasBlob());

          PBackgroundMutableFileChild* const mutableFileActor =
              file.MutableFile().GetBackgroundActor();
          if (NS_WARN_IF(!mutableFileActor)) {
            IDB_REPORT_INTERNAL_ERR();
            aRv = NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
            return nullptr;
          }

          fileAddInfo->file() = mutableFileActor;
          fileAddInfo->type() = StructuredCloneFile::eMutableFile;

          break;
        }

        case StructuredCloneFile::eWasmBytecode:
        case StructuredCloneFile::eWasmCompiled: {
          MOZ_ASSERT(file.HasBlob());
          MOZ_ASSERT(!file.HasMutableFile());

          PBackgroundIDBDatabaseFileChild* const fileActor =
              database->GetOrCreateFileActorForBlob(file.MutableBlob());
          if (NS_WARN_IF(!fileActor)) {
            IDB_REPORT_INTERNAL_ERR();
            aRv = NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
            return nullptr;
          }

          fileAddInfo->file() = fileActor;
          fileAddInfo->type() = file.Type();

          break;
        }

        default:
          MOZ_CRASH("Should never get here!");
      }
    }

    commonParams.fileAddInfos().SwapElements(fileAddInfos);
  }

  const auto& params =
      aOverwrite ? RequestParams{ObjectStorePutParams(std::move(commonParams))}
                 : RequestParams{ObjectStoreAddParams(std::move(commonParams))};

  auto request = GenerateRequest(aCx, this);
  MOZ_ASSERT(request);

  if (!aFromCursor) {
    if (aOverwrite) {
      IDB_LOG_MARK_CHILD_TRANSACTION_REQUEST(
          "database(%s).transaction(%s).objectStore(%s).put(%s)",
          "IDBObjectStore.put()", mTransaction->LoggingSerialNumber(),
          request->LoggingSerialNumber(),
          IDB_LOG_STRINGIFY(mTransaction->Database()),
          IDB_LOG_STRINGIFY(mTransaction), IDB_LOG_STRINGIFY(this),
          IDB_LOG_STRINGIFY(key));
    } else {
      IDB_LOG_MARK_CHILD_TRANSACTION_REQUEST(
          "database(%s).transaction(%s).objectStore(%s).add(%s)",
          "IDBObjectStore.add()", mTransaction->LoggingSerialNumber(),
          request->LoggingSerialNumber(),
          IDB_LOG_STRINGIFY(mTransaction->Database()),
          IDB_LOG_STRINGIFY(mTransaction), IDB_LOG_STRINGIFY(this),
          IDB_LOG_STRINGIFY(key));
    }
  }

  mTransaction->StartRequest(request, params);

  mTransaction->InvalidateCursorCaches();

  return request;
}

RefPtr<IDBRequest> IDBObjectStore::GetAllInternal(
    bool aKeysOnly, JSContext* aCx, JS::Handle<JS::Value> aKey,
    const Optional<uint32_t>& aLimit, ErrorResult& aRv) {
  AssertIsOnOwningThread();

  if (mDeletedSpec) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_NOT_ALLOWED_ERR);
    return nullptr;
  }

  if (!mTransaction->IsActive()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  RefPtr<IDBKeyRange> keyRange;
  IDBKeyRange::FromJSVal(aCx, aKey, &keyRange, aRv);
  if (NS_WARN_IF(aRv.Failed())) {
    return nullptr;
  }

  const int64_t id = Id();

  Maybe<SerializedKeyRange> optionalKeyRange;
  if (keyRange) {
    SerializedKeyRange serializedKeyRange;
    keyRange->ToSerialized(serializedKeyRange);
    optionalKeyRange.emplace(serializedKeyRange);
  }

  const uint32_t limit = aLimit.WasPassed() ? aLimit.Value() : 0;

  RequestParams params;
  if (aKeysOnly) {
    params = ObjectStoreGetAllKeysParams(id, optionalKeyRange, limit);
  } else {
    params = ObjectStoreGetAllParams(id, optionalKeyRange, limit);
  }

  auto request = GenerateRequest(aCx, this);
  MOZ_ASSERT(request);

  if (aKeysOnly) {
    IDB_LOG_MARK_CHILD_TRANSACTION_REQUEST(
        "database(%s).transaction(%s).objectStore(%s)."
        "getAllKeys(%s, %s)",
        "IDBObjectStore.getAllKeys()", mTransaction->LoggingSerialNumber(),
        request->LoggingSerialNumber(),
        IDB_LOG_STRINGIFY(mTransaction->Database()),
        IDB_LOG_STRINGIFY(mTransaction), IDB_LOG_STRINGIFY(this),
        IDB_LOG_STRINGIFY(keyRange), IDB_LOG_STRINGIFY(aLimit));
  } else {
    IDB_LOG_MARK_CHILD_TRANSACTION_REQUEST(
        "database(%s).transaction(%s).objectStore(%s)."
        "getAll(%s, %s)",
        "IDBObjectStore.getAll()", mTransaction->LoggingSerialNumber(),
        request->LoggingSerialNumber(),
        IDB_LOG_STRINGIFY(mTransaction->Database()),
        IDB_LOG_STRINGIFY(mTransaction), IDB_LOG_STRINGIFY(this),
        IDB_LOG_STRINGIFY(keyRange), IDB_LOG_STRINGIFY(aLimit));
  }

  // TODO: This is necessary to preserve request ordering only. Proper
  // sequencing of requests should be done in a more sophisticated manner that
  // doesn't require invalidating cursor caches (Bug 1580499).
  mTransaction->InvalidateCursorCaches();

  mTransaction->StartRequest(request, params);

  return request;
}

RefPtr<IDBRequest> IDBObjectStore::Add(JSContext* aCx,
                                       JS::Handle<JS::Value> aValue,
                                       JS::Handle<JS::Value> aKey,
                                       ErrorResult& aRv) {
  AssertIsOnOwningThread();

  ValueWrapper valueWrapper(aCx, aValue);

  return AddOrPut(aCx, valueWrapper, aKey, false, /* aFromCursor */ false, aRv);
}

RefPtr<IDBRequest> IDBObjectStore::Put(JSContext* aCx,
                                       JS::Handle<JS::Value> aValue,
                                       JS::Handle<JS::Value> aKey,
                                       ErrorResult& aRv) {
  AssertIsOnOwningThread();

  ValueWrapper valueWrapper(aCx, aValue);

  return AddOrPut(aCx, valueWrapper, aKey, true, /* aFromCursor */ false, aRv);
}

RefPtr<IDBRequest> IDBObjectStore::Delete(JSContext* aCx,
                                          JS::Handle<JS::Value> aKey,
                                          ErrorResult& aRv) {
  AssertIsOnOwningThread();

  return DeleteInternal(aCx, aKey, /* aFromCursor */ false, aRv);
}

RefPtr<IDBRequest> IDBObjectStore::Get(JSContext* aCx,
                                       JS::Handle<JS::Value> aKey,
                                       ErrorResult& aRv) {
  AssertIsOnOwningThread();

  return GetInternal(/* aKeyOnly */ false, aCx, aKey, aRv);
}

RefPtr<IDBRequest> IDBObjectStore::GetKey(JSContext* aCx,
                                          JS::Handle<JS::Value> aKey,
                                          ErrorResult& aRv) {
  AssertIsOnOwningThread();

  return GetInternal(/* aKeyOnly */ true, aCx, aKey, aRv);
}

RefPtr<IDBRequest> IDBObjectStore::Clear(JSContext* aCx, ErrorResult& aRv) {
  AssertIsOnOwningThread();

  if (mDeletedSpec) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_NOT_ALLOWED_ERR);
    return nullptr;
  }

  if (!mTransaction->IsActive()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  if (!mTransaction->IsWriteAllowed()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_READ_ONLY_ERR);
    return nullptr;
  }

  const ObjectStoreClearParams params = {Id()};

  auto request = GenerateRequest(aCx, this);
  MOZ_ASSERT(request);

  IDB_LOG_MARK_CHILD_TRANSACTION_REQUEST(
      "database(%s).transaction(%s).objectStore(%s).clear()",
      "IDBObjectStore.clear()", mTransaction->LoggingSerialNumber(),
      request->LoggingSerialNumber(),
      IDB_LOG_STRINGIFY(mTransaction->Database()),
      IDB_LOG_STRINGIFY(mTransaction), IDB_LOG_STRINGIFY(this));

  mTransaction->InvalidateCursorCaches();

  mTransaction->StartRequest(request, params);

  return request;
}

RefPtr<IDBRequest> IDBObjectStore::GetAll(JSContext* aCx,
                                          JS::Handle<JS::Value> aKey,
                                          const Optional<uint32_t>& aLimit,
                                          ErrorResult& aRv) {
  AssertIsOnOwningThread();

  return GetAllInternal(/* aKeysOnly */ false, aCx, aKey, aLimit, aRv);
}

RefPtr<IDBRequest> IDBObjectStore::GetAllKeys(JSContext* aCx,
                                              JS::Handle<JS::Value> aKey,
                                              const Optional<uint32_t>& aLimit,
                                              ErrorResult& aRv) {
  AssertIsOnOwningThread();

  return GetAllInternal(/* aKeysOnly */ true, aCx, aKey, aLimit, aRv);
}

RefPtr<IDBRequest> IDBObjectStore::OpenCursor(JSContext* aCx,
                                              JS::Handle<JS::Value> aRange,
                                              IDBCursorDirection aDirection,
                                              ErrorResult& aRv) {
  AssertIsOnOwningThread();

  return OpenCursorInternal(/* aKeysOnly */ false, aCx, aRange, aDirection,
                            aRv);
}

RefPtr<IDBRequest> IDBObjectStore::OpenCursor(JSContext* aCx,
                                              IDBCursorDirection aDirection,
                                              ErrorResult& aRv) {
  AssertIsOnOwningThread();

  return OpenCursorInternal(/* aKeysOnly */ false, aCx,
                            JS::UndefinedHandleValue, aDirection, aRv);
}

RefPtr<IDBRequest> IDBObjectStore::OpenKeyCursor(JSContext* aCx,
                                                 JS::Handle<JS::Value> aRange,
                                                 IDBCursorDirection aDirection,
                                                 ErrorResult& aRv) {
  AssertIsOnOwningThread();

  return OpenCursorInternal(/* aKeysOnly */ true, aCx, aRange, aDirection, aRv);
}

RefPtr<IDBIndex> IDBObjectStore::Index(const nsAString& aName,
                                       ErrorResult& aRv) {
  AssertIsOnOwningThread();

  if (mTransaction->IsCommittingOrFinished() || mDeletedSpec) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return nullptr;
  }

  const nsTArray<IndexMetadata>& indexMetadatas = mSpec->indexes();

  const auto endIndexMetadatas = indexMetadatas.cend();
  const auto foundMetadata =
      std::find_if(indexMetadatas.cbegin(), endIndexMetadatas,
                   [&aName](const auto& indexMetadata) {
                     return indexMetadata.name() == aName;
                   });

  if (foundMetadata == endIndexMetadatas) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_NOT_FOUND_ERR);
    return nullptr;
  }

  const IndexMetadata& metadata = *foundMetadata;

  const auto endIndexes = mIndexes.cend();
  const auto foundIndex =
      std::find_if(mIndexes.cbegin(), endIndexes,
                   [desiredId = metadata.id()](const auto& index) {
                     return index->Id() == desiredId;
                   });

  RefPtr<IDBIndex> index;

  if (foundIndex == endIndexes) {
    index = IDBIndex::Create(this, metadata);
    MOZ_ASSERT(index);

    mIndexes.AppendElement(index);
  } else {
    index = *foundIndex;
  }

  return index;
}

NS_IMPL_CYCLE_COLLECTION_CLASS(IDBObjectStore)

NS_IMPL_CYCLE_COLLECTION_TRACE_BEGIN(IDBObjectStore)
  NS_IMPL_CYCLE_COLLECTION_TRACE_PRESERVED_WRAPPER
  NS_IMPL_CYCLE_COLLECTION_TRACE_JS_MEMBER_CALLBACK(mCachedKeyPath)
NS_IMPL_CYCLE_COLLECTION_TRACE_END

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN(IDBObjectStore)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mTransaction)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mIndexes)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mDeletedIndexes)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN(IDBObjectStore)
  NS_IMPL_CYCLE_COLLECTION_UNLINK_PRESERVED_WRAPPER

  // Don't unlink mTransaction!

  NS_IMPL_CYCLE_COLLECTION_UNLINK(mIndexes)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mDeletedIndexes)

  tmp->mCachedKeyPath.setUndefined();

  if (tmp->mRooted) {
    mozilla::DropJSObjects(tmp);
    tmp->mRooted = false;
  }
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(IDBObjectStore)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

NS_IMPL_CYCLE_COLLECTING_ADDREF(IDBObjectStore)
NS_IMPL_CYCLE_COLLECTING_RELEASE(IDBObjectStore)

JSObject* IDBObjectStore::WrapObject(JSContext* aCx,
                                     JS::Handle<JSObject*> aGivenProto) {
  return IDBObjectStore_Binding::Wrap(aCx, this, aGivenProto);
}

nsIGlobalObject* IDBObjectStore::GetParentObject() const {
  return mTransaction->GetParentObject();
}

void IDBObjectStore::GetKeyPath(JSContext* aCx,
                                JS::MutableHandle<JS::Value> aResult,
                                ErrorResult& aRv) {
  if (!mCachedKeyPath.isUndefined()) {
    aResult.set(mCachedKeyPath);
    return;
  }

  aRv = GetKeyPath().ToJSVal(aCx, mCachedKeyPath);
  if (NS_WARN_IF(aRv.Failed())) {
    return;
  }

  if (mCachedKeyPath.isGCThing()) {
    mozilla::HoldJSObjects(this);
    mRooted = true;
  }

  aResult.set(mCachedKeyPath);
}

RefPtr<DOMStringList> IDBObjectStore::IndexNames() {
  AssertIsOnOwningThread();

  return CreateSortedDOMStringList(
      mSpec->indexes(), [](const auto& index) { return index.name(); });
}

RefPtr<IDBRequest> IDBObjectStore::GetInternal(bool aKeyOnly, JSContext* aCx,
                                               JS::Handle<JS::Value> aKey,
                                               ErrorResult& aRv) {
  AssertIsOnOwningThread();

  if (mDeletedSpec) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_NOT_ALLOWED_ERR);
    return nullptr;
  }

  if (!mTransaction->IsActive()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  RefPtr<IDBKeyRange> keyRange;
  IDBKeyRange::FromJSVal(aCx, aKey, &keyRange, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  if (!keyRange) {
    // Must specify a key or keyRange for get().
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_KEY_ERR);
    return nullptr;
  }

  const int64_t id = Id();

  SerializedKeyRange serializedKeyRange;
  keyRange->ToSerialized(serializedKeyRange);

  const auto& params =
      aKeyOnly ? RequestParams{ObjectStoreGetKeyParams(id, serializedKeyRange)}
               : RequestParams{ObjectStoreGetParams(id, serializedKeyRange)};

  auto request = GenerateRequest(aCx, this);
  MOZ_ASSERT(request);

  IDB_LOG_MARK_CHILD_TRANSACTION_REQUEST(
      "database(%s).transaction(%s).objectStore(%s).get(%s)",
      "IDBObjectStore.get()", mTransaction->LoggingSerialNumber(),
      request->LoggingSerialNumber(),
      IDB_LOG_STRINGIFY(mTransaction->Database()),
      IDB_LOG_STRINGIFY(mTransaction), IDB_LOG_STRINGIFY(this),
      IDB_LOG_STRINGIFY(keyRange));

  // TODO: This is necessary to preserve request ordering only. Proper
  // sequencing of requests should be done in a more sophisticated manner that
  // doesn't require invalidating cursor caches (Bug 1580499).
  mTransaction->InvalidateCursorCaches();

  mTransaction->StartRequest(request, params);

  return request;
}

RefPtr<IDBRequest> IDBObjectStore::DeleteInternal(JSContext* aCx,
                                                  JS::Handle<JS::Value> aKey,
                                                  bool aFromCursor,
                                                  ErrorResult& aRv) {
  AssertIsOnOwningThread();

  if (mDeletedSpec) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_NOT_ALLOWED_ERR);
    return nullptr;
  }

  if (!mTransaction->IsActive()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  if (!mTransaction->IsWriteAllowed()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_READ_ONLY_ERR);
    return nullptr;
  }

  RefPtr<IDBKeyRange> keyRange;
  IDBKeyRange::FromJSVal(aCx, aKey, &keyRange, aRv);
  if (NS_WARN_IF((aRv.Failed()))) {
    return nullptr;
  }

  if (!keyRange) {
    // Must specify a key or keyRange for delete().
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_KEY_ERR);
    return nullptr;
  }

  ObjectStoreDeleteParams params;
  params.objectStoreId() = Id();
  keyRange->ToSerialized(params.keyRange());

  auto request = GenerateRequest(aCx, this);
  MOZ_ASSERT(request);

  if (!aFromCursor) {
    IDB_LOG_MARK_CHILD_TRANSACTION_REQUEST(
        "database(%s).transaction(%s).objectStore(%s).delete(%s)",
        "IDBObjectStore.delete()", mTransaction->LoggingSerialNumber(),
        request->LoggingSerialNumber(),
        IDB_LOG_STRINGIFY(mTransaction->Database()),
        IDB_LOG_STRINGIFY(mTransaction), IDB_LOG_STRINGIFY(this),
        IDB_LOG_STRINGIFY(keyRange));
  }

  mTransaction->StartRequest(request, params);

  mTransaction->InvalidateCursorCaches();

  return request;
}

RefPtr<IDBIndex> IDBObjectStore::CreateIndex(
    const nsAString& aName, const StringOrStringSequence& aKeyPath,
    const IDBIndexParameters& aOptionalParameters, ErrorResult& aRv) {
  AssertIsOnOwningThread();

  if (mTransaction->GetMode() != IDBTransaction::Mode::VersionChange ||
      mDeletedSpec) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_NOT_ALLOWED_ERR);
    return nullptr;
  }

  IDBTransaction* const transaction = IDBTransaction::GetCurrent();
  if (!transaction || transaction != mTransaction || !transaction->IsActive()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  const auto& indexes = mSpec->indexes();
  const auto end = indexes.cend();
  const auto foundIt = std::find_if(
      indexes.cbegin(), end,
      [&aName](const auto& index) { return aName == index.name(); });
  if (foundIt != end) {
    aRv.ThrowConstraintError(nsPrintfCString(
        "Index named '%s' already exists at index '%zu'",
        NS_ConvertUTF16toUTF8(aName).get(), foundIt.GetIndex()));
    return nullptr;
  }

  KeyPath keyPath(0);
  if (aKeyPath.IsString()) {
    if (NS_FAILED(KeyPath::Parse(aKeyPath.GetAsString(), &keyPath)) ||
        !keyPath.IsValid()) {
      aRv.Throw(NS_ERROR_DOM_SYNTAX_ERR);
      return nullptr;
    }
  } else {
    MOZ_ASSERT(aKeyPath.IsStringSequence());
    if (aKeyPath.GetAsStringSequence().IsEmpty() ||
        NS_FAILED(KeyPath::Parse(aKeyPath.GetAsStringSequence(), &keyPath)) ||
        !keyPath.IsValid()) {
      aRv.Throw(NS_ERROR_DOM_SYNTAX_ERR);
      return nullptr;
    }
  }

  if (aOptionalParameters.mMultiEntry && keyPath.IsArray()) {
    aRv.Throw(NS_ERROR_DOM_INVALID_ACCESS_ERR);
    return nullptr;
  }

#ifdef DEBUG
  {
    const auto duplicateIndexName = std::any_of(
        mIndexes.cbegin(), mIndexes.cend(),
        [&aName](const auto& index) { return index->Name() == aName; });
    MOZ_ASSERT(!duplicateIndexName);
  }
#endif

  const IndexMetadata* const oldMetadataElements =
      indexes.IsEmpty() ? nullptr : indexes.Elements();

  // With this setup we only validate the passed in locale name by the time we
  // get to encoding Keys. Maybe we should do it here right away and error out.

  // Valid locale names are always ASCII as per BCP-47.
  nsCString locale = NS_LossyConvertUTF16toASCII(aOptionalParameters.mLocale);
  bool autoLocale = locale.EqualsASCII("auto");
  if (autoLocale) {
    locale = IndexedDatabaseManager::GetLocale();
  }

  IndexMetadata* const metadata = mSpec->indexes().EmplaceBack(
      transaction->NextIndexId(), nsString(aName), keyPath, locale,
      aOptionalParameters.mUnique, aOptionalParameters.mMultiEntry, autoLocale);

  if (oldMetadataElements && oldMetadataElements != indexes.Elements()) {
    MOZ_ASSERT(indexes.Length() > 1);

    // Array got moved, update the spec pointers for all live indexes.
    RefreshSpec(/* aMayDelete */ false);
  }

  transaction->CreateIndex(this, *metadata);

  auto index = IDBIndex::Create(this, *metadata);

  mIndexes.AppendElement(index);

  // Don't do this in the macro because we always need to increment the serial
  // number to keep in sync with the parent.
  const uint64_t requestSerialNumber = IDBRequest::NextSerialNumber();

  IDB_LOG_MARK_CHILD_TRANSACTION_REQUEST(
      "database(%s).transaction(%s).objectStore(%s).createIndex(%s)",
      "IDBObjectStore.createIndex()", mTransaction->LoggingSerialNumber(),
      requestSerialNumber, IDB_LOG_STRINGIFY(mTransaction->Database()),
      IDB_LOG_STRINGIFY(mTransaction), IDB_LOG_STRINGIFY(this),
      IDB_LOG_STRINGIFY(index));

  return index;
}

void IDBObjectStore::DeleteIndex(const nsAString& aName, ErrorResult& aRv) {
  AssertIsOnOwningThread();

  if (mTransaction->GetMode() != IDBTransaction::Mode::VersionChange ||
      mDeletedSpec) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_NOT_ALLOWED_ERR);
    return;
  }

  IDBTransaction* transaction = IDBTransaction::GetCurrent();
  if (!transaction || transaction != mTransaction || !transaction->IsActive()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return;
  }

  const auto& metadataArray = mSpec->indexes();

  const auto endMetadata = metadataArray.cend();
  const auto foundMetadataIt = std::find_if(
      metadataArray.cbegin(), endMetadata,
      [&aName](const auto& metadata) { return aName == metadata.name(); });

  if (foundMetadataIt == endMetadata) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_NOT_FOUND_ERR);
    return;
  }

  const auto foundId = foundMetadataIt->id();
  MOZ_ASSERT(foundId);

  // Must remove index from mIndexes before altering the metadata array!
  {
    const auto end = mIndexes.end();
    const auto foundIt = std::find_if(
        mIndexes.begin(), end,
        [foundId](const auto& index) { return index->Id() == foundId; });
    // TODO: Or should we assert foundIt != end?
    if (foundIt != end) {
      auto& index = *foundIt;

      index->NoteDeletion();

      mDeletedIndexes.EmplaceBack(std::move(index));
      mIndexes.RemoveElementAt(foundIt.GetIndex());
    }
  }

  mSpec->indexes().RemoveElementAt(foundMetadataIt.GetIndex());

  RefreshSpec(/* aMayDelete */ false);

  // Don't do this in the macro because we always need to increment the serial
  // number to keep in sync with the parent.
  const uint64_t requestSerialNumber = IDBRequest::NextSerialNumber();

  IDB_LOG_MARK_CHILD_TRANSACTION_REQUEST(
      "database(%s).transaction(%s).objectStore(%s)."
      "deleteIndex(\"%s\")",
      "IDBObjectStore.deleteIndex()", mTransaction->LoggingSerialNumber(),
      requestSerialNumber, IDB_LOG_STRINGIFY(mTransaction->Database()),
      IDB_LOG_STRINGIFY(mTransaction), IDB_LOG_STRINGIFY(this),
      NS_ConvertUTF16toUTF8(aName).get());

  transaction->DeleteIndex(this, foundId);
}

RefPtr<IDBRequest> IDBObjectStore::Count(JSContext* aCx,
                                         JS::Handle<JS::Value> aKey,
                                         ErrorResult& aRv) {
  AssertIsOnOwningThread();

  if (mDeletedSpec) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_NOT_ALLOWED_ERR);
    return nullptr;
  }

  if (!mTransaction->IsActive()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  RefPtr<IDBKeyRange> keyRange;
  IDBKeyRange::FromJSVal(aCx, aKey, &keyRange, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  ObjectStoreCountParams params;
  params.objectStoreId() = Id();

  if (keyRange) {
    SerializedKeyRange serializedKeyRange;
    keyRange->ToSerialized(serializedKeyRange);
    params.optionalKeyRange().emplace(serializedKeyRange);
  }

  auto request = GenerateRequest(aCx, this);
  MOZ_ASSERT(request);

  IDB_LOG_MARK_CHILD_TRANSACTION_REQUEST(
      "database(%s).transaction(%s).objectStore(%s).count(%s)",
      "IDBObjectStore.count()", mTransaction->LoggingSerialNumber(),
      request->LoggingSerialNumber(),
      IDB_LOG_STRINGIFY(mTransaction->Database()),
      IDB_LOG_STRINGIFY(mTransaction), IDB_LOG_STRINGIFY(this),
      IDB_LOG_STRINGIFY(keyRange));

  // TODO: This is necessary to preserve request ordering only. Proper
  // sequencing of requests should be done in a more sophisticated manner that
  // doesn't require invalidating cursor caches (Bug 1580499).
  mTransaction->InvalidateCursorCaches();

  mTransaction->StartRequest(request, params);

  return request;
}

RefPtr<IDBRequest> IDBObjectStore::OpenCursorInternal(
    bool aKeysOnly, JSContext* aCx, JS::Handle<JS::Value> aRange,
    IDBCursorDirection aDirection, ErrorResult& aRv) {
  AssertIsOnOwningThread();
  MOZ_ASSERT(aCx);

  if (mDeletedSpec) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_NOT_ALLOWED_ERR);
    return nullptr;
  }

  if (!mTransaction->IsActive()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  RefPtr<IDBKeyRange> keyRange;
  IDBKeyRange::FromJSVal(aCx, aRange, &keyRange, aRv);
  if (NS_WARN_IF(aRv.Failed())) {
    return nullptr;
  }

  const int64_t objectStoreId = Id();

  Maybe<SerializedKeyRange> optionalKeyRange;

  if (keyRange) {
    SerializedKeyRange serializedKeyRange;
    keyRange->ToSerialized(serializedKeyRange);

    optionalKeyRange.emplace(std::move(serializedKeyRange));
  }

  const CommonOpenCursorParams commonParams = {
      objectStoreId, std::move(optionalKeyRange), aDirection};

  // TODO: It would be great if the IPDL generator created a constructor
  // accepting a CommonOpenCursorParams by value or rvalue reference.
  const auto params =
      aKeysOnly ? OpenCursorParams{ObjectStoreOpenKeyCursorParams{commonParams}}
                : OpenCursorParams{ObjectStoreOpenCursorParams{commonParams}};

  auto request = GenerateRequest(aCx, this);
  MOZ_ASSERT(request);

  if (aKeysOnly) {
    IDB_LOG_MARK_CHILD_TRANSACTION_REQUEST(
        "database(%s).transaction(%s).objectStore(%s)."
        "openKeyCursor(%s, %s)",
        "IDBObjectStore.openKeyCursor()", mTransaction->LoggingSerialNumber(),
        request->LoggingSerialNumber(),
        IDB_LOG_STRINGIFY(mTransaction->Database()),
        IDB_LOG_STRINGIFY(mTransaction), IDB_LOG_STRINGIFY(this),
        IDB_LOG_STRINGIFY(keyRange), IDB_LOG_STRINGIFY(aDirection));
  } else {
    IDB_LOG_MARK_CHILD_TRANSACTION_REQUEST(
        "database(%s).transaction(%s).objectStore(%s)."
        "openCursor(%s, %s)",
        "IDBObjectStore.openCursor()", mTransaction->LoggingSerialNumber(),
        request->LoggingSerialNumber(),
        IDB_LOG_STRINGIFY(mTransaction->Database()),
        IDB_LOG_STRINGIFY(mTransaction), IDB_LOG_STRINGIFY(this),
        IDB_LOG_STRINGIFY(keyRange), IDB_LOG_STRINGIFY(aDirection));
  }

  BackgroundCursorChildBase* const actor =
      aKeysOnly ? static_cast<BackgroundCursorChildBase*>(
                      new BackgroundCursorChild<IDBCursorType::ObjectStoreKey>(
                          request, this, aDirection))
                : new BackgroundCursorChild<IDBCursorType::ObjectStore>(
                      request, this, aDirection);

  // TODO: This is necessary to preserve request ordering only. Proper
  // sequencing of requests should be done in a more sophisticated manner that
  // doesn't require invalidating cursor caches (Bug 1580499).
  mTransaction->InvalidateCursorCaches();

  mTransaction->OpenCursor(actor, params);

  return request;
}

void IDBObjectStore::RefreshSpec(bool aMayDelete) {
  AssertIsOnOwningThread();
  MOZ_ASSERT_IF(mDeletedSpec, mSpec == mDeletedSpec.get());

  auto* const foundObjectStoreSpec =
      mTransaction->Database()->LookupModifiableObjectStoreSpec(
          [id = Id()](const auto& objSpec) {
            return objSpec.metadata().id() == id;
          });
  if (foundObjectStoreSpec) {
    mSpec = foundObjectStoreSpec;

    for (auto& index : mIndexes) {
      index->RefreshMetadata(aMayDelete);
    }

    for (auto& index : mDeletedIndexes) {
      index->RefreshMetadata(false);
    }
  }

  MOZ_ASSERT_IF(!aMayDelete && !mDeletedSpec, foundObjectStoreSpec);

  if (foundObjectStoreSpec) {
    MOZ_ASSERT(mSpec != mDeletedSpec.get());
    mDeletedSpec = nullptr;
  } else {
    NoteDeletion();
  }
}

const ObjectStoreSpec& IDBObjectStore::Spec() const {
  AssertIsOnOwningThread();
  MOZ_ASSERT(mSpec);

  return *mSpec;
}

void IDBObjectStore::NoteDeletion() {
  AssertIsOnOwningThread();
  MOZ_ASSERT(mSpec);
  MOZ_ASSERT(Id() == mSpec->metadata().id());

  if (mDeletedSpec) {
    MOZ_ASSERT(mDeletedSpec.get() == mSpec);
    return;
  }

  // Copy the spec here.
  mDeletedSpec = MakeUnique<ObjectStoreSpec>(*mSpec);
  mDeletedSpec->indexes().Clear();

  mSpec = mDeletedSpec.get();

  for (const auto& index : mIndexes) {
    index->NoteDeletion();
  }
}

const nsString& IDBObjectStore::Name() const {
  AssertIsOnOwningThread();
  MOZ_ASSERT(mSpec);

  return mSpec->metadata().name();
}

void IDBObjectStore::SetName(const nsAString& aName, ErrorResult& aRv) {
  AssertIsOnOwningThread();

  if (mTransaction->GetMode() != IDBTransaction::Mode::VersionChange ||
      mDeletedSpec) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return;
  }

  IDBTransaction* transaction = IDBTransaction::GetCurrent();
  if (!transaction || transaction != mTransaction || !transaction->IsActive()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return;
  }

  if (aName == mSpec->metadata().name()) {
    return;
  }

  // Cache logging string of this object store before renaming.
  const LoggingString loggingOldObjectStore(this);

  const nsresult rv =
      transaction->Database()->RenameObjectStore(mSpec->metadata().id(), aName);

  if (NS_FAILED(rv)) {
    aRv.Throw(rv);
    return;
  }

  // Don't do this in the macro because we always need to increment the serial
  // number to keep in sync with the parent.
  const uint64_t requestSerialNumber = IDBRequest::NextSerialNumber();

  IDB_LOG_MARK_CHILD_TRANSACTION_REQUEST(
      "database(%s).transaction(%s).objectStore(%s).rename(%s)",
      "IDBObjectStore.rename()", mTransaction->LoggingSerialNumber(),
      requestSerialNumber, IDB_LOG_STRINGIFY(mTransaction->Database()),
      IDB_LOG_STRINGIFY(mTransaction), loggingOldObjectStore.get(),
      IDB_LOG_STRINGIFY(this));

  transaction->RenameObjectStore(mSpec->metadata().id(), aName);
}

bool IDBObjectStore::AutoIncrement() const {
  AssertIsOnOwningThread();
  MOZ_ASSERT(mSpec);

  return mSpec->metadata().autoIncrement();
}

const indexedDB::KeyPath& IDBObjectStore::GetKeyPath() const {
  AssertIsOnOwningThread();
  MOZ_ASSERT(mSpec);

  return mSpec->metadata().keyPath();
}

bool IDBObjectStore::HasValidKeyPath() const {
  AssertIsOnOwningThread();
  MOZ_ASSERT(mSpec);

  return GetKeyPath().IsValid();
}

bool IDBObjectStore::ValueWrapper::Clone(JSContext* aCx) {
  if (mCloned) {
    return true;
  }

  static const JSStructuredCloneCallbacks callbacks = {
      CopyingStructuredCloneReadCallback /* read */,
      CopyingStructuredCloneWriteCallback /* write */,
      nullptr /* reportError */,
      nullptr /* readTransfer */,
      nullptr /* writeTransfer */,
      nullptr /* freeTransfer */,
      nullptr /* canTransfer */,
      nullptr /* sabCloned */
  };

  StructuredCloneInfo cloneInfo;

  JS::Rooted<JS::Value> clonedValue(aCx);
  if (!JS_StructuredClone(aCx, mValue, &clonedValue, &callbacks, &cloneInfo)) {
    return false;
  }

  mValue = clonedValue;

  mCloned = true;

  return true;
}

}  // namespace dom
}  // namespace mozilla
