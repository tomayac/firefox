/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_CrossOriginStorageRateLimiter_h
#define mozilla_dom_CrossOriginStorageRateLimiter_h

#include "mozilla/TimeStamp.h"
#include "nsClassHashtable.h"
#include "nsString.h"

namespace mozilla::dom {

// Per-origin token-bucket rate limiting for COS reads and writes. Not
// spec-mandated in its exact shape, but real implementations need
// something here to bound how fast an origin can probe the registry
// (e.g. for the hash-guessing/timing attacks the spec's own security
// considerations section flags) -- burst/refill constants below match
// Servo's own chosen starting point.
//
// A single PBackground-thread-only singleton, like
// CrossOriginStorageRegistry; see that class for the threading rationale.
// Deliberately a distinct bounded map from the registry's own entry
// storage -- an unbounded origin-keyed map here would itself be a memory-
// exhaustion vector independent of how COS entries are stored/evicted.
class CrossOriginStorageRateLimiter {
 public:
  static CrossOriginStorageRateLimiter& GetOrCreate();

  enum class Kind { Read, Write };

  // Returns false if aOrigin is currently over budget for aKind, in which
  // case the caller MUST treat this identically to a genuine cache miss
  // (NotFound) -- never a distinguishable error or timing difference, so
  // a rate-limited probe can't be told apart from a real one.
  bool TryConsume(const nsACString& aOrigin, Kind aKind);

 private:
  CrossOriginStorageRateLimiter() = default;

  struct Bucket {
    double mReadTokens = 0;
    double mWriteTokens = 0;
    TimeStamp mLastRefill;
    // Independent of mLastRefill: read/write activity both refresh this,
    // but a token-exhausted-yet-idle origin should still be evictable.
    TimeStamp mLastTouched;
  };

  // A fixed cap on distinct tracked origins, LRU-evicted by mLastTouched
  // once exceeded -- deliberately naive (a linear scan on insert past the
  // cap) since this phase doesn't expect to realistically approach it.
  static constexpr uint32_t kMaxTrackedOrigins = 10000;

  nsClassHashtable<nsCStringHashKey, Bucket> mBuckets;
};

}  // namespace mozilla::dom

#endif  // mozilla_dom_CrossOriginStorageRateLimiter_h
