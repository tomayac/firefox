/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "BackgroundThreadTestHelpers.h"
#include "CrossOriginStoragePersistence.h"
#include "CrossOriginStoragePublicHashList.h"
#include "CrossOriginStorageRegistry.h"
#include "CrossOriginStorageUtils.h"
#include "gtest/gtest.h"
#include "mozilla/BasePrincipal.h"
#include "mozilla/ipc/BackgroundUtils.h"
#include "nsIPrincipal.h"
#include "nsString.h"
#include "nsThreadUtils.h"

using namespace mozilla;
using namespace mozilla::dom;
using namespace mozilla::dom::crossoriginstorage_gtest;
using namespace mozilla::ipc;

namespace {

// A real deadlock risk this avoids, worth spelling out: every test below
// blocks *this* (main) thread inside SyncRunnable::DispatchToThread()
// while the background thread does its work -- and SyncRunnable's own
// doc comment is explicit that the target thread must never block on the
// calling thread while that's happening, or it deadlocks (the calling
// thread is parked in a Monitor::Wait(), not pumping its own event
// queue, so a nested dispatch back to it would simply never run).
// CrossOriginStorageRegistry's *first-ever* GetOrCreate() call
// transitively does exactly that nested dispatch twice: once via
// CrossOriginStoragePersistence::GetOrCreate() (always, to resolve the
// profile directory) and once via CrossOriginStoragePublicHashList's
// lazy loader (only for a wildcard-scope read that reaches Gate 1) --
// both resolve NS_GetSpecialDirectory() with their own
// SyncRunnable-to-main-thread round trip. Priming both of those
// singletons directly from the main thread, before any background-
// thread dispatch happens, sidesteps this: SyncRunnable::DispatchToThread
// takes a same-thread fast path (runs inline, no Monitor involved at
// all) whenever the target thread is the caller's own, so priming here
// is synchronous and risk-free, and every later, real nested lookup from
// the background thread just returns the already-cached result instead
// of dispatching anywhere.
void PrimeMainThreadOnlySingletons() {
  (void)CrossOriginStoragePersistence::GetOrCreate();
  (void)CrossOriginStoragePublicHashList::Contains(
      COSHashAlgorithm::SHA256,
      "0000000000000000000000000000000000000000000000000000000000000000"_ns);
}

// Real nsIPrincipal → PrincipalInfo conversion (mirroring
// dom/quota/test/gtest/QuotaManagerDependencyFixture.cpp's own
// CreateContentPrincipalInfo helper) rather than hand-filling
// ContentPrincipalInfo's fields directly, so same-site/eTLD+1
// computation goes through the real Public Suffix List-backed logic
// every other caller gets, not a hand-reasoned stand-in. Must run on
// the main thread; call before RunOnBackgroundThread, then pass the
// resulting (plain-data, thread-safe-to-copy) PrincipalInfo in by
// value.
PrincipalInfo MakePrincipalInfo(const nsACString& aOrigin) {
  nsCOMPtr<nsIPrincipal> principal =
      BasePrincipal::CreateContentPrincipal(aOrigin);
  MOZ_RELEASE_ASSERT(principal);
  PrincipalInfo info;
  MOZ_ALWAYS_SUCCEEDS(PrincipalToPrincipalInfo(principal, &info));
  return info;
}

nsTArray<uint8_t> MakeBytes(const nsACString& aContent) {
  nsTArray<uint8_t> bytes;
  bytes.AppendElements(
      reinterpret_cast<const uint8_t*>(aContent.BeginReading()),
      aContent.Length());
  return bytes;
}

nsAutoCString HashOf(const nsTArray<uint8_t>& aBytes) {
  nsAutoCString hex;
  MOZ_ALWAYS_SUCCEEDS(
      ComputeHashValueHex(COSHashAlgorithm::SHA256, aBytes, hex));
  return hex;
}

// Every test uses its own hash, derived from a value unique to that
// test, since CrossOriginStorageRegistry is a single process-wide
// singleton shared across every TEST() in this binary -- two tests
// reusing the same hash would corrupt each other's state.
nsTArray<uint8_t> UniqueContentFor(const char* aTestName) {
  nsAutoCString content("TestCrossOriginStorageRegistry:");
  content.Append(aTestName);
  return MakeBytes(content);
}

}  // namespace

class CrossOriginStorageRegistryTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    EnsureNSSInitializedForTest();
    EnsureBackgroundThreadStarted();
    PrimeMainThreadOnlySingletons();
  }
};

TEST_F(CrossOriginStorageRegistryTest, CreateRequestThenPlainReadIsPending) {
  nsTArray<uint8_t> bytes = UniqueContentFor(CurrentTestName());
  nsAutoCString value = HashOf(bytes);
  PrincipalInfo writer = MakePrincipalInfo("https://writer.example.com"_ns);

  CrossOriginStorageRegistry::ReadOutcome readOutcome;
  RunOnBackgroundThread(NS_NewRunnableFunction(__func__, [&] {
    auto& registry = CrossOriginStorageRegistry::GetOrCreate();
    bool wasWritten =
        registry.CompleteCreateRequest(COSHashAlgorithm::SHA256, value, writer);
    EXPECT_FALSE(wasWritten);
    readOutcome =
        registry.CompleteReadRequest(COSHashAlgorithm::SHA256, value, writer);
    // Clean up so this pending entry doesn't linger for later tests.
    registry.ReleaseOutstandingWriter(COSHashAlgorithm::SHA256, value);
  }));

  EXPECT_EQ(readOutcome, CrossOriginStorageRegistry::ReadOutcome::Pending);
}

TEST_F(CrossOriginStorageRegistryTest, CorrectWriteIsReadableByStoringOrigin) {
  nsTArray<uint8_t> bytes = UniqueContentFor(CurrentTestName());
  nsAutoCString value = HashOf(bytes);
  PrincipalInfo writer = MakePrincipalInfo("https://writer.example.com"_ns);

  nsresult verifyResult;
  CrossOriginStorageRegistry::ReadOutcome readOutcome;
  RunOnBackgroundThread(NS_NewRunnableFunction(__func__, [&] {
    auto& registry = CrossOriginStorageRegistry::GetOrCreate();
    registry.CompleteCreateRequest(COSHashAlgorithm::SHA256, value, writer);
    COSRequestedOriginsValue requestedOrigins;  // Default: SameSiteOnly.
    verifyResult = registry.VerifyAndStore(COSHashAlgorithm::SHA256, value,
                                           bytes, writer, requestedOrigins);
    readOutcome =
        registry.CompleteReadRequest(COSHashAlgorithm::SHA256, value, writer);
  }));

  EXPECT_TRUE(NS_SUCCEEDED(verifyResult));
  EXPECT_EQ(readOutcome, CrossOriginStorageRegistry::ReadOutcome::Found);
}

TEST_F(CrossOriginStorageRegistryTest, MismatchedWriteFailsAndLeavesNoEntry) {
  nsTArray<uint8_t> declaredBytes = UniqueContentFor(CurrentTestName());
  nsAutoCString value = HashOf(declaredBytes);  // Hash of the RIGHT content...
  nsTArray<uint8_t> wrongBytes = MakeBytes("this is not that content"_ns);
  PrincipalInfo writer = MakePrincipalInfo("https://writer.example.com"_ns);

  nsresult verifyResult;
  CrossOriginStorageRegistry::ReadOutcome readOutcomeAfterFailure;
  RunOnBackgroundThread(NS_NewRunnableFunction(__func__, [&] {
    auto& registry = CrossOriginStorageRegistry::GetOrCreate();
    registry.CompleteCreateRequest(COSHashAlgorithm::SHA256, value, writer);
    COSRequestedOriginsValue requestedOrigins;
    // ...but actually write wrongBytes, which doesn't hash to `value`.
    verifyResult = registry.VerifyAndStore(
        COSHashAlgorithm::SHA256, value, wrongBytes, writer, requestedOrigins);
    readOutcomeAfterFailure =
        registry.CompleteReadRequest(COSHashAlgorithm::SHA256, value, writer);
  }));

  EXPECT_EQ(verifyResult, NS_ERROR_DOM_DATA_ERR);
  // Not stuck Pending forever, and not still readable -- the failed
  // write's entry is fully gone, matching the outstanding-writer-count
  // cleanup this behavior depends on.
  EXPECT_EQ(readOutcomeAfterFailure,
            CrossOriginStorageRegistry::ReadOutcome::NotFound);
}

