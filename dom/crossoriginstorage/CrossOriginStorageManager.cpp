/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CrossOriginStorageManager.h"

#include "CrossOriginStorageChild.h"
#include "CrossOriginStorageRequestHandler.h"
#include "CrossOriginStorageUtils.h"
#include "mozilla/BasePrincipal.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/dom/CrossOriginStorageBinding.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/FeaturePolicyUtils.h"
#include "mozilla/dom/FileSystemFileHandle.h"
#include "mozilla/dom/FileSystemManager.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/ipc/BackgroundChild.h"
#include "mozilla/ipc/BackgroundUtils.h"
#include "mozilla/ipc/PBackgroundChild.h"
#include "nsContentUtils.h"
#include "nsIGlobalObject.h"
#include "nsIURI.h"
#include "nsNetUtil.h"
#include "nsPIDOMWindow.h"

namespace mozilla::dom {

namespace {

bool IsLowercaseHex(const nsAString& aValue) {
  for (uint32_t i = 0; i < aValue.Length(); ++i) {
    char16_t c = aValue.CharAt(i);
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

// https://wicg.github.io/cross-origin-storage/#validate-a-cos-request
// Returns the parsed algorithm on success, or Nothing() with aRv set to a
// TypeError on failure.
Maybe<COSHashAlgorithm> ValidateRequest(
    const CrossOriginStorageRequestFileHandleHash& aHash,
    const CrossOriginStorageRequestFileHandleOptions& aOptions,
    ErrorResult& aRv) {
  // Step 1.
  Maybe<COSHashAlgorithm> algorithm = ParseHashAlgorithm(aHash.mAlgorithm);
  if (!algorithm) {
    aRv.ThrowTypeError(
        "algorithm is not a hash algorithm name recognized by the Web "
        "Crypto API");
    return Nothing();
  }

  // Step 2.
  if (aHash.mValue.Length() != ExpectedHexValueLength(*algorithm) ||
      !IsLowercaseHex(aHash.mValue)) {
    aRv.ThrowTypeError(
        "value is not a lowercase hexadecimal digest of the length "
        "expected for the given algorithm");
    return Nothing();
  }

  // Step 3: if options["origins"] doesn't exist, there is nothing further
  // to validate.
  if (!aOptions.mOrigins.WasPassed()) {
    return algorithm;
  }
  const auto& origins = aOptions.mOrigins.Value();
  bool isWildcard =
      origins.IsString() && origins.GetAsString().EqualsLiteral("*");

  if (!isWildcard) {
    // Step 3.1: a single string is treated as a list of one.
    nsTArray<nsString> candidates;
    if (origins.IsString()) {
      candidates.AppendElement(origins.GetAsString());
    } else {
      candidates.AppendElements(origins.GetAsStringSequence());
    }

    // Step 3.2.
    if (candidates.Length() > kMaxOriginsListLength) {
      aRv.ThrowTypeError("origins list exceeds the maximum supported length");
      return Nothing();
    }

    // Step 3.3: for each candidate, it must parse as a URL, and the parsed
    // URL's origin must not be opaque.
    for (const auto& candidate : candidates) {
      nsCOMPtr<nsIURI> uri;
      if (NS_FAILED(NS_NewURI(getter_AddRefs(uri), candidate))) {
        aRv.ThrowTypeError("origins entry does not parse as a URL");
        return Nothing();
      }

      nsCOMPtr<nsIPrincipal> principal =
          BasePrincipal::CreateContentPrincipal(uri, OriginAttributes());
      if (principal->GetIsNullPrincipal()) {
        aRv.ThrowTypeError("origins entry parses to an opaque origin");
        return Nothing();
      }
    }
  }

  return algorithm;
}

// https://wicg.github.io/cross-origin-storage/#normalize-requested-origins
// Only called once ValidateRequest has already confirmed aOptions.mOrigins
// (if present) shape-validates, so the URL-parse/opaque-origin checks below
// cannot fail here.
COSRequestedOriginsValue NormalizeRequestedOrigins(
    const CrossOriginStorageRequestFileHandleOptions& aOptions) {
  COSRequestedOriginsValue result;
  if (!aOptions.mOrigins.WasPassed()) {
    return result;
  }
  const auto& origins = aOptions.mOrigins.Value();
  if (origins.IsString() && origins.GetAsString().EqualsLiteral("*")) {
    result.mKind = COSRequestedOriginsValue::Kind::Wildcard;
    return result;
  }

  nsTArray<nsString> candidates;
  if (origins.IsString()) {
    candidates.AppendElement(origins.GetAsString());
  } else {
    candidates.AppendElements(origins.GetAsStringSequence());
  }

  result.mKind = COSRequestedOriginsValue::Kind::List;
  for (const auto& candidate : candidates) {
    nsCOMPtr<nsIURI> uri;
    if (NS_FAILED(NS_NewURI(getter_AddRefs(uri), candidate))) {
      continue;
    }
    nsCOMPtr<nsIPrincipal> principal =
        BasePrincipal::CreateContentPrincipal(uri, OriginAttributes());
    nsAutoCString origin;
    if (NS_FAILED(principal->GetOrigin(origin))) {
      continue;
    }
    if (!result.mList.Contains(origin)) {
      result.mList.AppendElement(origin);
    }
  }
  return result;
}

// If aGlobal is a Window, returns its Document, if any. Worker globals have
// no single Document reliably reachable this way; per the "if any" in the
// spec's requestFileHandle() step 5, such globals simply skip the
// Permissions Policy check below.
Document* GetAssociatedDocument(nsIGlobalObject* aGlobal) {
  if (nsPIDOMWindowInner* window = aGlobal->GetAsInnerWindow()) {
    return window->GetExtantDoc();
  }
  return nullptr;
}

}  // namespace

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

// https://wicg.github.io/cross-origin-storage/#the-crossoriginstoragemanager-requestfilehandle-method
already_AddRefed<Promise> CrossOriginStorageManager::RequestFileHandle(
    const CrossOriginStorageRequestFileHandleHash& aHash,
    const CrossOriginStorageRequestFileHandleOptions& aOptions,
    ErrorResult& aRv) {
  // Step 1.
  RefPtr<Promise> promise = Promise::Create(mGlobal, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  // Steps 2-4 (realm, global, origin) are implicit: mGlobal is `this`'s
  // relevant global object, and the requesting origin is derived later,
  // from mGlobal, wherever it's actually needed.

  // Step 5. MaybeReject* below is this implementation's way of running
  // "queue a global task ... to reject |result|": a Promise rejection is
  // never observable by script synchronously regardless of how it's
  // triggered, so a direct call here is spec-equivalent.
  if (Document* doc = GetAssociatedDocument(mGlobal)) {
    if (!FeaturePolicyUtils::IsFeatureAllowed(doc,
                                              u"cross-origin-storage"_ns)) {
      promise->MaybeRejectWithNotAllowedError(
          "Cross-Origin Storage is disabled by Permissions Policy in this "
          "context");
      return promise.forget();
    }
  }

  // Steps 6-7.
  ErrorResult validationError;
  Maybe<COSHashAlgorithm> algorithm =
      ValidateRequest(aHash, aOptions, validationError);
  if (validationError.Failed()) {
    MOZ_ASSERT(algorithm.isNothing());
    promise->MaybeReject(std::move(validationError));
    return promise.forget();
  }
  MOZ_ASSERT(algorithm.isSome());

  CrossOriginStorageChild* actor = EnsureActor();
  if (!actor) {
    promise->MaybeRejectWithUnknownError(
        "Cross-Origin Storage is not available");
    return promise.forget();
  }

  nsIPrincipal* principal = mGlobal->PrincipalOrNull();
  mozilla::ipc::PrincipalInfo principalInfo;
  if (!principal ||
      NS_FAILED(PrincipalToPrincipalInfo(principal, &principalInfo))) {
    promise->MaybeRejectWithNotAllowedError(
        "Cross-Origin Storage requires a principal");
    return promise.forget();
  }

  nsAutoCString value(NS_ConvertUTF16toUTF8(aHash.mValue));
  nsAutoCString algorithmName(CanonicalHashAlgorithmName(*algorithm));
  bool create = aOptions.mCreate;
  COSRequestedOriginsValue requestedOrigins =
      NormalizeRequestedOrigins(aOptions);

  // Step 8.
  actor->SendRequestFileHandle(algorithmName, value, create, principalInfo)
      ->Then(
          GetCurrentSerialEventTarget(), __func__,
          [promise, global = mGlobal, actor = RefPtr(actor),
           algorithm = *algorithm, value, requestedOrigins](
              const CrossOriginStorageChild::RequestFileHandlePromise::
                  ResolveOrRejectValue& aResult) {
            if (!aResult.IsResolve()) {
              promise->MaybeRejectWithUnknownError(
                  "Cross-Origin Storage IPC error");
              return;
            }
            const COSRequestFileHandleResult& result = aResult.ResolveValue();
            if (result.type() == COSRequestFileHandleResult::Tnsresult) {
              promise->MaybeReject(result.get_nsresult());
              return;
            }

            // https://wicg.github.io/cross-origin-storage/#complete-a-read-request
            // and #complete-a-create-request: "creating a new
            // FileSystemFileHandle" whose locator addresses the entry.
            // entryId only needs to be non-empty and stable for this
            // handle; the request handler below carries the actual
            // algorithm/value it acts on directly, not via this locator.
            fs::FileSystemEntryMetadata metadata(
                value, NS_ConvertUTF8toUTF16(value), /* directory */ false);
            RefPtr<FileSystemManager> nullManager;
            auto requestHandler = MakeUnique<CrossOriginStorageRequestHandler>(
                actor, algorithm, value, requestedOrigins);
            RefPtr<FileSystemFileHandle> handle = new FileSystemFileHandle(
                global, nullManager, metadata, requestHandler.release());
            promise->MaybeResolve(handle);
          });

  return promise.forget();
}

CrossOriginStorageChild* CrossOriginStorageManager::EnsureActor() {
  if (mActor && mActor->CanSend()) {
    return mActor;
  }

  mozilla::ipc::PBackgroundChild* background =
      mozilla::ipc::BackgroundChild::GetOrCreateForCurrentThread();
  if (!background) {
    return nullptr;
  }

  mActor = new CrossOriginStorageChild();
  if (!background->SendPCrossOriginStorageConstructor(mActor)) {
    mActor = nullptr;
    return nullptr;
  }

  return mActor;
}

}  // namespace mozilla::dom
