/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_CrossOriginStorageChild_h
#define mozilla_dom_CrossOriginStorageChild_h

#include "mozilla/dom/PCrossOriginStorageChild.h"
#include "nsISupportsImpl.h"

namespace mozilla::dom {

// Per-thread (main thread, or a single worker thread) actor for
// navigator.crossOriginStorage, held by the CrossOriginStorageManager for
// that global. See CrossOriginStorageRegistry.h for the parent-side registry
// this talks to.
class CrossOriginStorageChild final : public PCrossOriginStorageChild {
  friend class PCrossOriginStorageChild;

 public:
  NS_INLINE_DECL_REFCOUNTING(CrossOriginStorageChild)

  CrossOriginStorageChild() = default;

  // A fresh, per-actor-only token to correlate a BeginWrite/WriteChunk*/
  // FinishWrite(or AbortWrite) sequence for one write session. Not
  // spec-visible; see PCrossOriginStorage.ipdl.
  uint64_t NextWriteId() { return ++mLastWriteId; }

 private:
  ~CrossOriginStorageChild() = default;

  uint64_t mLastWriteId = 0;
};

}  // namespace mozilla::dom

#endif  // mozilla_dom_CrossOriginStorageChild_h
