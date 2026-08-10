/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CrossOriginStorageService.h"

#include "CrossOriginStorageRegistry.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/ipc/BackgroundParent.h"
#include "nsIGlobalObject.h"
#include "nsThreadUtils.h"
#include "xpcpublic.h"

namespace mozilla::dom {

NS_IMPL_ISUPPORTS(CrossOriginStorageService, nsICrossOriginStorageService)

/* static */
RefPtr<CrossOriginStorageService::ClearPromise>
CrossOriginStorageService::ClearAllAsync() {
  MOZ_ASSERT(NS_IsMainThread());

  nsCOMPtr<nsISerialEventTarget> backgroundThread =
      mozilla::ipc::BackgroundParent::GetBackgroundThread();
  if (!backgroundThread) {
    return ClearPromise::CreateAndResolve(true, __func__);
  }

  RefPtr<ClearPromise::Private> promise = new ClearPromise::Private(__func__);
  nsCOMPtr<nsIRunnable> runnable = NS_NewRunnableFunction(
      "CrossOriginStorageService::ClearAllAsync", [promise]() {
        CrossOriginStorageRegistry::GetOrCreate().ClearAll();
        promise->Resolve(true, __func__);
      });
  backgroundThread->Dispatch(runnable.forget());
  return promise;
}

/* static */
RefPtr<CrossOriginStorageService::ClearPromise>
CrossOriginStorageService::ClearBySiteAsync(const nsACString& aSchemelessSite) {
  MOZ_ASSERT(NS_IsMainThread());

  nsCOMPtr<nsISerialEventTarget> backgroundThread =
      mozilla::ipc::BackgroundParent::GetBackgroundThread();
  if (!backgroundThread) {
    return ClearPromise::CreateAndResolve(true, __func__);
  }

  RefPtr<ClearPromise::Private> promise = new ClearPromise::Private(__func__);
  nsCString site(aSchemelessSite);
  nsCOMPtr<nsIRunnable> runnable = NS_NewRunnableFunction(
      "CrossOriginStorageService::ClearBySiteAsync", [promise, site]() {
        CrossOriginStorageRegistry::GetOrCreate().RemoveSite(site);
        promise->Resolve(true, __func__);
      });
  backgroundThread->Dispatch(runnable.forget());
  return promise;
}

NS_IMETHODIMP
CrossOriginStorageService::Clear(JSContext* aCx, Promise** aResult) {
  MOZ_ASSERT(NS_IsMainThread());

  nsIGlobalObject* global = xpc::CurrentNativeGlobal(aCx);
  MOZ_ASSERT(global);

  ErrorResult rv;
  RefPtr<Promise> promise = Promise::Create(global, rv);
  if (NS_WARN_IF(rv.Failed())) {
    return rv.StealNSResult();
  }

  ClearAllAsync()->Then(
      GetMainThreadSerialEventTarget(), __func__,
      [promise](bool) { promise->MaybeResolveWithUndefined(); },
      [promise](nsresult aRv) { promise->MaybeReject(aRv); });

  promise.forget(aResult);
  return NS_OK;
}

NS_IMETHODIMP
CrossOriginStorageService::ClearBySite(const nsACString& aSchemelessSite,
                                       JSContext* aCx, Promise** aResult) {
  MOZ_ASSERT(NS_IsMainThread());

  nsIGlobalObject* global = xpc::CurrentNativeGlobal(aCx);
  MOZ_ASSERT(global);

  ErrorResult rv;
  RefPtr<Promise> promise = Promise::Create(global, rv);
  if (NS_WARN_IF(rv.Failed())) {
    return rv.StealNSResult();
  }

  ClearBySiteAsync(aSchemelessSite)
      ->Then(
          GetMainThreadSerialEventTarget(), __func__,
          [promise](bool) { promise->MaybeResolveWithUndefined(); },
          [promise](nsresult aRv) { promise->MaybeReject(aRv); });

  promise.forget(aResult);
  return NS_OK;
}

}  // namespace mozilla::dom