// The single most important invariant this registry's outstanding-writer
// count exists to protect (see CrossOriginStorageRegistry.h's own
// comment on ReleaseOutstandingWriter): a failed sibling write must not
// take down a concurrent, still-outstanding write for the same hash.
// This is also covered end-to-end by the imported WPT suite
// (requestFileHandle-create-and-read.*.any.js), but that requires a full
// browser and IPC round trip per run; this version exercises the exact
// same invariant in milliseconds, directly against the registry.
TEST_F(CrossOriginStorageRegistryTest,
       ConcurrentWriteFailureDoesNotDisruptConcurrentSuccess) {
  nsTArray<uint8_t> correctBytes = UniqueContentFor(CurrentTestName());
  nsAutoCString value = HashOf(correctBytes);
  nsTArray<uint8_t> wrongBytes =
      MakeBytes("wrong bytes entirely, still unique"_ns);
  PrincipalInfo writerA = MakePrincipalInfo("https://writer-a.example.com"_ns);
  PrincipalInfo writerB = MakePrincipalInfo("https://writer-b.example.com"_ns);

  nsresult resultA, resultB;
  CrossOriginStorageRegistry::ReadOutcome readOutcome;
  RunOnBackgroundThread(NS_NewRunnableFunction(__func__, [&] {
    auto& registry = CrossOriginStorageRegistry::GetOrCreate();
    // Both writers open a create request for the *same* hash before
    // either finishes -- this is what makes the two outstanding writers
    // genuinely concurrent from the registry's point of view, matching
    // two tabs racing in the real WPT scenario.
    registry.CompleteCreateRequest(COSHashAlgorithm::SHA256, value, writerA);
    registry.CompleteCreateRequest(COSHashAlgorithm::SHA256, value, writerB);

    COSRequestedOriginsValue requestedOrigins;
    resultA = registry.VerifyAndStore(COSHashAlgorithm::SHA256, value,
                                      wrongBytes, writerA, requestedOrigins);
    resultB = registry.VerifyAndStore(COSHashAlgorithm::SHA256, value,
                                      correctBytes, writerB, requestedOrigins);

    readOutcome =
        registry.CompleteReadRequest(COSHashAlgorithm::SHA256, value, writerB);
  }));

  EXPECT_EQ(resultA, NS_ERROR_DOM_DATA_ERR)
      << "writer A (wrong bytes) must fail";
  EXPECT_TRUE(NS_SUCCEEDED(resultB))
      << "writer B (correct bytes) must still succeed despite A's concurrent "
         "failure";
  EXPECT_EQ(readOutcome, CrossOriginStorageRegistry::ReadOutcome::Found);
}

