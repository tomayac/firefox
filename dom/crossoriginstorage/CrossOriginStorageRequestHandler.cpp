/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CrossOriginStorageRequestHandler.h"

#include "CrossOriginStorageChild.h"
#include "CrossOriginStorageSinkAlgorithms.h"
#include "CrossOriginStorageWritableFileStream.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/dom/File.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/ScriptSettings.h"
#include "mozilla/ipc/BackgroundUtils.h"
#include "nsIGlobalObject.h"
#include "nsThreadUtils.h"

namespace mozilla::dom {

CrossOriginStorageRequestHandler::CrossOriginStorageRequestHandler(
    RefPtr<CrossOriginStorageChild> aActor, COSHashAlgorithm aAlgorithm,
    const nsACString& aValue)
    : mActor(std::move(aActor)), mAlgorithm(aAlgorithm), mValue(aValue) {}

void CrossOriginStorageRequestHandler::GetFile(
    RefPtr<FileSystemManager>& aManager,
    const fs::FileSystemEntryMetadata& aFile, RefPtr<Promise> aPromise,
    ErrorResult& aError) {
  if (!mActor || !mActor->CanSend()) {
    aPromise->MaybeRejectWithNotFoundError(
        "Cross-Origin Storage is not available");
    return;
  }

  nsCOMPtr<nsIGlobalObject> global = aPromise->GetGlobalObject();
  nsCString algorithm(CanonicalHashAlgorithmName(mAlgorithm));
  nsCString value(mValue);

  mActor->SendGetFileBytes(algorithm, mValue)
      ->Then(GetCurrentSerialEventTarget(), __func__,
             [promise = aPromise, global,
              value](const CrossOriginStorageChild::GetFileBytesPromise::
                         ResolveOrRejectValue& aResult) {
               if (!aResult.IsResolve()) {
                 promise->MaybeRejectWithUnknownError(
                     "Cross-Origin Storage IPC error");
                 return;
               }
               const COSGetFileBytesResult& result = aResult.ResolveValue();
               if (result.type() == COSGetFileBytesResult::Tnsresult) {
                 promise->MaybeReject(result.get_nsresult());
                 return;
               }

               const nsTArray<uint8_t>& bytes = result.get_ArrayOfuint8_t();
               void* buffer = malloc(bytes.Length());
               if (!buffer) {
                 promise->MaybeReject(NS_ERROR_OUT_OF_MEMORY);
                 return;
               }
               memcpy(buffer, bytes.Elements(), bytes.Length());

               RefPtr<File> file = File::CreateMemoryFileWithLastModifiedNow(
                   global, buffer, bytes.Length(), NS_ConvertUTF8toUTF16(value),
                   u""_ns);
               promise->MaybeResolve(file);
             });
}

void CrossOriginStorageRequestHandler::GetWritable(
    RefPtr<FileSystemManager>& aManager,
    const fs::FileSystemEntryMetadata& aFile, bool aKeepData,
    const RefPtr<Promise>& aPromise, ErrorResult& aError) {
  if (!mActor || !mActor->CanSend()) {
    aPromise->MaybeRejectWithNotFoundError(
        "Cross-Origin Storage is not available");
    return;
  }

  uint64_t writeId = mActor->NextWriteId();

  nsCOMPtr<nsIGlobalObject> global = aPromise->GetGlobalObject();
  nsIPrincipal* principal = global->PrincipalOrNull();
  mozilla::ipc::PrincipalInfo writingPrincipal;
  if (!principal ||
      NS_FAILED(PrincipalToPrincipalInfo(principal, &writingPrincipal))) {
    aPromise->MaybeRejectWithNotAllowedError(
        "Cross-Origin Storage requires a principal");
    return;
  }

  nsCString algorithm(CanonicalHashAlgorithmName(mAlgorithm));
  (void)mActor->SendBeginWrite(writeId, algorithm, mValue, writingPrincipal);

  AutoJSAPI jsapi;
  if (!jsapi.Init(global)) {
    aPromise->MaybeRejectWithUnknownError("Internal error");
    return;
  }
  JSContext* cx = jsapi.cx();

  auto algorithms =
      MakeRefPtr<CrossOriginStorageSinkAlgorithms>(global, mActor, writeId);
  ErrorResult rv;
  RefPtr<CrossOriginStorageWritableFileStream> stream =
      CrossOriginStorageWritableFileStream::Create(cx, global, *algorithms, rv);
  if (rv.Failed()) {
    aPromise->MaybeReject(std::move(rv));
    return;
  }

  aPromise->MaybeResolve(stream);
}

}  // namespace mozilla::dom
