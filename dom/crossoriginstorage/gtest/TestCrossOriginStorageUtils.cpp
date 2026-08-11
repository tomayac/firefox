/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "BackgroundThreadTestHelpers.h"
#include "CrossOriginStorageUtils.h"
#include "gtest/gtest.h"
#include "nsString.h"

using namespace mozilla;
using namespace mozilla::dom;
using namespace mozilla::dom::crossoriginstorage_gtest;

class CrossOriginStorageUtilsTest : public ::testing::Test {
 protected:
  // Only ComputeHashValueHex's tests actually touch NSS, but priming it
  // for the whole fixture is simpler than splitting this file in two,
  // and EnsureNSSInitializedForTest() is a one-time no-op for the rest.
  static void SetUpTestSuite() { EnsureNSSInitializedForTest(); }
};

TEST_F(CrossOriginStorageUtilsTest,
       ParseHashAlgorithmRecognizesEveryAlgorithm) {
  EXPECT_EQ(ParseHashAlgorithm(u"SHA-1"_ns), Some(COSHashAlgorithm::SHA1));
  EXPECT_EQ(ParseHashAlgorithm(u"SHA-256"_ns), Some(COSHashAlgorithm::SHA256));
  EXPECT_EQ(ParseHashAlgorithm(u"SHA-384"_ns), Some(COSHashAlgorithm::SHA384));
  EXPECT_EQ(ParseHashAlgorithm(u"SHA-512"_ns), Some(COSHashAlgorithm::SHA512));
}

TEST_F(CrossOriginStorageUtilsTest, ParseHashAlgorithmIsCaseInsensitive) {
  EXPECT_EQ(ParseHashAlgorithm(u"sha-256"_ns), Some(COSHashAlgorithm::SHA256));
  EXPECT_EQ(ParseHashAlgorithm(u"Sha-256"_ns), Some(COSHashAlgorithm::SHA256));
  EXPECT_EQ(ParseHashAlgorithm(u"SHA-256"_ns), Some(COSHashAlgorithm::SHA256));
}

TEST_F(CrossOriginStorageUtilsTest,
       ParseHashAlgorithmRejectsUnrecognizedNames) {
  EXPECT_EQ(ParseHashAlgorithm(u"MD5"_ns), Nothing());
  EXPECT_EQ(ParseHashAlgorithm(u""_ns), Nothing());
  EXPECT_EQ(ParseHashAlgorithm(u"SHA-256 "_ns), Nothing());
  EXPECT_EQ(ParseHashAlgorithm(u"SHA256"_ns), Nothing());
}

// The exact expected length per algorithm matters directly for the
// path-traversal defense CrossOriginStorageManager::ValidateRequest()
// builds on top of this function -- a wrong length here would either
// reject legitimate values (availability bug) or accept malformed ones
// (security bug), so pin every algorithm's value explicitly rather than
// just the default SHA-256 case.
TEST_F(CrossOriginStorageUtilsTest,
       ExpectedHexValueLengthMatchesEachDigestSize) {
  EXPECT_EQ(ExpectedHexValueLength(COSHashAlgorithm::SHA1), 40u);
  EXPECT_EQ(ExpectedHexValueLength(COSHashAlgorithm::SHA256), 64u);
  EXPECT_EQ(ExpectedHexValueLength(COSHashAlgorithm::SHA384), 96u);
  EXPECT_EQ(ExpectedHexValueLength(COSHashAlgorithm::SHA512), 128u);
}

TEST_F(CrossOriginStorageUtilsTest,
       CanonicalHashAlgorithmNameRoundTripsThroughParse) {
  for (auto algorithm : {COSHashAlgorithm::SHA1, COSHashAlgorithm::SHA256,
                         COSHashAlgorithm::SHA384, COSHashAlgorithm::SHA512}) {
    const char* name = CanonicalHashAlgorithmName(algorithm);
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(
        ParseHashAlgorithm(NS_ConvertUTF8toUTF16(nsDependentCString(name))),
        Some(algorithm));
  }
}

TEST_F(CrossOriginStorageUtilsTest,
       ComputeHashValueHexMatchesKnownSha256Vector) {
  // SHA-256("abc"), a standard published NIST test vector -- verifies
  // this isn't just internally self-consistent but matches the actual
  // published algorithm.
  nsTArray<uint8_t> bytes;
  bytes.AppendElements(reinterpret_cast<const uint8_t*>("abc"), 3);

  nsAutoCString hex;
  nsresult rv = ComputeHashValueHex(COSHashAlgorithm::SHA256, bytes, hex);
  ASSERT_TRUE(NS_SUCCEEDED(rv));
  EXPECT_TRUE(hex.EqualsLiteral(
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

TEST_F(CrossOriginStorageUtilsTest,
       ComputeHashValueHexIsLowercaseAndExpectedLength) {
  nsTArray<uint8_t> bytes;
  bytes.AppendElements(reinterpret_cast<const uint8_t*>("cross-origin-storage"),
                       21);

  for (auto algorithm : {COSHashAlgorithm::SHA1, COSHashAlgorithm::SHA256,
                         COSHashAlgorithm::SHA384, COSHashAlgorithm::SHA512}) {
    nsAutoCString hex;
    nsresult rv = ComputeHashValueHex(algorithm, bytes, hex);
    ASSERT_TRUE(NS_SUCCEEDED(rv));
    EXPECT_EQ(hex.Length(), ExpectedHexValueLength(algorithm));
    for (uint32_t i = 0; i < hex.Length(); i++) {
      char c = hex.CharAt(i);
      EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
          << "unexpected character '" << c << "' in digest";
    }
  }
}

TEST_F(CrossOriginStorageUtilsTest,
       ComputeHashValueHexDiffersForDifferentContent) {
  nsTArray<uint8_t> bytesA;
  bytesA.AppendElements(reinterpret_cast<const uint8_t*>("content-a"), 9);
  nsTArray<uint8_t> bytesB;
  bytesB.AppendElements(reinterpret_cast<const uint8_t*>("content-b"), 9);

  nsAutoCString hexA, hexB;
  ASSERT_TRUE(NS_SUCCEEDED(
      ComputeHashValueHex(COSHashAlgorithm::SHA256, bytesA, hexA)));
  ASSERT_TRUE(NS_SUCCEEDED(
      ComputeHashValueHex(COSHashAlgorithm::SHA256, bytesB, hexB)));
  EXPECT_FALSE(hexA.Equals(hexB));
}

TEST_F(CrossOriginStorageUtilsTest, ComputeHashValueHexIsDeterministic) {
  nsTArray<uint8_t> bytes;
  bytes.AppendElements(reinterpret_cast<const uint8_t*>("deterministic"), 13);

  nsAutoCString hex1, hex2;
  ASSERT_TRUE(
      NS_SUCCEEDED(ComputeHashValueHex(COSHashAlgorithm::SHA256, bytes, hex1)));
  ASSERT_TRUE(
      NS_SUCCEEDED(ComputeHashValueHex(COSHashAlgorithm::SHA256, bytes, hex2)));
  EXPECT_TRUE(hex1.Equals(hex2));
}