TEST_F(CrossOriginStorageRegistryTest,
       SameSiteOnlyDisclosesToSameSiteNotToDifferentSite) {
  nsTArray<uint8_t> bytes = UniqueContentFor(CurrentTestName());
  nsAutoCString value = HashOf(bytes);
  PrincipalInfo writer = MakePrincipalInfo("https://writer.example.com"_ns);
  // Same registrable domain (example.com) as the writer, different
  // origin -- must be disclosed under the default SameSiteOnly scope.
  PrincipalInfo sameSiteReader =
      MakePrincipalInfo("https://reader.example.com"_ns);
  // Genuinely different site -- must not be disclosed.
  PrincipalInfo differentSiteReader =
      MakePrincipalInfo("https://unrelated.test"_ns);

  CrossOriginStorageRegistry::ReadOutcome sameSiteOutcome, differentSiteOutcome;
  RunOnBackgroundThread(NS_NewRunnableFunction(__func__, [&] {
    auto& registry = CrossOriginStorageRegistry::GetOrCreate();
    registry.CompleteCreateRequest(COSHashAlgorithm::SHA256, value, writer);
    COSRequestedOriginsValue requestedOrigins;  // SameSiteOnly (default).
    MOZ_ALWAYS_SUCCEEDS(registry.VerifyAndStore(
        COSHashAlgorithm::SHA256, value, bytes, writer, requestedOrigins));

    sameSiteOutcome = registry.CompleteReadRequest(COSHashAlgorithm::SHA256,
                                                   value, sameSiteReader);
    differentSiteOutcome = registry.CompleteReadRequest(
        COSHashAlgorithm::SHA256, value, differentSiteReader);
  }));

  EXPECT_EQ(sameSiteOutcome, CrossOriginStorageRegistry::ReadOutcome::Found);
  EXPECT_EQ(differentSiteOutcome,
            CrossOriginStorageRegistry::ReadOutcome::NotFound);
}

TEST_F(CrossOriginStorageRegistryTest, ListScopeDisclosesOnlyToListedOrigins) {
  nsTArray<uint8_t> bytes = UniqueContentFor(CurrentTestName());
  nsAutoCString value = HashOf(bytes);
  PrincipalInfo writer = MakePrincipalInfo("https://writer.example.com"_ns);
  PrincipalInfo listedReader =
      MakePrincipalInfo("https://listed.example.org"_ns);
  PrincipalInfo unlistedReader =
      MakePrincipalInfo("https://unlisted.example.org"_ns);

  CrossOriginStorageRegistry::ReadOutcome listedOutcome, unlistedOutcome;
  RunOnBackgroundThread(NS_NewRunnableFunction(__func__, [&] {
    auto& registry = CrossOriginStorageRegistry::GetOrCreate();
    registry.CompleteCreateRequest(COSHashAlgorithm::SHA256, value, writer);
    COSRequestedOriginsValue requestedOrigins;
    requestedOrigins.mKind = COSRequestedOriginsValue::Kind::List;
    requestedOrigins.mList.AppendElement("https://listed.example.org"_ns);
    MOZ_ALWAYS_SUCCEEDS(registry.VerifyAndStore(
        COSHashAlgorithm::SHA256, value, bytes, writer, requestedOrigins));

    listedOutcome = registry.CompleteReadRequest(COSHashAlgorithm::SHA256,
                                                 value, listedReader);
    unlistedOutcome = registry.CompleteReadRequest(COSHashAlgorithm::SHA256,
                                                   value, unlistedReader);
  }));

  EXPECT_EQ(listedOutcome, CrossOriginStorageRegistry::ReadOutcome::Found);
  EXPECT_EQ(unlistedOutcome, CrossOriginStorageRegistry::ReadOutcome::NotFound);
}

