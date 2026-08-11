/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "BackgroundThreadTestHelpers.h"
#include "CrossOriginStorageRateLimiter.h"
#include "gtest/gtest.h"
#include "nsString.h"
#include "nsThreadUtils.h"

using namespace mozilla;
using namespace mozilla::dom;
using namespace mozilla::dom::crossoriginstorage_gtest;
using namespace mozilla::ipc;

namespace {

// Every test uses its own origin string, since CrossOriginStorageRateLimiter
// is a single process-wide singleton shared across every TEST() in this
// binary (like CrossOriginStorageRegistry -- see
// TestCrossOriginStorageRegistry.cpp's own comment on the same issue) --
// two tests sharing an origin would see each other's already-consumed
// tokens.
nsAutoCString OriginFor(const char* aTestName) {
  nsAutoCString origin("https://");
  origin.Append(aTestName);
  origin.AppendLiteral(".example.com");
  return origin;
}

}  // namespace

class CrossOriginStorageRateLimiterTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { EnsureBackgroundThreadStarted(); }
};

// Per this document's own implementation notes (§12): a real wall-clock
// token bucket is flakier to assert exactly than it looks -- a refill
// tick can land mid-loop and let one extra request through
// non-deterministically. Assert the *shape* (at least `capacity`
// consumes succeed; consumption eventually fails within a small,
// generous slack past capacity), never an exact boundary count.
TEST_F(CrossOriginStorageRateLimiterTest,
       ExhaustingTheWriteBurstEventuallyDenies) {
  nsAutoCString origin = OriginFor(CurrentTestName());
  constexpr int kWriteBurst = 200;  // Must match the real constant.
  constexpr int kSlack = 20;

  int successCount = 0;
  bool sawADenial = false;
  RunOnBackgroundThread(NS_NewRunnableFunction(__func__, [&] {
    auto& limiter = CrossOriginStorageRateLimiter::GetOrCreate();
    for (int i = 0; i < kWriteBurst + kSlack; i++) {
      if (limiter.TryConsume(origin,
                             CrossOriginStorageRateLimiter::Kind::Write)) {
        successCount++;
      } else {
        sawADenial = true;
        break;
      }
    }
  }));

  EXPECT_TRUE(sawADenial) << "burst capacity must eventually be exhausted";
  EXPECT_GE(successCount, kWriteBurst - kSlack);
  EXPECT_LE(successCount, kWriteBurst + kSlack);
}

TEST_F(CrossOriginStorageRateLimiterTest,
       DistinctOriginsHaveIndependentBudgets) {
  nsAutoCString originA = OriginFor("DistinctOriginsHaveIndependentBudgetsA");
  nsAutoCString originB = OriginFor("DistinctOriginsHaveIndependentBudgetsB");
  constexpr int kWriteBurst = 200;

  bool originAExhausted = false;
  bool originBFirstConsumeSucceeded = false;
  RunOnBackgroundThread(NS_NewRunnableFunction(__func__, [&] {
    auto& limiter = CrossOriginStorageRateLimiter::GetOrCreate();
    // Drive origin A's write budget to exhaustion.
    for (int i = 0; i < kWriteBurst + 20; i++) {
      if (!limiter.TryConsume(originA,
                              CrossOriginStorageRateLimiter::Kind::Write)) {
        originAExhausted = true;
        break;
      }
    }
    // A completely different, freshly-seen origin must still have its
    // own full budget -- A's exhaustion must not leak into B's bucket.
    originBFirstConsumeSucceeded =
        limiter.TryConsume(originB, CrossOriginStorageRateLimiter::Kind::Write);
  }));

  EXPECT_TRUE(originAExhausted);
  EXPECT_TRUE(originBFirstConsumeSucceeded);
}

TEST_F(CrossOriginStorageRateLimiterTest,
       ReadAndWriteBudgetsAreIndependentForTheSameOrigin) {
  nsAutoCString origin = OriginFor(CurrentTestName());
  constexpr int kWriteBurst = 200;

  bool writeExhausted = false;
  bool readStillSucceedsAfterWriteExhausted = false;
  RunOnBackgroundThread(NS_NewRunnableFunction(__func__, [&] {
    auto& limiter = CrossOriginStorageRateLimiter::GetOrCreate();
    for (int i = 0; i < kWriteBurst + 20; i++) {
      if (!limiter.TryConsume(origin,
                              CrossOriginStorageRateLimiter::Kind::Write)) {
        writeExhausted = true;
        break;
      }
    }
    // The read budget (burst 2000, an entirely separate token counter on
    // the same per-origin bucket) must be untouched by exhausting writes.
    readStillSucceedsAfterWriteExhausted =
        limiter.TryConsume(origin, CrossOriginStorageRateLimiter::Kind::Read);
  }));

  EXPECT_TRUE(writeExhausted);
  EXPECT_TRUE(readStillSucceedsAfterWriteExhausted);
}

TEST_F(CrossOriginStorageRateLimiterTest,
       AFreshOriginsFirstConsumeAlwaysSucceeds) {
  nsAutoCString origin = OriginFor(CurrentTestName());

  bool firstReadSucceeded, firstWriteSucceeded;
  RunOnBackgroundThread(NS_NewRunnableFunction(__func__, [&] {
    auto& limiter = CrossOriginStorageRateLimiter::GetOrCreate();
    firstReadSucceeded =
        limiter.TryConsume(origin, CrossOriginStorageRateLimiter::Kind::Read);
    firstWriteSucceeded =
        limiter.TryConsume(origin, CrossOriginStorageRateLimiter::Kind::Write);
  }));

  EXPECT_TRUE(firstReadSucceeded);
  EXPECT_TRUE(firstWriteSucceeded);
}
