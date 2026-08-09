/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CrossOriginStorageSinkAlgorithms.h"

#include "CrossOriginStorageChild.h"
#include "mozilla/dom/BindingUtils.h"
#include "mozilla/dom/Blob.h"
#include "mozilla/dom/BlobBinding.h"
#include "mozilla/dom/Promise.h"
#include "nsIGlobalObject.h"
#include "nsJSUtils.h"
#include "nsNetUtil.h"

namespace mozilla::dom {

NS_IMPL_ISUPPORTS_CYCLE_COLLECTION_INHERITED_0(CrossOriginStorageSinkAlgorithms,
                                               UnderlyingSinkAlgorithmsBase)
NS_IMPL_CYCLE_COLLECTION_INHERITED(CrossOriginStorageSinkAlgorithms,
                                   UnderlyingSinkAlgorithmsBase, mGlobal)

CrossOriginStorageSinkAlgorithms::CrossOriginStorageSinkAlgorithms(
    nsIGlobalObject* aGlobal, RefPtr<CrossOriginStorageChild> aActor,
    uint64_t aWriteId)
    : mGlobal(aGlobal), mActor(std::move(aActor)), mWriteId(aWriteId) {}

already_AddRefed<Promise> CrossOriginStorageSinkAlgorithms::WriteCallbackImpl(
    JSContext* aCx, JS::Handle<JS::Value> aChunk,
    WritableStreamDefaultController& aController, ErrorResult& aRv) {
  RefPtr<Promise> promise = Promise::Create(mGlobal, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  if (!mActor || !mActor->CanSend()) {
    promise->MaybeRejectWithInvalidStateError(
        "Cross-Origin Storage is not available");
    return promise.forget();
  }

  nsTArray<uint8_t> bytes;

  RefPtr<Blob> blob;
  if (aChunk.isObject() &&
      NS_SUCCEEDED(UNWRAP_OBJECT(Blob, &aChunk.toObject(), blob))) {
    nsCOMPtr<nsIInputStream> stream;
    ErrorResult rv;
    blob->CreateInputStream(getter_AddRefs(stream), rv);
    if (rv.Failed()) {
      promise->MaybeReject(rv.StealNSResult());
      return promise.forget();
    }
    nsCString data;
    nsresult drv = NS_ReadInputStreamToString(stream, data, -1);
    if (NS_FAILED(drv)) {
      promise->MaybeReject(drv);
      return promise.forget();
    }
    bytes.AppendElements(reinterpret_cast<const uint8_t*>(data.get()),
                         data.Length());
  } else if (aChunk.isString()) {
    nsAutoJSString str;
    if (!str.init(aCx, aChunk)) {
      promise->MaybeRejectWithTypeError("Invalid chunk");
      return promise.forget();
    }
    NS_ConvertUTF16toUTF8 utf8(str);
    bytes.AppendElements(reinterpret_cast<const uint8_t*>(utf8.get()),
                         utf8.Length());
  } else {
    // Phase 1 limitation: every write() call in this feature's WPT suite
    // passes a Blob or a string; ArrayBuffer/ArrayBufferView chunks (which
    // the real FileSystemWritableFileStream.write() also accepts) are
    // follow-up work.
    promise->MaybeRejectWithTypeError(
        "Unsupported chunk type: expected a Blob or a string");
    return promise.forget();
  }

  (void)mActor->SendWriteChunk(mWriteId, std::move(bytes));
  promise->MaybeResolveWithUndefined();
  return promise.forget();
}

already_AddRefed<Promise> CrossOriginStorageSinkAlgorithms::CloseCallbackImpl(
    JSContext* aCx, ErrorResult& aRv) {
  RefPtr<Promise> promise = Promise::Create(mGlobal, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  if (!mActor || !mActor->CanSend()) {
    promise->MaybeRejectWithInvalidStateError(
        "Cross-Origin Storage is not available");
    return promise.forget();
  }

  // https://wicg.github.io/cross-origin-storage/#verify-and-store
  mActor->SendFinishWrite(mWriteId)->Then(
      GetCurrentSerialEventTarget(), __func__,
      [promise](const CrossOriginStorageChild::FinishWritePromise::
                    ResolveOrRejectValue& aResult) {
        if (!aResult.IsResolve()) {
          promise->MaybeRejectWithUnknownError(
              "Cross-Origin Storage IPC error");
          return;
        }
        nsresult rv = aResult.ResolveValue();
        if (NS_FAILED(rv)) {
          promise->MaybeReject(rv);
          return;
        }
        promise->MaybeResolveWithUndefined();
      });
  return promise.forget();
}

already_AddRefed<Promise> CrossOriginStorageSinkAlgorithms::AbortCallbackImpl(
    JSContext* aCx, const Optional<JS::Handle<JS::Value>>& aReason,
    ErrorResult& aRv) {
  if (mActor && mActor->CanSend()) {
    (void)mActor->SendAbortWrite(mWriteId);
  }
  // Synchronous: the base wrapper treats a null return as an immediately
  // resolved abort algorithm.
  return nullptr;
}

}  // namespace mozilla::dom