// https://wicg.github.io/cross-origin-storage/#resource-visibility-upgrades
// Scope only ever gets *more* permissive across successive writes of the
// same hash, never less -- same-site-only < list < wildcard, and a later
// write requesting a narrower scope than what's already granted is a
// silent no-op for scope purposes, not a downgrade.
//
// Wildcard's *actual* disclosure additionally requires the hash to be on
// the Public Hash List, or an independent GREASE roll (see
// CrossOriginStorageRegistry::CompleteReadRequest's Scope::Kind::Wildcard
// case, and CrossOriginStorageGrease::ShouldDisclose's own comment on why
// that roll is deliberately unpredictable) -- neither of which a
// fabricated test hash can deterministically satisfy. So this test
// exercises the list -> wildcard upgrade code path (catching crashes/
// state corruption) but only asserts *deterministic* properties around
// it: same-site-only -> list disclosure, list never silently reverting
// to same-site-only, and original-storer access (scope-independent)
// surviving every later write. Wildcard's own disclosure gate is covered
// by CrossOriginStoragePublicHashList's dedicated tests instead.
TEST_F(CrossOriginStorageRegistryTest,
       ScopeUpgradesMonotonicallyAndNeverDowngrades) {
  nsTArray<uint8_t> bytes = UniqueContentFor(CurrentTestName());
  nsAutoCString value = HashOf(bytes);
  PrincipalInfo writerA = MakePrincipalInfo("https://writer-a.example.com"_ns);
  PrincipalInfo writerB = MakePrincipalInfo("https://writer-b.example.org"_ns);
  PrincipalInfo writerC = MakePrincipalInfo("https://writer-c.example.net"_ns);
  PrincipalInfo listedReader =
      MakePrincipalInfo("https://listed.example.net"_ns);

  CrossOriginStorageRegistry::ReadOutcome afterListUpgrade;
  CrossOriginStorageRegistry::ReadOutcome afterAttemptedDowngradeToSameSiteOnly;
  CrossOriginStorageRegistry::ReadOutcome writerCAfterWildcardUpgrade;
  CrossOriginStorageRegistry::ReadOutcome writerCAfterSecondAttemptedDowngrade;
  RunOnBackgroundThread(NS_NewRunnableFunction(__func__, [&] {
    auto& registry = CrossOriginStorageRegistry::GetOrCreate();

    // First write: default same-site-only scope.
    registry.CompleteCreateRequest(COSHashAlgorithm::SHA256, value, writerA);
    COSRequestedOriginsValue sameSiteOnly;
    MOZ_ALWAYS_SUCCEEDS(registry.VerifyAndStore(COSHashAlgorithm::SHA256, value,
                                                bytes, writerA, sameSiteOnly));

    // Second write, same content/hash, different origin, requests a list
    // scope naming a reader -- upgrades same-site-only -> list.
    registry.CompleteCreateRequest(COSHashAlgorithm::SHA256, value, writerB);
    COSRequestedOriginsValue listScope;
    listScope.mKind = COSRequestedOriginsValue::Kind::List;
    listScope.mList.AppendElement("https://listed.example.net"_ns);
    MOZ_ALWAYS_SUCCEEDS(registry.VerifyAndStore(COSHashAlgorithm::SHA256, value,
                                                bytes, writerB, listScope));
    afterListUpgrade = registry.CompleteReadRequest(COSHashAlgorithm::SHA256,
                                                    value, listedReader);

    // Third write from writerA again, requesting only same-site-only (the
    // narrowest scope) -- must NOT downgrade the already-list-scoped
    // entry: the listed reader must still be disclosed to afterward.
    registry.CompleteCreateRequest(COSHashAlgorithm::SHA256, value, writerA);
    MOZ_ALWAYS_SUCCEEDS(registry.VerifyAndStore(COSHashAlgorithm::SHA256, value,
                                                bytes, writerA, sameSiteOnly));
    afterAttemptedDowngradeToSameSiteOnly = registry.CompleteReadRequest(
        COSHashAlgorithm::SHA256, value, listedReader);

    // Fourth write, requests wildcard -- upgrades list -> wildcard.
    registry.CompleteCreateRequest(COSHashAlgorithm::SHA256, value, writerC);
    COSRequestedOriginsValue wildcardScope;
    wildcardScope.mKind = COSRequestedOriginsValue::Kind::Wildcard;
    MOZ_ALWAYS_SUCCEEDS(registry.VerifyAndStore(COSHashAlgorithm::SHA256, value,
                                                bytes, writerC, wildcardScope));
    writerCAfterWildcardUpgrade =
        registry.CompleteReadRequest(COSHashAlgorithm::SHA256, value, writerC);

    // Fifth write from writerA again, requesting same-site-only -- must
    // not downgrade the already-wildcard entry either. Re-check via
    // writerC's own storer access, the one disclosure path that's
    // unaffected either way by the PHL/GREASE gate.
    registry.CompleteCreateRequest(COSHashAlgorithm::SHA256, value, writerA);
    MOZ_ALWAYS_SUCCEEDS(registry.VerifyAndStore(COSHashAlgorithm::SHA256, value,
                                                bytes, writerA, sameSiteOnly));
    writerCAfterSecondAttemptedDowngrade =
        registry.CompleteReadRequest(COSHashAlgorithm::SHA256, value, writerC);
  }));

  EXPECT_EQ(afterListUpgrade, CrossOriginStorageRegistry::ReadOutcome::Found)
      << "list scope should disclose to the newly-listed origin";
  EXPECT_EQ(afterAttemptedDowngradeToSameSiteOnly,
            CrossOriginStorageRegistry::ReadOutcome::Found)
      << "a later same-site-only write must not downgrade an "
         "already-list-scoped entry";
  EXPECT_EQ(writerCAfterWildcardUpgrade,
            CrossOriginStorageRegistry::ReadOutcome::Found)
      << "the wildcard writer's own original-storer access must work "
         "regardless of the PHL/GREASE gate";
  EXPECT_EQ(writerCAfterSecondAttemptedDowngrade,
            CrossOriginStorageRegistry::ReadOutcome::Found)
      << "a later same-site-only write must not disrupt an already-wildcard "
         "entry";
}

