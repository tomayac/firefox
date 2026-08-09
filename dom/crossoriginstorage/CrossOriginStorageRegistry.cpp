/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CrossOriginStorageRegistry.h"

#include "mozilla/ipc/BackgroundParent.h"

namespace mozilla::dom {

using mozilla::ipc::PrincipalInfo;

namespace {

// Not spec-mandated; see the implementation notes' suggested starting
// value for reclaiming a write abandoned without an explicit close()/
// abort() -- e.g. a page that navigates away or crashes mid-write.
const TimeDuration kPendingStalenessTimeout = TimeDuration::FromSeconds(5 * 60);

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
CrossOriginStorageRegistry& CrossOriginStorageRegistry::GetOrCreate() {
  mozilla::ipc::AssertIsOnBackgroundThread();
  static CrossOriginStorageRegistry sInstance;
  return sInstance;
}

CrossOriginStorageRegistry::ReadOutcome
CrossOriginStorageRegistry::CompleteReadRequest(
    COSHashAlgorithm aAlgorithm, const nsACString& aValue,
    const PrincipalInfo& aRequestingPrincipal) {
  mozilla::ipc::AssertIsOnBackgroundThread();

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
  // (minus the Public Hash List / GREASE'ing gate for wildcard-scoped
  // entries -- see the Phase 1 limitations note in
  // CrossOriginStorageRegistry.h).

  // Step 3: original storer access, independent of scope.
  nsAutoCString requestingOrigin;
  bool haveOrigin =
      NS_SUCCEEDED(GetOrigin(aRequestingPrincipal, requestingOrigin));
  if (haveOrigin && entry->mStoringOrigins.Contains(requestingOrigin)) {
    return ReadOutcome::Found;
  }

  switch (entry->mScope.mKind) {
    case Scope::Kind::Wildcard:
      // Step 4 (PHL/GREASE'ing gate not yet implemented; see above).
      return ReadOutcome::Found;

    case Scope::Kind::List: {
      // Step 5.
      if (!haveOrigin) {
        return ReadOutcome::NotFound;
      }
      int32_t index = entry->mScope.mList.IndexOf(requestingOrigin);
      if (index < 0) {
        return ReadOutcome::NotFound;
      }
      // Implementation notes §4: a successful read by a listed origin
      // refreshes its LRU recency (move to the most-recently-used end);
      // merely re-declaring it in a later write does not.
      entry->mScope.mList.RemoveElementAt(index);
      entry->mScope.mList.AppendElement(requestingOrigin);
      return ReadOutcome::Found;
    }

    case Scope::Kind::SameSiteOnly: {
      // Step 7.
      nsAutoCString requestingSite;
      if (NS_FAILED(GetSite(aRequestingPrincipal, requestingSite))) {
        return ReadOutcome::NotFound;
      }
      return entry->mStoringSites.Contains(requestingSite)
                 ? ReadOutcome::Found
                 : ReadOutcome::NotFound;
    }
  }
  return ReadOutcome::NotFound;
}

bool CrossOriginStorageRegistry::CompleteCreateRequest(
    COSHashAlgorithm aAlgorithm, const nsACString& aValue) {
  mozilla::ipc::AssertIsOnBackgroundThread();

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
  entry->mBytes = aBytes.Clone();
  entry->mState = Entry::State::Written;

  nsAutoCString writingOrigin;
  if (NS_SUCCEEDED(GetOrigin(aWritingPrincipal, writingOrigin)) &&
      !entry->mStoringOrigins.Contains(writingOrigin)) {
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

  aOutBytes = entry->mBytes.Clone();
  return ReadOutcome::Found;
}

}  // namespace mozilla::dom
