/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_CrossOriginStorageRegistry_h
#define mozilla_dom_CrossOriginStorageRegistry_h

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
// Phase 1 limitations, tracked as explicit follow-up work rather than
// silently skipped:
// - Entries are in-memory only (lost on restart); no on-disk persistence.
// - Only the same-site-only disclosure scope is implemented. List- and
//   wildcard-scoped entries, the Public Hash List, and GREASE'ing don't
//   exist yet -- CrossOriginStorageManager rejects any requestFileHandle()
//   call that specifies the `origins` option at all.
// - No rate limiting, and no storage-budget/eviction accounting.
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
  // which are the caller's job).
  ReadOutcome CompleteReadRequest(
      COSHashAlgorithm aAlgorithm, const nsACString& aValue,
      const mozilla::ipc::PrincipalInfo& aRequestingPrincipal);

  // https://wicg.github.io/cross-origin-storage/#complete-a-create-request
  // Always "succeeds": creates a pending entry if none exists yet (or the
  // existing one had gone stale), and increments its outstanding-writer
  // count either way. Returns whether the entry is already `written` at
  // this moment; informational only -- the live registry state at
  // close() time is always authoritative.
  bool CompleteCreateRequest(COSHashAlgorithm aAlgorithm,
                             const nsACString& aValue);

  // https://wicg.github.io/cross-origin-storage/#verify-and-store
  // aBytes is the complete written byte sequence. On a hash mismatch,
  // returns NS_ERROR_DOM_DATA_ERR and has already run the
  // outstanding-writer cleanup below; the caller must not call it again
  // for this session.
  nsresult VerifyAndStore(COSHashAlgorithm aAlgorithm, const nsACString& aValue,
                          const nsTArray<uint8_t>& aBytes,
                          const mozilla::ipc::PrincipalInfo& aWritingPrincipal);

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

 private:
  CrossOriginStorageRegistry() = default;

  struct Entry {
    enum class State { Pending, Written } mState = State::Pending;
    nsTArray<uint8_t> mBytes;
    uint32_t mPendingWriterCount = 0;
    // The "site" (scheme + eTLD+1; see GetSite() in the .cpp) of every
    // origin that has successfully written this entry. Phase 1 only
    // supports same-site-only disclosure, so membership here is also the
    // entire disclosure story -- there is no separate `origins` scope
    // field yet. A future phase that adds per-origin storage-budget
    // accounting will need the exact origins too, not just their sites.
    nsTArray<nsCString> mStoringSites;
    // Only meaningful while mState is Pending; see IsStale().
    TimeStamp mPendingSince;

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

  nsClassHashtable<nsCStringHashKey, Entry> mEntries;
};

}  // namespace mozilla::dom

#endif  // mozilla_dom_CrossOriginStorageRegistry_h
