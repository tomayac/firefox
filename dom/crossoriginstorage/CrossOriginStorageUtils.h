/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_CrossOriginStorageUtils_h
#define mozilla_dom_CrossOriginStorageUtils_h

#include "mozilla/Maybe.h"
#include "nsStringFwd.h"
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
