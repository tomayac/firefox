/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CrossOriginStorageRegistry.h"

#include <algorithm>

#include "CrossOriginStoragePublicHashList.h"
#include "CrossOriginStorageRateLimiter.h"
#include "mozilla/ipc/BackgroundParent.h"
#include "prtime.h"

namespace mozilla::dom {

using mozilla::ipc::PrincipalInfo;

namespace {

// Not spec-mandated; see the implementation notes' suggested starting
// value for reclaiming a write abandoned without an explicit close()/
// abort() -- e.g. a page that navigates away or crashes mid-write.
const TimeDuration kPendingStalenessTimeout = TimeDuration::FromSeconds(5 * 60);

// Matches Ladybird's own chosen split: a global budget of 60% of disk
// capacity, and a per-origin share of 20% of *that* (not of raw disk
// capacity). kMaxGlobalBudget is a sanity ceiling guarding against a
// misreported disk capacity producing an unreasonably large budget --
// the same defensive pattern QuotaManager's own budget computation uses
// (see dom/quota/ActorsParent.cpp).
constexpr double kGlobalBudgetFraction = 0.6;
constexpr double kPerOriginBudgetFraction = 0.2;
constexpr int64_t kMaxGlobalBudget = 100LL * 1024 * 1024 * 1024;  // 100 GiB

// This, and GetSite() below, work directly off the wire-format
// PrincipalInfo rather than constructing a real nsIPrincipal via
// PrincipalInfoToPrincipal(), which must be called on the main thread --
// everything in this registry runs on the PBackground thread instead.
// ContentPrincipalInfo's originNoSuffix and baseDomain fields are
// explicitly documented as safe to use off the main thread for exactly
// this kind of reason.

// The ASCII origin serialization of aPrincipalInfo, for exact-origin
// comparisons ("original storer access", list-scope membership).
nsresult GetOrigin(const PrincipalInfo& aPrincipalInfo,
                   nsACString& aOutOrigin) {
  switch (aPrincipalInfo.type()) {
    case PrincipalInfo::TContentPrincipalInfo:
      aOutOrigin = aPrincipalInfo.get_ContentPrincipalInfo().originNoSuffix();
      return NS_OK;
    case PrincipalInfo::TNullPrincipalInfo:
      // Opaque; using its own (per-instance-unique) spec means it never
      // equals any other origin, including another null principal's.
      aOutOrigin = aPrincipalInfo.get_NullPrincipalInfo().spec();
      return NS_OK;
    case PrincipalInfo::TSystemPrincipalInfo:
      aOutOrigin = "system:"_ns;
      return NS_OK;
    case PrincipalInfo::TExpandedPrincipalInfo:
    default:
      // Not a realistic caller of navigator.crossOriginStorage. Fail
      // closed (never matches any origin) rather than guess.
      return NS_ERROR_FAILURE;
  }
}

// The "site" (scheme + eTLD+1) of aPrincipalInfo, per HTML's "obtain a
// site". Two origins are same-site iff their sites are equal.
nsresult GetSite(const PrincipalInfo& aPrincipalInfo, nsACString& aOutSite) {
  switch (aPrincipalInfo.type()) {
    case PrincipalInfo::TContentPrincipalInfo: {
      const auto& info = aPrincipalInfo.get_ContentPrincipalInfo();
      int32_t schemeEnd = info.originNoSuffix().Find("://");
      if (schemeEnd < 0) {
        return NS_ERROR_FAILURE;
      }
      aOutSite =
          Substring(info.originNoSuffix(), 0, static_cast<size_t>(schemeEnd)) +
          "://"_ns + info.baseDomain();
      return NS_OK;
    }
    case PrincipalInfo::TNullPrincipalInfo:
      aOutSite = aPrincipalInfo.get_NullPrincipalInfo().spec();
      return NS_OK;
    case PrincipalInfo::TSystemPrincipalInfo:
      aOutSite = "system:"_ns;
      return NS_OK;
    case PrincipalInfo::TExpandedPrincipalInfo:
    default:
      return NS_ERROR_FAILURE;
  }
}

// Whether aSiteOrOrigin (an Entry::mStoringSites/mScope.mList entry --
// "scheme://host[:port]", or a bare "scheme://host") is aSchemelessSite
// or a subdomain of it, ignoring scheme and port. Mirrors
// nsIClearDataService::deleteBySite()'s own host-suffix convention
// (ClearDataService.sys.mjs's QuotaCleaner.deleteByHost has the same
// "foo.example.com matches example.com" semantics) rather than a real
// public-suffix-list eTLD+1 computation, which a plain string already
// carrying a resolved eTLD+1 (as every site/origin string here does)
// doesn't need anyway.
bool HostMatchesSchemelessSite(const nsACString& aSiteOrOrigin,
                               const nsACString& aSchemelessSite) {
  nsAutoCString host(aSiteOrOrigin);
  int32_t schemeEnd = host.Find("://");
  if (schemeEnd >= 0) {
    host = Substring(host, schemeEnd + 3);
  }
  int32_t portColon = host.Find(":");
  if (portColon >= 0) {
    host = Substring(host, 0, portColon);
  }
  if (host.Equals(aSchemelessSite)) {
    return true;
  }
  return host.Length() > aSchemelessSite.Length() &&
         StringEndsWith(host, aSchemelessSite) &&
         host.CharAt(host.Length() - aSchemelessSite.Length() - 1) == '.';
}

}  // namespace

bool CrossOriginStorageRegistry::Entry::IsStale() const {
  return mState == State::Pending &&
         (TimeStamp::Now() - mPendingSince) > kPendingStalenessTimeout;
}

/* static */
nsAutoCString CrossOriginStorageRegistry::MakeKey(COSHashAlgorithm aAlgorithm,
                                                  const nsACString& aValue) {
  nsAutoCString key;
  key.Append(CanonicalHashAlgorithmName(aAlgorithm));
  key.Append(':');
  key.Append(aValue);
  return key;
}

/* static */
bool CrossOriginStorageRegistry::SplitKey(const nsACString& aKey,
                                          COSHashAlgorithm* aOutAlgorithm,
                                          nsACString& aOutValue) {
  int32_t colon = aKey.Find(":");
  if (colon < 0) {
    aOutValue.Truncate();
    return false;
  }
  nsAutoCString algorithmName(Substring(aKey, 0, colon));
  Maybe<COSHashAlgorithm> algorithm =
      ParseHashAlgorithm(NS_ConvertUTF8toUTF16(algorithmName));
  if (!algorithm) {
    aOutValue.Truncate();
    return false;
  }
  *aOutAlgorithm = *algorithm;
  aOutValue = Substring(aKey, colon + 1);
  return true;
}

CrossOriginStorageRegistry::CrossOriginStorageRegistry() {
  LoadPersistedEntries();
}

/* static */
CrossOriginStorageRegistry& CrossOriginStorageRegistry::GetOrCreate() {
  mozilla::ipc::AssertIsOnBackgroundThread();
  static CrossOriginStorageRegistry sInstance;
  return sInstance;
}

void CrossOriginStorageRegistry::LoadPersistedEntries() {
  CrossOriginStoragePersistence* persistence =
      CrossOriginStoragePersistence::GetOrCreate();
  if (!persistence) {
    return;
  }

  nsTArray<CrossOriginStoragePersistence::PersistedEntry> persisted;
  persistence->ScanPersistedEntries(persisted);

  for (auto& p : persisted) {
    Maybe<COSHashAlgorithm> algorithm =
        ParseHashAlgorithm(NS_ConvertUTF8toUTF16(p.mAlgorithm));
    if (!algorithm) {
      continue;  // Written by a future version with a new algorithm this
                 // build doesn't recognize; skip rather than guess.
    }

    nsAutoCString key = MakeKey(*algorithm, p.mValue);
    Entry* entry = mEntries.GetOrInsertNew(key);
    entry->mState = Entry::State::Written;
    entry->mScope.mKind = static_cast<Scope::Kind>(p.mScopeKind);
    entry->mScope.mList = p.mScopeList.Clone();
    entry->mStoringOrigins = p.mStoringOrigins.Clone();
    entry->mStoringSites = p.mStoringSites.Clone();
    entry->mByteSize = p.mByteSize;
    entry->mLastReadTime = p.mLastReadTime;
    entry->mBytesOnDisk = true;

    mTotalBytesUsed += p.mByteSize;
    if (!p.mStoringOrigins.IsEmpty()) {
      uint64_t* usage = mOriginUsage.GetOrInsertNew(p.mStoringOrigins[0]);
      *usage += p.mByteSize;
    }
  }
}

CrossOriginStorageRegistry::ReadOutcome
CrossOriginStorageRegistry::CompleteReadRequest(
    COSHashAlgorithm aAlgorithm, const nsACString& aValue,
    const PrincipalInfo& aRequestingPrincipal) {
  mozilla::ipc::AssertIsOnBackgroundThread();

  // Rate limiting is checked before anything else, and a rejection here
  // is NotFound -- the same outcome a genuine miss produces -- so a
  // rate-limited probe can't be distinguished from an ordinary one.
  nsAutoCString requestingOrigin;
  bool haveOrigin =
      NS_SUCCEEDED(GetOrigin(aRequestingPrincipal, requestingOrigin));
  if (haveOrigin &&
      !CrossOriginStorageRateLimiter::GetOrCreate().TryConsume(
          requestingOrigin, CrossOriginStorageRateLimiter::Kind::Read)) {
    return ReadOutcome::NotFound;
  }

  nsAutoCString key = MakeKey(aAlgorithm, aValue);

  // Step 1.
  Entry* entry = mEntries.Get(key);
  if (!entry) {
    return ReadOutcome::NotFound;
  }

  // Step 2.
  if (entry->mState == Entry::State::Pending) {
    if (entry->IsStale()) {
      mEntries.Remove(key);
      return ReadOutcome::NotFound;
    }
    return ReadOutcome::Pending;
  }

  // https://wicg.github.io/cross-origin-storage/#determine-cos-disclosure
  // and https://wicg.github.io/cross-origin-storage/#apply-availability-gating

  // Step 3: original storer access, independent of scope.
  bool found = false;
  if (haveOrigin && entry->mStoringOrigins.Contains(requestingOrigin)) {
    found = true;
  } else {
    switch (entry->mScope.mKind) {
      case Scope::Kind::Wildcard:
        // Step 4: the Public Hash List gate, or (independently) a
        // GREASE'd roll -- see CrossOriginStoragePublicHashList.h.
        found =
            CrossOriginStoragePublicHashList::Contains(aAlgorithm, aValue) ||
            CrossOriginStorageGrease::ShouldDisclose(entry->mByteSize);
        break;

      case Scope::Kind::List: {
        // Step 5.
        if (haveOrigin) {
          int32_t index = entry->mScope.mList.IndexOf(requestingOrigin);
          if (index >= 0) {
            // Implementation notes §4: a successful read by a listed
            // origin refreshes its LRU recency (move to the
            // most-recently-used end); merely re-declaring it in a later
            // write does not.
            entry->mScope.mList.RemoveElementAt(index);
            entry->mScope.mList.AppendElement(requestingOrigin);
            found = true;
          }
        }
        break;
      }

      case Scope::Kind::SameSiteOnly: {
        // Step 7.
        nsAutoCString requestingSite;
        if (NS_SUCCEEDED(GetSite(aRequestingPrincipal, requestingSite)) &&
            entry->mStoringSites.Contains(requestingSite)) {
          found = true;
        }
        break;
      }
    }
  }

  if (!found) {
    return ReadOutcome::NotFound;
  }
  entry->mLastReadTime = PR_Now();
  return ReadOutcome::Found;
}

bool CrossOriginStorageRegistry::CompleteCreateRequest(
    COSHashAlgorithm aAlgorithm, const nsACString& aValue,
    const PrincipalInfo& aWritingPrincipal) {
  mozilla::ipc::AssertIsOnBackgroundThread();

  nsAutoCString writingOrigin;
  bool haveOrigin = NS_SUCCEEDED(GetOrigin(aWritingPrincipal, writingOrigin));
  if (haveOrigin &&
      !CrossOriginStorageRateLimiter::GetOrCreate().TryConsume(
          writingOrigin, CrossOriginStorageRateLimiter::Kind::Write)) {
    // Silently refuse to create/touch any entry, and report "not written"
    // -- identical to an ordinary fresh pending creation's return value.
    // The eventual close() for this write will find no entry and fail
    // generically, rather than this request immediately surfacing a
    // distinguishable "you are rate limited" signal.
    return false;
  }

  nsAutoCString key = MakeKey(aAlgorithm, aValue);

  Entry* entry = mEntries.Get(key);
  if (entry && entry->IsStale()) {
    // A new writer silently replaces an abandoned pending entry, discarding
    // its outstanding-writer count along with the rest of its now-
    // irrelevant state -- nothing is still genuinely writing it.
    mEntries.Remove(key);
    entry = nullptr;
  }

  if (!entry) {
    entry = mEntries.GetOrInsertNew(key);
    entry->mPendingSince = TimeStamp::Now();
  }

  entry->mPendingWriterCount++;
  return entry->mState == Entry::State::Written;
}

nsresult CrossOriginStorageRegistry::VerifyAndStore(
    COSHashAlgorithm aAlgorithm, const nsACString& aValue,
    const nsTArray<uint8_t>& aBytes, const PrincipalInfo& aWritingPrincipal,
    const COSRequestedOriginsValue& aRequestedOrigins) {
  mozilla::ipc::AssertIsOnBackgroundThread();

  nsAutoCString key = MakeKey(aAlgorithm, aValue);
  Entry* entry = mEntries.Get(key);
  // The caller only reaches VerifyAndStore for a write session opened via
  // CompleteCreateRequest, which always leaves an entry behind.
  MOZ_ASSERT(entry);
  if (!entry) {
    return NS_ERROR_UNEXPECTED;
  }

  // Step 1.
  nsAutoCString computedValue;
  nsresult rv = ComputeHashValueHex(aAlgorithm, aBytes, computedValue);
  if (NS_FAILED(rv)) {
    return rv;
  }

  // Step 2.
  if (!computedValue.Equals(aValue)) {
    ReleaseOutstandingWriter(aAlgorithm, aValue);
    return NS_ERROR_DOM_DATA_ERR;
  }

  // Step 3.
  bool wasAlreadyWritten = entry->mState == Entry::State::Written;
  entry->mState = Entry::State::Written;
  entry->mByteSize = aBytes.Length();
  entry->mLastReadTime = PR_Now();

  nsAutoCString writingOrigin;
  bool haveWritingOrigin =
      NS_SUCCEEDED(GetOrigin(aWritingPrincipal, writingOrigin));
  if (haveWritingOrigin && !entry->mStoringOrigins.Contains(writingOrigin)) {
    entry->mStoringOrigins.AppendElement(writingOrigin);
  }
  nsAutoCString writingSite;
  if (NS_SUCCEEDED(GetSite(aWritingPrincipal, writingSite)) &&
      !entry->mStoringSites.Contains(writingSite)) {
    entry->mStoringSites.AppendElement(writingSite);
  }

  if (entry->mPendingWriterCount > 0) {
    entry->mPendingWriterCount--;
  }

  UpgradeResourceVisibility(*entry, aRequestedOrigins);

  // Persist to disk (bytes plus the final, post-upgrade metadata). Falls
  // back to keeping the bytes resident in memory -- Phase 1's original
  // behavior -- if persistence is unavailable or the write itself fails.
  CrossOriginStoragePersistence* persistence =
      CrossOriginStoragePersistence::GetOrCreate();
  bool persisted = false;
  if (persistence) {
    CrossOriginStoragePersistence::PersistedEntry meta;
    meta.mAlgorithm = CanonicalHashAlgorithmName(aAlgorithm);
    meta.mValue = aValue;
    meta.mScopeKind = static_cast<uint32_t>(entry->mScope.mKind);
    meta.mScopeList = entry->mScope.mList.Clone();
    meta.mStoringOrigins = entry->mStoringOrigins.Clone();
    meta.mStoringSites = entry->mStoringSites.Clone();
    meta.mByteSize = entry->mByteSize;
    meta.mLastReadTime = entry->mLastReadTime;
    persisted = persistence->WriteEntry(aAlgorithm, aValue, aBytes, meta);
  }
  if (persisted) {
    entry->mBytesOnDisk = true;
    entry->mBytes.Clear();
  } else {
    entry->mBytesOnDisk = false;
    entry->mBytes = aBytes.Clone();
  }

  if (!wasAlreadyWritten) {
    mTotalBytesUsed += entry->mByteSize;
    if (haveWritingOrigin) {
      uint64_t* usage = mOriginUsage.GetOrInsertNew(writingOrigin);
      *usage += entry->mByteSize;
    }
  }

  if (haveWritingOrigin) {
    EnforceStorageBudget(key, writingOrigin);
  }

  return NS_OK;
}

/* static */
void CrossOriginStorageRegistry::UpgradeResourceVisibility(
    Entry& aEntry, const COSRequestedOriginsValue& aRequestedOrigins) {
  // https://wicg.github.io/cross-origin-storage/#resource-visibility-upgrades
  // Step 1: omitting `origins` never narrows or upgrades an entry.
  if (aRequestedOrigins.mKind == COSRequestedOriginsValue::Kind::SameSiteOnly) {
    return;
  }
  // Step 2: an already-wildcard entry can't be restricted by a later
  // writer.
  if (aEntry.mScope.mKind == Scope::Kind::Wildcard) {
    return;
  }
  // Step 3.
  if (aRequestedOrigins.mKind == COSRequestedOriginsValue::Kind::Wildcard) {
    aEntry.mScope.mKind = Scope::Kind::Wildcard;
    aEntry.mScope.mList.Clear();
    return;
  }
  // Step 4.
  if (aEntry.mScope.mKind == Scope::Kind::SameSiteOnly) {
    aEntry.mScope.mKind = Scope::Kind::List;
    aEntry.mScope.mList = aRequestedOrigins.mList.Clone();
    return;
  }
  // Step 5: merge, capped at kMaxOriginsListLength -- silently dropping any
  // excess candidates (with mList's existing LRU order deciding which
  // survive) rather than failing the write, which has already succeeded.
  MOZ_ASSERT(aEntry.mScope.mKind == Scope::Kind::List);
  for (const auto& candidate : aRequestedOrigins.mList) {
    if (aEntry.mScope.mList.Contains(candidate)) {
      continue;
    }
    if (aEntry.mScope.mList.Length() >= kMaxOriginsListLength) {
      break;
    }
    aEntry.mScope.mList.AppendElement(candidate);
  }
}

void CrossOriginStorageRegistry::EvictEntry(const nsACString& aKey,
                                            Entry& aEntry,
                                            COSHashAlgorithm aAlgorithm,
                                            const nsACString& aValue) {
  mTotalBytesUsed -= std::min(mTotalBytesUsed, aEntry.mByteSize);
  if (!aEntry.mStoringOrigins.IsEmpty()) {
    uint64_t* usage = mOriginUsage.Get(aEntry.mStoringOrigins[0]);
    if (usage) {
      *usage -= std::min(*usage, aEntry.mByteSize);
    }
  }
  if (aEntry.mBytesOnDisk) {
    if (CrossOriginStoragePersistence* persistence =
            CrossOriginStoragePersistence::GetOrCreate()) {
      persistence->DeleteEntry(aAlgorithm, aValue);
    }
  }
  mEntries.Remove(aKey);
}

void CrossOriginStorageRegistry::EnforceStorageBudget(
    const nsACString& aJustWrittenKey, const nsACString& aWritingOrigin) {
  CrossOriginStoragePersistence* persistence =
      CrossOriginStoragePersistence::GetOrCreate();
  if (!persistence) {
    return;  // No disk capacity to compute a budget from; matches Phase
             // 1's original unlimited-in-memory behavior.
  }
  int64_t diskCapacity = persistence->GetDiskCapacity();
  if (diskCapacity <= 0) {
    return;
  }
  int64_t globalBudget =
      std::min(static_cast<int64_t>(diskCapacity * kGlobalBudgetFraction),
               kMaxGlobalBudget);
  if (globalBudget <= 0) {
    return;
  }
  uint64_t perOriginBudget =
      static_cast<uint64_t>(globalBudget * kPerOriginBudgetFraction);

  // Collect every other Written entry's key/owner/size/recency once; both
  // eviction passes below select from this same snapshot. Deliberately a
  // full-table scan and sort -- see the Phase 1 limitations note in the
  // header for the real, incremental design this should become.
  struct Candidate {
    nsCString mKey;
    COSHashAlgorithm mAlgorithm = COSHashAlgorithm::SHA256;
    nsCString mValue;
    nsCString mOwner;  // Empty if this entry has no attributed owner.
    int64_t mLastReadTime = 0;
  };
  nsTArray<Candidate> candidates;
  for (auto iter = mEntries.Iter(); !iter.Done(); iter.Next()) {
    if (iter.Key().Equals(aJustWrittenKey)) {
      continue;  // Never evict the entry that was just written.
    }
    Entry* entry = iter.Data().get();
    if (entry->mState != Entry::State::Written) {
      continue;
    }
    Candidate& c = *candidates.AppendElement();
    c.mKey = iter.Key();
    SplitKey(c.mKey, &c.mAlgorithm, c.mValue);
    if (!entry->mStoringOrigins.IsEmpty()) {
      c.mOwner = entry->mStoringOrigins[0];
    }
    c.mLastReadTime = entry->mLastReadTime;
  }
  candidates.Sort([](const Candidate& a, const Candidate& b) {
    return a.mLastReadTime < b.mLastReadTime   ? -1
           : a.mLastReadTime > b.mLastReadTime ? 1
                                               : 0;
  });

  // Pass 1: if the writing origin is now over its own share, evict its
  // own sole-owned entries (oldest-read first) first, before touching any
  // other origin's entries at all.
  uint64_t* originUsage = mOriginUsage.Get(aWritingOrigin);
  if (originUsage && *originUsage > perOriginBudget) {
    uint64_t toFree = *originUsage - perOriginBudget;
    for (const Candidate& c : candidates) {
      if (toFree == 0) {
        break;
      }
      if (!c.mOwner.Equals(aWritingOrigin)) {
        continue;
      }
      Entry* entry = mEntries.Get(c.mKey);
      if (!entry) {
        continue;
      }
      uint64_t size = entry->mByteSize;
      EvictEntry(c.mKey, *entry, c.mAlgorithm, c.mValue);
      toFree -= std::min(toFree, size);
    }
  }

  // Pass 2: if still over the global cap (this origin's own eviction
  // above may not have been enough, or the pressure came from other
  // origins entirely), evict any entry, oldest-read first, regardless of
  // owner.
  if (mTotalBytesUsed > static_cast<uint64_t>(globalBudget)) {
    uint64_t toFree = mTotalBytesUsed - static_cast<uint64_t>(globalBudget);
    for (const Candidate& c : candidates) {
      if (toFree == 0) {
        break;
      }
      Entry* entry = mEntries.Get(c.mKey);
      if (!entry) {
        continue;  // Already evicted in pass 1.
      }
      uint64_t size = entry->mByteSize;
      EvictEntry(c.mKey, *entry, c.mAlgorithm, c.mValue);
      toFree -= std::min(toFree, size);
    }
  }
}

void CrossOriginStorageRegistry::ReleaseOutstandingWriter(
    COSHashAlgorithm aAlgorithm, const nsACString& aValue) {
  mozilla::ipc::AssertIsOnBackgroundThread();

  nsAutoCString key = MakeKey(aAlgorithm, aValue);
  Entry* entry = mEntries.Get(key);
  if (!entry) {
    return;
  }

  if (entry->mPendingWriterCount > 0) {
    entry->mPendingWriterCount--;
  }

  if (entry->mState == Entry::State::Pending &&
      entry->mPendingWriterCount == 0) {
    mEntries.Remove(key);
  }
}

CrossOriginStorageRegistry::ReadOutcome
CrossOriginStorageRegistry::GetFileBytes(COSHashAlgorithm aAlgorithm,
                                         const nsACString& aValue,
                                         nsTArray<uint8_t>& aOutBytes) {
  mozilla::ipc::AssertIsOnBackgroundThread();

  nsAutoCString key = MakeKey(aAlgorithm, aValue);
  Entry* entry = mEntries.Get(key);
  if (!entry) {
    return ReadOutcome::NotFound;
  }

  if (entry->mState == Entry::State::Pending) {
    if (entry->IsStale()) {
      mEntries.Remove(key);
      return ReadOutcome::NotFound;
    }
    return ReadOutcome::Pending;
  }

  if (!entry->mBytesOnDisk) {
    aOutBytes = entry->mBytes.Clone();
    return ReadOutcome::Found;
  }

  CrossOriginStoragePersistence* persistence =
      CrossOriginStoragePersistence::GetOrCreate();
  if (persistence && persistence->ReadBytes(aAlgorithm, aValue, aOutBytes)) {
    return ReadOutcome::Found;
  }

  // Metadata says Written but the bytes are reachable nowhere (disk read
  // failed, e.g. removed out from under us) -- treat as not found rather
  // than returning a Found result with no actual content.
  return ReadOutcome::NotFound;
}

void CrossOriginStorageRegistry::ClearAll() {
  mozilla::ipc::AssertIsOnBackgroundThread();

  if (CrossOriginStoragePersistence* persistence =
          CrossOriginStoragePersistence::GetOrCreate()) {
    persistence->ClearAll();
  }
  mEntries.Clear();
  mTotalBytesUsed = 0;
  mOriginUsage.Clear();
}

void CrossOriginStorageRegistry::RemoveSite(const nsACString& aSchemelessSite) {
  mozilla::ipc::AssertIsOnBackgroundThread();

  nsTArray<nsCString> keysToEvict;
  for (auto iter = mEntries.Iter(); !iter.Done(); iter.Next()) {
    Entry* entry = iter.Data().get();

    // If every storing origin belongs to the site being cleared, evict
    // the whole entry -- via EvictEntry below, which needs
    // mStoringOrigins[0] intact for its own usage-decrement, so this
    // check (and the eviction itself) must happen before any mutation.
    bool allOriginsMatch = !entry->mStoringOrigins.IsEmpty();
    for (const auto& site : entry->mStoringSites) {
      if (!HostMatchesSchemelessSite(site, aSchemelessSite)) {
        allOriginsMatch = false;
        break;
      }
    }
    if (allOriginsMatch) {
      keysToEvict.AppendElement(iter.Key());
      continue;
    }

    // Otherwise, just revoke this site's own share: remove its
    // storing-origin/storing-site entries (kept in lockstep) and its
    // List-scope disclosure grants, leaving the rest of the entry (and
    // any other storing origin's data) intact.
    for (size_t i = entry->mStoringSites.Length(); i > 0; --i) {
      if (HostMatchesSchemelessSite(entry->mStoringSites[i - 1],
                                    aSchemelessSite)) {
        entry->mStoringSites.RemoveElementAt(i - 1);
        entry->mStoringOrigins.RemoveElementAt(i - 1);
      }
    }
    if (entry->mScope.mKind == Scope::Kind::List) {
      for (size_t i = entry->mScope.mList.Length(); i > 0; --i) {
        if (HostMatchesSchemelessSite(entry->mScope.mList[i - 1],
                                      aSchemelessSite)) {
          entry->mScope.mList.RemoveElementAt(i - 1);
        }
      }
    }
  }

  for (const auto& key : keysToEvict) {
    Entry* entry = mEntries.Get(key);
    if (!entry) {
      continue;
    }
    COSHashAlgorithm algorithm;
    nsAutoCString value;
    if (!SplitKey(key, &algorithm, value)) {
      continue;
    }
    EvictEntry(key, *entry, algorithm, value);
  }
}

}  // namespace mozilla::dom
