/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_CrossOriginStorageRegistry_h
#define mozilla_dom_CrossOriginStorageRegistry_h

#include "CrossOriginStoragePersistence.h"
#include "CrossOriginStorageUtils.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/ipc/PBackgroundSharedTypes.h"
#include "nsClassHashtable.h"
#include "nsString.h"
#include "nsTArray.h"

namespace mozilla::dom {

// The shared, cross-origin, hash-keyed registry backing
// navigator.crossOriginStorage
// (https://wicg.github.io/cross-origin-storage/#cos-entries). A single
// instance lives on the PBackground thread in the parent process and is
// reached by every content process's CrossOriginStorageParent actor.
//
// Deliberately NOT a dom::quota::Client: COS entries are not per-origin
// storage -- registering as a quota Client would force this into
// QuotaManager's origin-partitioned directory-lock model, which is the
// opposite of what content-addressability needs here.
//
// Threading: every method below must be called on the PBackground thread.
// All PCrossOriginStorage Recv* handlers already run there, which is an
// nsISerialEventTarget (handlers never run concurrently with each other,
// even if not literally the same OS thread), so the registry itself needs
// no internal locking -- mirroring Ladybird's documented choice for the
// same reason, rather than Servo's reader-writer lock (which exists for a
// genuinely multi-threaded access pattern this implementation doesn't
// have).
//
// Persistence: `written` entries are flushed to disk via
// CrossOriginStoragePersistence as part of VerifyAndStore, and reloaded
// (metadata only -- bytes are read back lazily, per GetFileBytes) at
// construction time. `pending` entries are never persisted; losing an
// in-flight write across a crash/restart is acceptable, matches every
// other implementation's own choice, and avoids needing crash-recovery
// for partial writes at all. If persistence is unavailable in this
// process (no profile, e.g.), the registry silently falls back to this
// phase's original in-memory-only behavior rather than failing the
// feature outright.
//
// Phase 1 limitations, tracked as explicit follow-up work rather than
// silently skipped:
// - Storage-budget eviction (EnforceStorageBudget below) is a naive O(n
//   log n) full-entry-list sort on every write that might need to evict,
//   not the incremental O(1) usage-tracking / O(log n) eviction-index
//   design a real implementation needs at scale.
// - The Public Hash List (CrossOriginStoragePublicHashList) ships with an
//   empty seed; populating it is a separate fetch/verify/build-time-embed
//   concern. See that class's own header comment.
// - Rate limiting (CrossOriginStorageRateLimiter) uses fixed,
//   non-configurable burst/refill constants copied from Servo's own
//   choices, not tuned for Firefox specifically.
class CrossOriginStorageRegistry {
 public:
  static CrossOriginStorageRegistry& GetOrCreate();

  enum class ReadOutcome {
    Found,     // Disclosable; construct a handle addressing a written entry.
    NotFound,  // No entry, not disclosable, or a stale abandoned write.
    Pending,   // A write for this hash is still genuinely in progress.
  };

  // https://wicg.github.io/cross-origin-storage/#complete-a-read-request
  // (minus the promise/task-queuing and FileSystemFileHandle construction,
  // which are the caller's job). A rate-limited request (see
  // CrossOriginStorageRateLimiter) returns NotFound -- indistinguishable
  // from a genuine miss, by design.
  ReadOutcome CompleteReadRequest(
      COSHashAlgorithm aAlgorithm, const nsACString& aValue,
      const mozilla::ipc::PrincipalInfo& aRequestingPrincipal);

  // https://wicg.github.io/cross-origin-storage/#complete-a-create-request
  // Always "succeeds": creates a pending entry if none exists yet (or the
  // existing one had gone stale), and increments its outstanding-writer
  // count either way. Returns whether the entry is already `written` at
  // this moment; informational only -- the live registry state at
  // close() time is always authoritative. A rate-limited request (see
  // CrossOriginStorageRateLimiter) silently skips creating/touching any
  // entry and returns false, identically to an ordinary fresh pending
  // creation -- the eventual close() for that write then fails generically
  // (no entry to verify against), rather than surfacing a distinguishable
  // "you are rate limited" signal at request time.
  bool CompleteCreateRequest(
      COSHashAlgorithm aAlgorithm, const nsACString& aValue,
      const mozilla::ipc::PrincipalInfo& aWritingPrincipal);

