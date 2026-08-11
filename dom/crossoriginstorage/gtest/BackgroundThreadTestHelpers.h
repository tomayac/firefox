/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_crossoriginstorage_gtest_BackgroundThreadTestHelpers_h
#define mozilla_dom_crossoriginstorage_gtest_BackgroundThreadTestHelpers_h

#include "gtest/gtest.h"
#include "mozilla/SyncRunnable.h"
#include "mozilla/ipc/BackgroundChild.h"
#include "mozilla/ipc/BackgroundParent.h"
#include "nsThreadUtils.h"
#include "nss.h"

// Shared by every dom/crossoriginstorage/gtest/Test*.cpp file that needs
// to call into a PBackground-thread-only singleton
// (CrossOriginStorageRegistry, CrossOriginStorageRateLimiter). `inline`
// deliberately, not `static`/anonymous-namespace: UNIFIED_SOURCES
// concatenates every gtest .cpp in this directory into one real
// translation unit, so two files each defining their own same-named
// anonymous-namespace helper collide as a redefinition -- `inline`
// functions are explicitly permitted to appear identically in more than
// one place for exactly this reason.

namespace mozilla::dom::crossoriginstorage_gtest {

inline void EnsureBackgroundThreadStarted() {
  // BackgroundChild::GetOrCreateForCurrentThread() is documented as
  // synchronous: it either returns an existing actor or creates one (and,
  // in-process, that creation also spins up the parent-side PBackground
  // thread these tests actually need) before returning. Self-contained --
  // doesn't rely on some other, earlier test in the same gtest binary
  // having already triggered this as a side effect.
  mozilla::ipc::PBackgroundChild* child =
      mozilla::ipc::BackgroundChild::GetOrCreateForCurrentThread();
  ASSERT_TRUE(child);
  // GetBackgroundThread() returns already_AddRefed<>, which deliberately
  // has no bool conversion -- assign it to check.
  nsCOMPtr<nsISerialEventTarget> backgroundThread =
      mozilla::ipc::BackgroundParent::GetBackgroundThread();
  ASSERT_TRUE(backgroundThread);
}

// Outside a real browser session nothing ever triggers PSM's normal
// nsNSSComponent startup observer, so raw PK11 calls (e.g.
// ComputeHashValueHex, and principal handling that touches NSS) fail with
// no NSS context -- mirrors security/manager/ssl/tests/gtest/HMACTest.cpp's
// own SetUp(). Safe to call more than once: the guard makes the real
// NSS_NoDB_Init() call happen exactly once per process regardless of how
// many test fixtures in this binary call this.
inline void EnsureNSSInitializedForTest() {
  static bool sInitialized = [] {
    NSS_NoDB_Init(nullptr);
    return true;
  }();
  (void)sInitialized;
}

inline void RunOnBackgroundThread(
    already_AddRefed<mozilla::Runnable> aRunnable) {
  nsCOMPtr<nsISerialEventTarget> backgroundThread =
      mozilla::ipc::BackgroundParent::GetBackgroundThread();
  ASSERT_TRUE(backgroundThread);
  MOZ_ALWAYS_SUCCEEDS(mozilla::SyncRunnable::DispatchToThread(
      backgroundThread, std::move(aRunnable)));
}

// The current TEST()/TEST_F()'s actual name, e.g. for deriving a unique
// hash/origin per test against a shared, process-wide singleton (see
// each Test*.cpp file's own comment on this). Deliberately *not*
// `__func__`: GoogleTest's TEST_F() macro expands to a method literally
// named `TestBody()`, so `__func__` inside any test body is always the
// same four characters regardless of which test is actually running --
// a real bug this suite shipped with initially, silently breaking every
// test's assumed isolation until caught by a spuriously-failing rate
// limiter test.
inline const char* CurrentTestName() {
  return ::testing::UnitTest::GetInstance()->current_test_info()->name();
}

}  // namespace mozilla::dom::crossoriginstorage_gtest

#endif  // mozilla_dom_crossoriginstorage_gtest_BackgroundThreadTestHelpers_h
