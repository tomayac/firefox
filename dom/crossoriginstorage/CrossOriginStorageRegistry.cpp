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

// The "site" (scheme + eTLD+1) of aPrincipalInfo, per HTML's "obtain a
// site". Two origins are same-site iff their sites are equal.
//
// This works directly off the wire-format PrincipalInfo rather than
// constructing a real nsIPrincipal via PrincipalInfoToPrincipal(), which
// must be called on the main thread -- everything in this registry runs on
// the PBackground thread instead. ContentPrincipalInfo's originNoSuffix and
// baseDomain fields are explicitly documented as safe to use off the main
// thread for exactly this kind of reason.
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
      // Opaque; using its own (per-instance-unique) spec as the "site"
      // means it correctly never compares same-site with anything else,
      // including another null principal.
      aOutSite = aPrincipalInfo.get_NullPrincipalInfo().spec();
      return NS_OK;
    case PrincipalInfo::TSystemPrincipalInfo:
      aOutSite = "system:"_ns;
      return NS_OK;
    case PrincipalInfo::TExpandedPrincipalInfo:
    default:
      // Not a realistic caller of navigator.crossOriginStorage. Fail
      // closed (never same-site with anything) rather than guess.
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
  // and https://wicg.github.io/cross-origin-storage/#apply-availability-gating,
  // restricted to the same-site-only branch -- the only scope Phase 1
  // implements. Storing sites double as "original storer access" here: a
  // requester whose site is already in mStoringSites is, by construction,
  // either the original storer or same-site with it, so no separate exact-
  // origin check is needed on top of the site check.
  nsAutoCString requestingSite;
  if (NS_FAILED(GetSite(aRequestingPrincipal, requestingSite))) {
    return ReadOutcome::NotFound;
  }
  if (entry->mStoringSites.Contains(requestingSite)) {
    return ReadOutcome::Found;
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
    const nsTArray<uint8_t>& aBytes, const PrincipalInfo& aWritingPrincipal) {
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

  // Step 3 (bytes/state) and the "upgrade resource visibility" call it
  // makes -- collapsed here, since Phase 1's only scope is same-site-only,
  // there is nothing to upgrade beyond recording the writing site.
  entry->mBytes = aBytes.Clone();
  entry->mState = Entry::State::Written;

  nsAutoCString writingSite;
  if (NS_SUCCEEDED(GetSite(aWritingPrincipal, writingSite)) &&
      !entry->mStoringSites.Contains(writingSite)) {
    entry->mStoringSites.AppendElement(writingSite);
  }

  if (entry->mPendingWriterCount > 0) {
    entry->mPendingWriterCount--;
  }

  return NS_OK;
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

}  // namespace mozilla::dom
