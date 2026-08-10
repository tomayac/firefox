/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CrossOriginStoragePublicHashList.h"

#include <cstring>

#include "mozilla/RandomNum.h"
#include "mozilla/SyncRunnable.h"
#include "nsDirectoryServiceDefs.h"
#include "nsDirectoryServiceUtils.h"
#include "nsIFile.h"
#include "nsIInputStream.h"
#include "nsNetUtil.h"
#include "nsString.h"
#include "nsThreadUtils.h"

namespace mozilla::dom {

namespace {

constexpr size_t kDigestSize = 32;  // SHA-256, raw bytes.

// Lazily loads and binary-searches data/public-hash-list.bin (shipped
// alongside the GRE via FINAL_TARGET_FILES; see moz.build). A load
// failure (file missing, unreadable, or an unexpected size) leaves this
// empty rather than crashing or throwing -- indistinguishable from a
// genuinely empty PHL, the same fail-safe behavior the original empty
// seed had.
class PublicHashListData {
 public:
  static const PublicHashListData& Get() {
    static PublicHashListData sInstance;
    return sInstance;
  }

  bool Contains(const uint8_t (&aDigest)[kDigestSize]) const {
    if (mData.IsEmpty()) {
      return false;
    }
    size_t count = mData.Length() / kDigestSize;
    size_t lo = 0;
    size_t hi = count;
    while (lo < hi) {
      size_t mid = lo + (hi - lo) / 2;
      int cmp =
          memcmp(mData.Elements() + mid * kDigestSize, aDigest, kDigestSize);
      if (cmp == 0) {
        return true;
      }
      if (cmp < 0) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    return false;
  }

 private:
  PublicHashListData() { Load(); }

  void Load() {
    // NS_GetSpecialDirectory must run on the main thread; this is a
    // one-time cost paid on whichever thread first calls Contains()
    // (always the PBackground thread in practice).
    nsCOMPtr<nsIFile> file;
    nsCOMPtr<nsIRunnable> runnable =
        NS_NewRunnableFunction("PublicHashListData::ResolveFile", [&file]() {
          NS_GetSpecialDirectory(NS_GRE_DIR, getter_AddRefs(file));
        });
    nsCOMPtr<nsIThread> mainThread;
    NS_GetMainThread(getter_AddRefs(mainThread));
    if (!mainThread) {
      return;
    }
    SyncRunnable::DispatchToThread(mainThread, runnable);
    if (!file) {
      return;
    }

    if (NS_FAILED(file->AppendNative("crossoriginstorage"_ns)) ||
        NS_FAILED(file->AppendNative("public-hash-list.bin"_ns))) {
      return;
    }

    nsCOMPtr<nsIInputStream> stream;
    if (NS_FAILED(NS_NewLocalFileInputStream(getter_AddRefs(stream), file))) {
      return;
    }
    nsCString bytes;
    if (NS_FAILED(NS_ReadInputStreamToString(stream, bytes, -1))) {
      return;
    }
    if (bytes.Length() % kDigestSize != 0) {
      return;  // Corrupt/truncated: fail safe to "nothing loaded".
    }
    mData.AppendElements(reinterpret_cast<const uint8_t*>(bytes.get()),
                         bytes.Length());
  }

  nsTArray<uint8_t> mData;
};

bool HexNibble(char aChar, uint8_t* aOutNibble) {
  if (aChar >= '0' && aChar <= '9') {
    *aOutNibble = static_cast<uint8_t>(aChar - '0');
    return true;
  }
  if (aChar >= 'a' && aChar <= 'f') {
    *aOutNibble = static_cast<uint8_t>(aChar - 'a' + 10);
    return true;
  }
  return false;  // Uppercase deliberately rejected: COS hash values are
                 // already validated lowercase-hex by the time this is
                 // reached (CrossOriginStorageManager::ValidateRequest).
}

bool HexDecode(const nsACString& aHex, uint8_t (&aOutDigest)[kDigestSize]) {
  if (aHex.Length() != kDigestSize * 2) {
    return false;
  }
  for (size_t i = 0; i < kDigestSize; i++) {
    uint8_t hi, lo;
    if (!HexNibble(aHex.CharAt(2 * i), &hi) ||
        !HexNibble(aHex.CharAt(2 * i + 1), &lo)) {
      return false;
    }
    aOutDigest[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

// Not spec-mandated; matches Servo's and Ladybird's own chosen constants.
constexpr uint64_t kGreaseSizeCeilingBytes = 500 * 1024;  // 500 KiB
constexpr uint64_t kGreaseProbabilityPercent = 1;         // 1%

}  // namespace

/* static */
bool CrossOriginStoragePublicHashList::Contains(COSHashAlgorithm aAlgorithm,
                                                const nsACString& aValueHex) {
  // Only SHA-256 is normatively specified by the spec's own hash-value
  // shape constraints; no PHL is maintained for the other recognized
  // algorithms.
  if (aAlgorithm != COSHashAlgorithm::SHA256) {
    return false;
  }
  uint8_t digest[kDigestSize];
  if (!HexDecode(aValueHex, digest)) {
    return false;
  }
  return PublicHashListData::Get().Contains(digest);
}

/* static */
bool CrossOriginStorageGrease::ShouldDisclose(uint64_t aByteSize) {
  if (aByteSize > kGreaseSizeCeilingBytes) {
    return false;
  }
  // A cryptographically-sourced roll, not a fast/predictable PRNG: this
  // feeds a privacy-relevant decision an adversary must not be able to
  // predict or bias by observing prior rolls.
  return (mozilla::RandomUint64OrDie() % 100) < kGreaseProbabilityPercent;
}

}  // namespace mozilla::dom
