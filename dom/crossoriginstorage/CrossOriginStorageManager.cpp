/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CrossOriginStorageManager.h"

#include "mozilla/ErrorResult.h"
#include "mozilla/dom/CrossOriginStorageBinding.h"
#include "mozilla/dom/Promise.h"
#include "nsIGlobalObject.h"

namespace mozilla::dom {

CrossOriginStorageManager::CrossOriginStorageManager(nsIGlobalObject* aGlobal)
    : mGlobal(aGlobal) {
  MOZ_ASSERT(aGlobal);
}

NS_IMPL_CYCLE_COLLECTING_ADDREF(CrossOriginStorageManager)
NS_IMPL_CYCLE_COLLECTING_RELEASE(CrossOriginStorageManager)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(CrossOriginStorageManager)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE_CLASS(CrossOriginStorageManager)
NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN(CrossOriginStorageManager)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mGlobal)
  NS_IMPL_CYCLE_COLLECTION_UNLINK_PRESERVED_WRAPPER
NS_IMPL_CYCLE_COLLECTION_UNLINK_END
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN(CrossOriginStorageManager)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mGlobal)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

// WebIDL Boilerplate

JSObject* CrossOriginStorageManager::WrapObject(
    JSContext* aCx, JS::Handle<JSObject*> aGivenProto) {
  return CrossOriginStorageManager_Binding::Wrap(aCx, this, aGivenProto);
}

// WebIDL Interface

already_AddRefed<Promise> CrossOriginStorageManager::RequestFileHandle(
    const CrossOriginStorageRequestFileHandleHash& aHash,
    const CrossOriginStorageRequestFileHandleOptions& aOptions,
    ErrorResult& aRv) {
  // TODO(Bug TBD): request validation, Permissions Policy gating, and the
  // actual read/create request algorithms land in follow-up patches. See
  // https://wicg.github.io/cross-origin-storage/#the-crossoriginstoragemanager-requestfilehandle-method
  aRv.Throw(NS_ERROR_NOT_IMPLEMENTED);
  return nullptr;
}

}  // namespace mozilla::dom
