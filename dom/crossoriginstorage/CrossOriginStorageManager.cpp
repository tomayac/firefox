/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CrossOriginStorageManager.h"

#include "mozilla/BasePrincipal.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/dom/CrossOriginStorageBinding.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/FeaturePolicyUtils.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/WebCryptoCommon.h"
#include "nsContentUtils.h"
#include "nsIGlobalObject.h"
#include "nsIURI.h"
#include "nsNetUtil.h"
#include "nsPIDOMWindow.h"

namespace mozilla::dom {

namespace {

struct HashAlgorithmInfo {
  const char* mName;
  // The length, in hex characters, of a lowercase hexadecimal encoding of
  // this algorithm's digest.
  uint32_t mValueLength;
};

// The set of hash algorithm names recognized by [[WEBCRYPTO]]. The spec
// (https://wicg.github.io/cross-origin-storage/#validate-a-cos-request)
// only normatively constrains the shape of a SHA-256 value (64 lowercase
// hex characters), but every recognized algorithm's value is validated the
// same way here: `value` is later used to address an entry on disk, so an
// unvalidated value for a non-default algorithm would be a path-traversal
// risk.
constexpr HashAlgorithmInfo kRecognizedHashAlgorithms[] = {
    {WEBCRYPTO_ALG_SHA1, 40},
    {WEBCRYPTO_ALG_SHA256, 64},
    {WEBCRYPTO_ALG_SHA384, 96},
    {WEBCRYPTO_ALG_SHA512, 128},
};

const HashAlgorithmInfo* FindHashAlgorithm(const nsAString& aAlgorithm) {
  for (const auto& info : kRecognizedHashAlgorithms) {
    if (nsContentUtils::EqualsIgnoreASCIICase(
            aAlgorithm, NS_ConvertASCIItoUTF16(info.mName))) {
      return &info;
    }
  }
  return nullptr;
}

bool IsLowercaseHex(const nsAString& aValue) {
  for (uint32_t i = 0; i < aValue.Length(); ++i) {
    char16_t c = aValue.CharAt(i);
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

// An implementation-defined maximum length for the `origins` option's list
// form (https://wicg.github.io/cross-origin-storage/#storage-limits), so a
// list of origins can't be used as an undeclared substitute for "*". 100 is
// the value both of the two known independent implementations (Servo,
// Ladybird) use.
constexpr uint32_t kMaxOriginsListLength = 100;

// https://wicg.github.io/cross-origin-storage/#validate-a-cos-request
void ValidateRequest(const CrossOriginStorageRequestFileHandleHash& aHash,
                     const CrossOriginStorageRequestFileHandleOptions& aOptions,
                     ErrorResult& aRv) {
  // Step 1.
  const HashAlgorithmInfo* algorithm = FindHashAlgorithm(aHash.mAlgorithm);
  if (!algorithm) {
    aRv.ThrowTypeError(
        "algorithm is not a hash algorithm name recognized by the Web "
        "Crypto API");
    return;
  }

  // Step 2.
  if (aHash.mValue.Length() != algorithm->mValueLength ||
      !IsLowercaseHex(aHash.mValue)) {
    aRv.ThrowTypeError(
        "value is not a lowercase hexadecimal digest of the length "
        "expected for the given algorithm");
    return;
  }

  // Step 3: if options["origins"] doesn't exist, or is "*", there is
  // nothing further to validate here.
  if (!aOptions.mOrigins.WasPassed()) {
    return;
  }
  const auto& origins = aOptions.mOrigins.Value();
  if (origins.IsString() && origins.GetAsString().EqualsLiteral("*")) {
    return;
  }

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
    return;
  }

  // Step 3.3: for each candidate, it must parse as a URL, and the parsed
  // URL's origin must not be opaque.
  for (const auto& candidate : candidates) {
    nsCOMPtr<nsIURI> uri;
    if (NS_FAILED(NS_NewURI(getter_AddRefs(uri), candidate))) {
      aRv.ThrowTypeError("origins entry does not parse as a URL");
      return;
    }

    nsCOMPtr<nsIPrincipal> principal =
        BasePrincipal::CreateContentPrincipal(uri, OriginAttributes());
    if (principal->GetIsNullPrincipal()) {
      aRv.ThrowTypeError("origins entry parses to an opaque origin");
      return;
    }
  }
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
  ValidateRequest(aHash, aOptions, validationError);
  if (validationError.Failed()) {
    promise->MaybeReject(std::move(validationError));
    return promise.forget();
  }

  // TODO(Bug TBD): step 8, "complete a create request" / "complete a read
  // request", lands in a follow-up patch once the Cross-Origin Storage
  // registry actor exists.
  promise->MaybeRejectWithNotSupportedError(
      "Cross-Origin Storage is not yet implemented");
  return promise.forget();
}

}  // namespace mozilla::dom
