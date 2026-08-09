/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CrossOriginStoragePublicHashList.h"

#include <array>

#include "mozilla/RandomNum.h"
#include "nsString.h"

namespace mozilla::dom {

namespace {

// Sorted (ascending, lowercase hex) so a real implementation can grow this
// into a binary search without changing the lookup's shape; see the
// header comment for why this seed is currently empty. (std::array<T, 0>,
// not a zero-length C array, which isn't valid standard C++.)
constexpr std::array<const char*, 0> kSha256PublicHashList{};

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
  for (const char* candidate : kSha256PublicHashList) {
    if (aValueHex.EqualsASCII(candidate)) {
      return true;
    }
  }
  return false;
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
