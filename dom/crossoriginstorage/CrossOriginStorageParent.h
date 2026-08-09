/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_CrossOriginStorageParent_h
#define mozilla_dom_CrossOriginStorageParent_h

#include "CrossOriginStorageUtils.h"
#include "mozilla/dom/PCrossOriginStorageParent.h"
#include "nsClassHashtable.h"
#include "nsISupportsImpl.h"
#include "nsTArray.h"

namespace mozilla::dom {

// Parent-side actor for navigator.crossOriginStorage; one per content
// process. Recv* handlers below drive CrossOriginStorageRegistry, the
// actual shared registry -- this class only additionally owns the
// per-actor bookkeeping for write sessions currently in flight from its
// own content process (keyed by the child-generated `writeId`; see
// PCrossOriginStorage.ipdl).
class CrossOriginStorageParent final : public PCrossOriginStorageParent {
  friend class PCrossOriginStorageParent;

 public:
  NS_INLINE_DECL_REFCOUNTING(CrossOriginStorageParent)

  CrossOriginStorageParent() = default;

 private:
  ~CrossOriginStorageParent() = default;

  // If this content process disappears (crash, or ordinary shutdown) while
  // it still has write sessions in flight, release their outstanding-
  // writer counts immediately rather than leaving them to the registry's
  // staleness timeout -- the same cleanup an explicit AbortWrite would
  // have done.
  void ActorDestroy(ActorDestroyReason aWhy) override;

  mozilla::ipc::IPCResult RecvRequestFileHandle(
      const nsCString& aAlgorithm, const nsCString& aValue, bool aCreate,
      const mozilla::ipc::PrincipalInfo& aRequestingPrincipal,
      RequestFileHandleResolver&& aResolve);

  mozilla::ipc::IPCResult RecvBeginWrite(
      uint64_t aWriteId, const nsCString& aAlgorithm, const nsCString& aValue,
      const mozilla::ipc::PrincipalInfo& aWritingPrincipal);

  mozilla::ipc::IPCResult RecvWriteChunk(uint64_t aWriteId,
                                         nsTArray<uint8_t>&& aChunk);

  mozilla::ipc::IPCResult RecvFinishWrite(uint64_t aWriteId,
                                          FinishWriteResolver&& aResolve);

  mozilla::ipc::IPCResult RecvAbortWrite(uint64_t aWriteId);

  mozilla::ipc::IPCResult RecvGetFileBytes(const nsCString& aAlgorithm,
                                           const nsCString& aValue,
                                           GetFileBytesResolver&& aResolve);

  struct WriteSession {
    COSHashAlgorithm mAlgorithm;
    nsCString mValue;
    mozilla::ipc::PrincipalInfo mWritingPrincipal;
    nsTArray<uint8_t> mBytes;
  };

  nsClassHashtable<nsUint64HashKey, WriteSession> mWriteSessions;
};

}  // namespace mozilla::dom

#endif  // mozilla_dom_CrossOriginStorageParent_h