// See CrossOriginStorageRegistry::RemoveSite()'s own header comment and
// the Firefox implementation notes' §16: an entry with exactly one
// storing origin is deleted outright once that site is revoked (nothing
// legitimately references it anymore); this is the "GC" half of
// revoke-and-GC.
TEST_F(CrossOriginStorageRegistryTest, RemoveSiteDeletesASoleOwnedEntry) {
  nsTArray<uint8_t> bytes = UniqueContentFor(CurrentTestName());
  nsAutoCString value = HashOf(bytes);
  PrincipalInfo writer = MakePrincipalInfo("https://sole-owner.example.com"_ns);

  CrossOriginStorageRegistry::ReadOutcome outcomeBeforeRemoval;
  CrossOriginStorageRegistry::ReadOutcome outcomeAfterRemoval;
  RunOnBackgroundThread(NS_NewRunnableFunction(__func__, [&] {
    auto& registry = CrossOriginStorageRegistry::GetOrCreate();
    registry.CompleteCreateRequest(COSHashAlgorithm::SHA256, value, writer);
    COSRequestedOriginsValue requestedOrigins;
    MOZ_ALWAYS_SUCCEEDS(registry.VerifyAndStore(
        COSHashAlgorithm::SHA256, value, bytes, writer, requestedOrigins));
    outcomeBeforeRemoval =
        registry.CompleteReadRequest(COSHashAlgorithm::SHA256, value, writer);

    // GetSite() computes scheme + eTLD+1 ("baseDomain"), so
    // https://sole-owner.example.com's actual site is example.com, not
    // the full host -- RemoveSite() takes a site, matching
    // nsIClearDataService::deleteBySite()'s own convention.
    registry.RemoveSite("example.com"_ns);

    outcomeAfterRemoval =
        registry.CompleteReadRequest(COSHashAlgorithm::SHA256, value, writer);
  }));

  EXPECT_EQ(outcomeBeforeRemoval,
            CrossOriginStorageRegistry::ReadOutcome::Found);
  EXPECT_EQ(outcomeAfterRemoval,
            CrossOriginStorageRegistry::ReadOutcome::NotFound)
      << "a sole-owned entry must be fully deleted once its only storing site "
         "is removed";
}

