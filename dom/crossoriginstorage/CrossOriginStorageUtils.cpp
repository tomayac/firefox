/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CrossOriginStorageUtils.h"

#include "ScopedNSSTypes.h"
#include "mozilla/Hex.h"
#include "mozilla/dom/WebCryptoCommon.h"
#include "nsContentUtils.h"
#include "nsNSSComponent.h"
#include "nsString.h"
#include "pk11pub.h"

namespace mozilla::dom {

namespace {

struct HashAlgorithmInfo {
  COSHashAlgorithm mAlgorithm;
  const char* mName;
  SECOidTag mOidTag;
  // The length, in hex characters, of a lowercase hexadecimal encoding of
  // this algorithm's digest.
  uint32_t mValueLength;
};

constexpr HashAlgorithmInfo kHashAlgorithms[] = {
    {COSHashAlgorithm::SHA1, WEBCRYPTO_ALG_SHA1, SEC_OID_SHA1, 40},
    {COSHashAlgorithm::SHA256, WEBCRYPTO_ALG_SHA256, SEC_OID_SHA256, 64},
    {COSHashAlgorithm::SHA384, WEBCRYPTO_ALG_SHA384, SEC_OID_SHA384, 96},
    {COSHashAlgorithm::SHA512, WEBCRYPTO_ALG_SHA512, SEC_OID_SHA512, 128},
};

const HashAlgorithmInfo& GetInfo(COSHashAlgorithm aAlgorithm) {
  for (const auto& info : kHashAlgorithms) {
    if (info.mAlgorithm == aAlgorithm) {
      return info;
    }
  }
  MOZ_CRASH("unhandled COSHashAlgorithm");
}

}  // namespace

Maybe<COSHashAlgorithm> ParseHashAlgorithm(const nsAString& aName) {
  for (const auto& info : kHashAlgorithms) {
    if (nsContentUtils::EqualsIgnoreASCIICase(
            aName, NS_ConvertASCIItoUTF16(info.mName))) {
      return Some(info.mAlgorithm);
    }
  }
  return Nothing();
}

uint32_t ExpectedHexValueLength(COSHashAlgorithm aAlgorithm) {
  return GetInfo(aAlgorithm).mValueLength;
}

const char* CanonicalHashAlgorithmName(COSHashAlgorithm aAlgorithm) {
  return GetInfo(aAlgorithm).mName;
}

nsresult ComputeHashValueHex(COSHashAlgorithm aAlgorithm,
                             const nsTArray<uint8_t>& aBytes,
                             nsACString& aOutValueHex) {
  SECOidTag oidTag = GetInfo(aAlgorithm).mOidTag;

  uint32_t hashLen = HASH_ResultLenByOidTag(oidTag);
  nsTArray<uint8_t> digest;
  if (!digest.SetLength(hashLen, fallible)) {
    return NS_ERROR_OUT_OF_MEMORY;
  }

  nsresult rv =
      MapSECStatus(PK11_HashBuf(oidTag, digest.Elements(), aBytes.Elements(),
                                static_cast<int32_t>(aBytes.Length())));
  if (NS_FAILED(rv)) {
    return rv;
  }

  aOutValueHex.Truncate();
  HexEncode(digest, aOutValueHex);
  return NS_OK;
}

}  // namespace mozilla::dom
