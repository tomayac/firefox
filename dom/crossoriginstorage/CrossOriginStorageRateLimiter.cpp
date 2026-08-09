/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CrossOriginStorageRateLimiter.h"

#include <algorithm>

#include "mozilla/ipc/BackgroundParent.h"

namespace mozilla::dom {

namespace {
// Not spec-mandated; matches Servo's own chosen starting point.
constexpr double kReadBurst = 2000;
constexpr double kReadRefillPerSecond = 20;
constexpr double kWriteBurst = 200;
constexpr double kWriteRefillPerSecond = 2;
}  // namespace

/* static */
CrossOriginStorageRateLimiter& CrossOriginStorageRateLimiter::GetOrCreate() {
  mozilla::ipc::AssertIsOnBackgroundThread();
  static CrossOriginStorageRateLimiter sInstance;
  return sInstance;
}

bool CrossOriginStorageRateLimiter::TryConsume(const nsACString& aOrigin,
                                               Kind aKind) {
  mozilla::ipc::AssertIsOnBackgroundThread();

  Bucket* bucket = mBuckets.Get(aOrigin);
  if (!bucket) {
    if (mBuckets.Count() >= kMaxTrackedOrigins) {
      // Evict the least-recently-touched origin to keep the map itself
      // bounded, independent of how COS entries are stored/evicted.
      nsCString oldestKey;
      TimeStamp oldest;
      bool found = false;
      for (auto iter = mBuckets.ConstIter(); !iter.Done(); iter.Next()) {
        if (!found || iter.Data()->mLastTouched < oldest) {
          oldest = iter.Data()->mLastTouched;
          oldestKey = iter.Key();
          found = true;
        }
      }
      if (found) {
        mBuckets.Remove(oldestKey);
      }
    }
    bucket = mBuckets.GetOrInsertNew(aOrigin);
    bucket->mReadTokens = kReadBurst;
    bucket->mWriteTokens = kWriteBurst;
    bucket->mLastRefill = TimeStamp::Now();
  }

  TimeStamp now = TimeStamp::Now();
  double elapsedSeconds = (now - bucket->mLastRefill).ToSeconds();
  bucket->mReadTokens = std::min(
      kReadBurst, bucket->mReadTokens + elapsedSeconds * kReadRefillPerSecond);
  bucket->mWriteTokens =
      std::min(kWriteBurst,
               bucket->mWriteTokens + elapsedSeconds * kWriteRefillPerSecond);
  bucket->mLastRefill = now;
  bucket->mLastTouched = now;

  double& tokens =
      aKind == Kind::Read ? bucket->mReadTokens : bucket->mWriteTokens;
  if (tokens < 1.0) {
    return false;
  }
  tokens -= 1.0;
  return true;
}

}  // namespace mozilla::dom