// The other half of revoke-and-GC: an entry a *different*, uncleared
// site also legitimately stored (content-addressability means two
// unrelated origins can both "write" the same bytes -- see the
// implementation notes' §2) must survive a site-scoped clear naming only
// one of them.
TEST_F(CrossOriginStorageRegistryTest,
       RemoveSitePreservesAMultiOwnedEntryForTheOtherOwner) {
  nsTArray<uint8_t> bytes = UniqueContentFor(CurrentTestName());
  nsAutoCString value = HashOf(bytes);
  PrincipalInfo ownerA = MakePrincipalInfo("https://owner-a.example.com"_ns);
  PrincipalInfo ownerB = MakePrincipalInfo("https://owner-b.example.org"_ns);

  CrossOriginStorageRegistry::ReadOutcome ownerAOutcomeAfterRemoval;
  CrossOriginStorageRegistry::ReadOutcome ownerBOutcomeAfterRemoval;
  RunOnBackgroundThread(NS_NewRunnableFunction(__func__, [&] {
    auto& registry = CrossOriginStorageRegistry::GetOrCreate();
    COSRequestedOriginsValue requestedOrigins;

    registry.CompleteCreateRequest(COSHashAlgorithm::SHA256, value, ownerA);
    MOZ_ALWAYS_SUCCEEDS(registry.VerifyAndStore(
        COSHashAlgorithm::SHA256, value, bytes, ownerA, requestedOrigins));
    // Independently "written" again by a genuinely different origin --
    // same hash, same entry, a second storing origin, not a copy.
    registry.CompleteCreateRequest(COSHashAlgorithm::SHA256, value, ownerB);
    MOZ_ALWAYS_SUCCEEDS(registry.VerifyAndStore(
        COSHashAlgorithm::SHA256, value, bytes, ownerB, requestedOrigins));

    // As above, RemoveSite() takes a site (scheme + eTLD+1), not a full
    // host -- owner-a.example.com's actual site is example.com.
    registry.RemoveSite("example.com"_ns);

    // Owner A's own automatic storing-origin access is gone (their site
    // no longer matches anything on the entry, and they're not on any
    // disclosure list either) -- a plain read from A now depends on
    // ordinary same-site-only disclosure against B's site, which A's
    // site doesn't match.
    ownerAOutcomeAfterRemoval =
        registry.CompleteReadRequest(COSHashAlgorithm::SHA256, value, ownerA);
    // Owner B's own storing-origin access is untouched.
    ownerBOutcomeAfterRemoval =
        registry.CompleteReadRequest(COSHashAlgorithm::SHA256, value, ownerB);
  }));

  EXPECT_EQ(ownerAOutcomeAfterRemoval,
            CrossOriginStorageRegistry::ReadOutcome::NotFound)
      << "the removed site's own storer access must be revoked";
  EXPECT_EQ(ownerBOutcomeAfterRemoval,
            CrossOriginStorageRegistry::ReadOutcome::Found)
      << "a different, uncleared site's legitimate copy must survive";
}

TEST_F(CrossOriginStorageRegistryTest, ClearAllRemovesEveryEntry) {
  nsTArray<uint8_t> bytesA = UniqueContentFor("ClearAllRemovesEveryEntry-A");
  nsTArray<uint8_t> bytesB = UniqueContentFor("ClearAllRemovesEveryEntry-B");
  nsAutoCString valueA = HashOf(bytesA);
  nsAutoCString valueB = HashOf(bytesB);
  PrincipalInfo writer = MakePrincipalInfo("https://writer.example.com"_ns);

  CrossOriginStorageRegistry::ReadOutcome outcomeA, outcomeB;
  RunOnBackgroundThread(NS_NewRunnableFunction(__func__, [&] {
    auto& registry = CrossOriginStorageRegistry::GetOrCreate();
    COSRequestedOriginsValue requestedOrigins;

    registry.CompleteCreateRequest(COSHashAlgorithm::SHA256, valueA, writer);
    MOZ_ALWAYS_SUCCEEDS(registry.VerifyAndStore(
        COSHashAlgorithm::SHA256, valueA, bytesA, writer, requestedOrigins));
    registry.CompleteCreateRequest(COSHashAlgorithm::SHA256, valueB, writer);
    MOZ_ALWAYS_SUCCEEDS(registry.VerifyAndStore(
        COSHashAlgorithm::SHA256, valueB, bytesB, writer, requestedOrigins));

    registry.ClearAll();

    outcomeA =
        registry.CompleteReadRequest(COSHashAlgorithm::SHA256, valueA, writer);
    outcomeB =
        registry.CompleteReadRequest(COSHashAlgorithm::SHA256, valueB, writer);
  }));

  EXPECT_EQ(outcomeA, CrossOriginStorageRegistry::ReadOutcome::NotFound);
  EXPECT_EQ(outcomeB, CrossOriginStorageRegistry::ReadOutcome::NotFound);
}