  // https://wicg.github.io/cross-origin-storage/#verify-and-store
  // aBytes is the complete written byte sequence. On a hash mismatch,
  // returns NS_ERROR_DOM_DATA_ERR and has already run the
  // outstanding-writer cleanup below; the caller must not call it again
  // for this session. On success, also runs "upgrade resource visibility"
  // (https://wicg.github.io/cross-origin-storage/#resource-visibility-upgrades)
  // against aRequestedOrigins, persists the entry to disk, and enforces
  // the storage budget (EnforceStorageBudget), possibly evicting other
  // entries.
  nsresult VerifyAndStore(COSHashAlgorithm aAlgorithm, const nsACString& aValue,
                          const nsTArray<uint8_t>& aBytes,
                          const mozilla::ipc::PrincipalInfo& aWritingPrincipal,
                          const COSRequestedOriginsValue& aRequestedOrigins);

  // The cleanup half of a failed close() (already run by VerifyAndStore on
  // a hash mismatch) or of an explicit abort(): decrements the entry's
  // outstanding-writer count, and removes the entry if the count reaches 0
  // and it was never `written`. The zero-count check protects a
  // genuinely concurrent sibling write for the same hash from having its
  // entry deleted out from under it; the never-`written` check protects an
  // already-`written` entry from ever being removed this way, no matter
  // how many later writers request and fail it.
  void ReleaseOutstandingWriter(COSHashAlgorithm aAlgorithm,
                                const nsACString& aValue);

  // Backs FileSystemFileHandle.getFile() on a handle already obtained via
  // CompleteReadRequest/CompleteCreateRequest -- so, unlike those, this
  // does not repeat availability gating; it only checks the entry's
  // current state
  // (https://wicg.github.io/cross-origin-storage/#cos-file-system) and, if
  // Found, reads its bytes back from disk (or memory, if persistence is
  // unavailable) into aOutBytes.
  ReadOutcome GetFileBytes(COSHashAlgorithm aAlgorithm,
                           const nsACString& aValue,
                           nsTArray<uint8_t>& aOutBytes);

  // nsICrossOriginStorageService's backing (see CrossOriginStorageService.h)
  // -- reached from nsIClearDataService's CLEAR_CROSS_ORIGIN_STORAGE
  // cleaner, not from the actor path any content process uses.

  // Deletes every entry, in memory and on disk.
  void ClearAll();

  // The spec has no clear-data algorithm of its own; this implements
  // what nsIClearDataService's deleteBySite()/deleteByPrincipal() need:
  // revokes aSchemelessSite's (and every origin under it, matched by
  // host suffix) association with every entry it stored -- removed from
  // storing origins/sites and any explicit origins list -- deleting an
  // entry outright only if that leaves it with no storing origin left.
  // An entry another, unrelated site also legitimately stored keeps
  // that other site's copy.
  //
  // Known limitation: storage-budget usage (mOriginUsage) is attributed
  // only to an entry's *first* storing origin (mStoringOrigins[0]); if
  // that specific origin is the one revoked here while other storing
  // origins remain, its usage credit is not reattributed or released,
  // becoming stale. Rare (needs multiple origins to have genuinely
  // stored byte-identical content) and bounded (can't grow, only go
  // stale), not fixed here.
  void RemoveSite(const nsACString& aSchemelessSite);

 private:
  CrossOriginStorageRegistry();

  // https://wicg.github.io/cross-origin-storage/#cos-entries -- the
  // disclosure-scope part of a COS entry ("origins"). Distinct from
  // COSRequestedOrigins (the wire-format request payload): this is the
  // live, mergeable entry state, and List additionally tracks recency for
  // the LRU eviction upgrade-time merging needs.
  struct Scope {
    enum class Kind { SameSiteOnly, List, Wildcard } mKind = Kind::SameSiteOnly;
    // Meaningful only when mKind == Kind::List. Ordered least-recently-used
    // first; see CrossOriginStorageRegistry.cpp's disclosure/merge logic.
    nsTArray<nsCString> mList;
  };

