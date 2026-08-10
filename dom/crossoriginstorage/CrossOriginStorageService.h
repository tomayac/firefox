/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_CrossOriginStorageService_h
#define mozilla_dom_CrossOriginStorageService_h

#include "mozilla/MozPromise.h"
#include "nsICrossOriginStorageService.h"

namespace mozilla::dom {

// See nsICrossOriginStorageService.idl. A plain, stateless XPCOM service
// (all real state lives in CrossOriginStorageRegistry, on the
// PBackground thread) -- getService() callers all share the one XPCOM
// component-manager-cached instance, but that's incidental, not
// load-bearing; nothing here depends on being a singleton.
class CrossOriginStorageService final : public nsICrossOriginStorageService {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSICROSSORIGINSTORAGESERVICE

  CrossOriginStorageService() = default;

 private:
  ~CrossOriginStorageService() = default;

  // Resolves once the PBackground-thread operation has run, or
  // immediately if the background thread was never started (nothing was
  // ever stored, so there's nothing to clear).
  using ClearPromise = MozPromise<bool, nsresult, true>;
  static RefPtr<ClearPromise> ClearAllAsync();
  static RefPtr<ClearPromise> ClearBySiteAsync(
      const nsACString& aSchemelessSite);
};

}  // namespace mozilla::dom

#endif  // mozilla_dom_CrossOriginStorageService_h
