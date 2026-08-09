/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_CrossOriginStorageUtils_h
#define mozilla_dom_CrossOriginStorageUtils_h

#include "mozilla/Maybe.h"
#include "nsStringFwd.h"
#include "nsTArray.h"
#include "nsTArrayForwardDeclare.h"

namespace mozilla::dom {

// The set of hash algorithm names recognized by [[WEBCRYPTO]] that a
// [=COS hash=] may name (https://wicg.github.io/cross-origin-storage/#hashes).
enum class COSHashAlgorithm {
  SHA1,
  SHA256,
  SHA384,
  SHA512,
};

// An implementation-defined maximum length for the `origins` option's list
// form (https://wicg.github.io/cross-origin-storage/#storage-limits), so a
// list of origins can't be used as an undeclared substitute for "*". 100 is
// the value both of the two known independent implementations (Servo,
// Ladybird) use. Enforced both at request-validation time
// (CrossOriginStorageManager) and at merge time
// (CrossOriginStorageRegistry::UpgradeResourceVisibility).
constexpr uint32_t kMaxOriginsListLength = 100;

// https://wicg.github.io/cross-origin-storage/#normalize-requested-origins
// The result of normalizing options["origins"]: same-site-only (the
// default, when the option is omitted), an explicit list of ASCII-
// serialized origins, or wildcard. Distinct from the IPDL-generated
// COSRequestedOrigins (PCrossOriginStorage.ipdl's wire form of the same
// concept) -- this is the client-side, pre-serialization representation.
struct COSRequestedOriginsValue {
  enum class Kind { SameSiteOnly, List, Wildcard };
  Kind mKind = Kind::SameSiteOnly;
  // Meaningful only when mKind == Kind::List.
  nsTArray<nsCString> mList;

  COSRequestedOriginsValue() = default;
  COSRequestedOriginsValue(COSRequestedOriginsValue&&) = default;
  COSRequestedOriginsValue& operator=(COSRequestedOriginsValue&&) = default;
  // nsTArray's copy constructor is explicit (deliberately, to make copies
  // visible at call sites as .Clone()), which otherwise leaves this
  // struct's own copy constructor implicitly deleted; every call site that
  // needs a copy (lambda captures, a UniquePtr constructor forwarding an
  // lvalue) needs it to be implicit, so spell it out here instead of
  // pushing .Clone() out to each of them.
  COSRequestedOriginsValue(const COSRequestedOriginsValue& aOther)
      : mKind(aOther.mKind), mList(aOther.mList.Clone()) {}
  COSRequestedOriginsValue& operator=(const COSRequestedOriginsValue&) = delete;
};

// Case-insensitively matches aName against a recognized [[WEBCRYPTO]] hash
// algorithm name.
Maybe<COSHashAlgorithm> ParseHashAlgorithm(const nsAString& aName);

// The length, in lowercase hex characters, of a [=COS hash/value=] digest
// produced by aAlgorithm. The spec only normatively constrains SHA-256's
// (64), but every recognized algorithm's value is validated to its own
// expected length -- see the caller in CrossOriginStorageManager.cpp.
uint32_t ExpectedHexValueLength(COSHashAlgorithm aAlgorithm);

// A canonical, stable spelling of aAlgorithm, suitable for use as (part of)
// a [=COS registry=] key. Not necessarily identical to the spelling script
// passed in, since [=COS hash/algorithm=] comparison is ASCII
// case-insensitive.
const char* CanonicalHashAlgorithmName(COSHashAlgorithm aAlgorithm);

// Computes the lowercase hexadecimal digest of aBytes under aAlgorithm,
// using NSS directly (the same primitive crypto.subtle.digest() itself
// bottoms out on) rather than round-tripping through Web Crypto.
nsresult ComputeHashValueHex(COSHashAlgorithm aAlgorithm,
                             const nsTArray<uint8_t>& aBytes,
                             nsACString& aOutValueHex);

}  // namespace mozilla::dom

#endif  // mozilla_dom_CrossOriginStorageUtils_h
