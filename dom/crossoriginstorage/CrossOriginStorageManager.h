/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_CrossOriginStorageManager_h
#define mozilla_dom_CrossOriginStorageManager_h

#include "js/TypeDecls.h"
#include "mozilla/AlreadyAddRefed.h"
#include "nsCOMPtr.h"
#include "nsCycleCollectionParticipant.h"
#include "nsISupports.h"
#include "nsWrapperCache.h"

class JSObject;
class nsIGlobalObject;
struct JSContext;

namespace mozilla {
class ErrorResult;

namespace dom {

class Promise;
struct CrossOriginStorageRequestFileHandleHash;
struct CrossOriginStorageRequestFileHandleOptions;

// https://wicg.github.io/cross-origin-storage/#the-crossoriginstoragemanager-interface
class CrossOriginStorageManager final : public nsISupports,
                                        public nsWrapperCache {
  nsCOMPtr<nsIGlobalObject> mGlobal;

 public:
  explicit CrossOriginStorageManager(nsIGlobalObject* aGlobal);

  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_WRAPPERCACHE_CLASS(CrossOriginStorageManager)

  // WebIDL Boilerplate
  nsIGlobalObject* GetParentObject() const { return mGlobal; }

  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;

  // WebIDL Interface
  already_AddRefed<Promise> RequestFileHandle(
      const CrossOriginStorageRequestFileHandleHash& aHash,
      const CrossOriginStorageRequestFileHandleOptions& aOptions,
      ErrorResult& aRv);

 private:
  ~CrossOriginStorageManager() = default;
};

}  // namespace dom
}  // namespace mozilla

#endif  // mozilla_dom_CrossOriginStorageManager_h
