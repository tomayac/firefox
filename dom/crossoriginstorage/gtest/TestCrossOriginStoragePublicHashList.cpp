/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CrossOriginStoragePublicHashList.h"
#include "gtest/gtest.h"
#include "nsString.h"

using namespace mozilla::dom;

// Real entries from data/public-hash-list.bin (first, a middle, and the
// last of the sorted list at generation time) -- exercises the actual
// shipped file, not a synthetic stand-in, so a regression in loading it
// (wrong path, off-by-one in the binary search, a bad FINAL_TARGET_FILES
// wiring) fails this test rather than silently degrading every wildcard-
// scope read to GREASE-only. If data/public-hash-list.bin is regenerated
// (see generate_public_hash_list.py), refresh these three constants from
// the new file's actual first/middle/last entries.
constexpr auto kKnownFirstHash =
    "00003bcf96fc9cb1ac3c88678137b49a3a67a7991aa46f8c944fb8756b51b84e"_ns;
constexpr auto kKnownMiddleHash =
    "803753246ed1d1f08694cfad7aae40c7fb91cc79650c48d444e0f4d376c26fe2"_ns;
constexpr auto kKnownLastHash =
    "ffffd61000db2fae56da79c1ae8ec36c4e890b9a2e9feb30460a342ddf61efc2"_ns;

TEST(CrossOriginStoragePublicHashList, ContainsKnownEntries)
{
  EXPECT_TRUE(CrossOriginStoragePublicHashList::Contains(
      COSHashAlgorithm::SHA256, kKnownFirstHash));
  EXPECT_TRUE(CrossOriginStoragePublicHashList::Contains(
      COSHashAlgorithm::SHA256, kKnownMiddleHash));
  EXPECT_TRUE(CrossOriginStoragePublicHashList::Contains(
      COSHashAlgorithm::SHA256, kKnownLastHash));
}

TEST(CrossOriginStoragePublicHashList, RejectsAbsentEntry)
{
  // All-zeros: astronomically unlikely to be a real SHA-256 digest of
  // anything on the list.
  auto allZeros =
      "0000000000000000000000000000000000000000000000000000000000000000"_ns;
  EXPECT_FALSE(CrossOriginStoragePublicHashList::Contains(
      COSHashAlgorithm::SHA256, allZeros));
}

TEST(CrossOriginStoragePublicHashList, RejectsMalformedInput)
{
  EXPECT_FALSE(CrossOriginStoragePublicHashList::Contains(
      COSHashAlgorithm::SHA256, "too-short"_ns));
  EXPECT_FALSE(CrossOriginStoragePublicHashList::Contains(
      COSHashAlgorithm::SHA256,
      // Uppercase hex is rejected -- COS hash values are validated
      // lowercase before ever reaching this class.
      "00003BCF96FC9CB1AC3C88678137B49A3A67A7991AA46F8C944FB8756B51B84E"_ns));
}

TEST(CrossOriginStoragePublicHashList, OnlySha256IsGated)
{
  // A known-good SHA-256 digest under any other algorithm tag must not
  // match -- the PHL is SHA-256-only (see the class's own header
  // comment: no PHL is maintained for the other recognized algorithms).
  EXPECT_FALSE(CrossOriginStoragePublicHashList::Contains(
      COSHashAlgorithm::SHA1, kKnownFirstHash));
  EXPECT_FALSE(CrossOriginStoragePublicHashList::Contains(
      COSHashAlgorithm::SHA384, kKnownFirstHash));
  EXPECT_FALSE(CrossOriginStoragePublicHashList::Contains(
      COSHashAlgorithm::SHA512, kKnownFirstHash));
}