  struct Entry {
    enum class State { Pending, Written } mState = State::Pending;
    uint32_t mPendingWriterCount = 0;
    // Every origin that has successfully written this entry
    // (https://wicg.github.io/cross-origin-storage/#original-storer-access);
    // such an origin can always read the entry back, independent of mScope.
    // mStoringOrigins[0], if present, is also who this entry's bytes are
    // attributed to for per-origin storage-budget accounting -- the
    // *first* successful writer, not every later co-storer.
    nsTArray<nsCString> mStoringOrigins;
    // The site (scheme + eTLD+1) of every entry in mStoringOrigins, kept in
    // lockstep -- same-site-only disclosure compares against these, not
    // against mStoringOrigins directly, since that scope discloses to any
    // origin same-site with a storing origin, not just exact matches.
    nsTArray<nsCString> mStoringSites;
    Scope mScope;
    // Only meaningful while mState is Pending; see IsStale().
    TimeStamp mPendingSince;
    // Only meaningful while mState is Written. The complete byte count --
    // kept resident even though the bytes themselves live on disk once
    // written, since GREASE'ing's size ceiling and storage-budget
    // accounting both need it cheaply, without a disk read.
    uint64_t mByteSize = 0;
    // True once this entry's bytes are safely on disk (GetFileBytes reads
    // them back from there). False either transiently (write in
    // progress) or durably, as a fallback, if persistence was
    // unavailable or the disk write failed -- in which case mBytes below
    // holds the bytes instead, exactly as Phase 1 originally worked.
    bool mBytesOnDisk = false;
    // Only meaningful while mState is Written and !mBytesOnDisk.
    nsTArray<uint8_t> mBytes;
    // Only meaningful while mState is Written. Updated on every
    // disclosed read (in memory only -- not flushed to disk per read, to
    // avoid a disk write on every read; see the class comment) and used
    // for storage-budget eviction's oldest-read-first ordering.
    int64_t mLastReadTime = 0;

    // A pending entry with no close()/abort() ever received (a page
    // navigated away, crashed, or simply never finished) would otherwise
    // stay Pending forever. Checked lazily at lookup time -- rather than
    // via a proactively-sweeping timer -- since that already produces the
    // spec-required observable behavior (a stale pending entry reads as
    // not found, and a fresh create() silently replaces it) with much
    // less machinery.
    bool IsStale() const;
  };

  static nsAutoCString MakeKey(COSHashAlgorithm aAlgorithm,
                               const nsACString& aValue);

  // The inverse of MakeKey(): splits a "<algorithm>:<value>" mEntries key
  // back into its parts, needed wherever a key alone (from an mEntries
  // iterator, say) isn't enough -- persistence and eviction both need the
  // algorithm/value pair, not the opaque key string. Returns false (key
  // left as an empty string) if aKey isn't recognized, which shouldn't
  // happen for any key MakeKey() itself produced.
  static bool SplitKey(const nsACString& aKey, COSHashAlgorithm* aOutAlgorithm,
                       nsACString& aOutValue);

  // https://wicg.github.io/cross-origin-storage/#resource-visibility-upgrades
  static void UpgradeResourceVisibility(
      Entry& aEntry, const COSRequestedOriginsValue& aRequestedOrigins);

  // Loads every persisted entry's metadata (not bytes) into mEntries and
  // sums mTotalBytesUsed/mOriginUsage, if persistence is available. A
  // no-op (mEntries starts empty, matching Phase 1's original behavior)
  // if it isn't.
  void LoadPersistedEntries();

  // https://wicg.github.io/cross-origin-storage/#storage-limits (not
  // spec-mandated in exact shape). Called after a successful
  // VerifyAndStore for aJustWrittenKey: if aWritingOrigin's own share or
  // the global budget is now exceeded, evicts other entries -- oldest
  // mLastReadTime first, preferring aWritingOrigin's own sole-owned
  // entries before reaching for any other origin's -- until back under
  // budget. Never evicts aJustWrittenKey itself. A no-op if the disk
  // capacity needed to compute a budget is unavailable.
  void EnforceStorageBudget(const nsACString& aJustWrittenKey,
                            const nsACString& aWritingOrigin);

  // Removes aKey from mEntries, mOriginUsage, and disk, and adjusts
  // mTotalBytesUsed. Shared by EnforceStorageBudget and (indirectly) by
  // ordinary entry replacement.
  void EvictEntry(const nsACString& aKey, Entry& aEntry,
                  COSHashAlgorithm aAlgorithm, const nsACString& aValue);

  nsClassHashtable<nsCStringHashKey, Entry> mEntries;

  // Running totals kept in sync with mEntries by every insertion/eviction
  // site, rather than re-summed on demand -- see the Phase 1 limitations
  // note above re: this still being an O(n log n) eviction scan overall.
  uint64_t mTotalBytesUsed = 0;
  nsClassHashtable<nsCStringHashKey, uint64_t> mOriginUsage;
};

}  // namespace mozilla::dom

#endif  // mozilla_dom_CrossOriginStorageRegistry_h
