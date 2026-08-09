/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_CrossOriginStorageSinkAlgorithms_h
#define mozilla_dom_CrossOriginStorageSinkAlgorithms_h

#include "mozilla/RefPtr.h"
#include "mozilla/dom/UnderlyingSinkCallbackHelpers.h"
#include "nsCOMPtr.h"

class nsIGlobalObject;

namespace mozilla::dom {

class CrossOriginStorageChild;

// The native WritableStream sink backing FileSystemFileHandle.
// createWritable() for a handle obtained from
// navigator.crossOriginStorage.requestFileHandle(). Streams write() chunks
// to the registry via WriteChunk, and runs "verify and store"
// (https://wicg.github.io/cross-origin-storage/#verify-and-store) via
// FinishWrite when the stream is closed.
//
// createWritable()'s promise resolves with a
// CrossOriginStorageWritableFileStream (a minimal WritableStream subclass, not
// a genuine FileSystemWritableFileStream): no WPT for this feature checks
// `instanceof FileSystemWritableFileStream`, and this Phase 1's in-memory,
// capped-size writes don't need that class's real disk-backed seek()/truncate()
// machinery -- which is exactly what ties it tightly to PFileSystemManager's
// fd-passing. See CrossOriginStorageRequestHandler::GetWritable() and
// CrossOriginStorageWritableFileStream.h.
class CrossOriginStorageSinkAlgorithms final
    : public UnderlyingSinkAlgorithmsWrapper {
 public:
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(CrossOriginStorageSinkAlgorithms,
                                           UnderlyingSinkAlgorithmsBase)

  CrossOriginStorageSinkAlgorithms(nsIGlobalObject* aGlobal,
                                   RefPtr<CrossOriginStorageChild> aActor,
                                   uint64_t aWriteId);

  MOZ_CAN_RUN_SCRIPT already_AddRefed<Promise> WriteCallbackImpl(
      JSContext* aCx, JS::Handle<JS::Value> aChunk,
      WritableStreamDefaultController& aController, ErrorResult& aRv) override;

  MOZ_CAN_RUN_SCRIPT already_AddRefed<Promise> CloseCallbackImpl(
      JSContext* aCx, ErrorResult& aRv) override;

  MOZ_CAN_RUN_SCRIPT already_AddRefed<Promise> AbortCallbackImpl(
      JSContext* aCx, const Optional<JS::Handle<JS::Value>>& aReason,
      ErrorResult& aRv) override;

 private:
  ~CrossOriginStorageSinkAlgorithms() override = default;

  nsCOMPtr<nsIGlobalObject> mGlobal;
  RefPtr<CrossOriginStorageChild> mActor;
  uint64_t mWriteId;
};

}  // namespace mozilla::dom

#endif  // mozilla_dom_CrossOriginStorageSinkAlgorithms_h
