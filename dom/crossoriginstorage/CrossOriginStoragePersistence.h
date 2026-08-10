/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_CrossOriginStoragePersistence_h
#define mozilla_dom_CrossOriginStoragePersistence_h

#include "CrossOriginStorageUtils.h"
#include "nsCOMPtr.h"
#include "nsString.h"
#include "nsTArray.h"

class nsIFile;

namespace mozilla::dom {

// On-disk backing for `written` COS entries, under
// <profile>/storage/cross-origin-storage/<algorithm>/<value>.{bytes,meta}
// -- a content-addressed layout (safe by construction: `value` is only ever
// used as a path component after CrossOriginStorageManager's per-algorithm
// hex-shape validation, closing the path-traversal footgun the Servo/
// Ladybird implementation notes flag) mirroring the layout Ladybird's own
// plan uses.
//
// Only `written` entries are persisted; a `pending` write session's bytes
// live solely in CrossOriginStorageParent::WriteSession (in memory) until
// VerifyAndStore succeeds, exactly as before this class existed -- losing
// an in-flight write on a crash/restart is acceptable, matches every other
// implementation's own choice, and avoids needing crash-recovery for
// partial writes here at all.
//
// Threading/perf caveat, deliberate for this phase: every method does
// synchronous file I/O directly on the calling thread (the PBackground
// thread, in practice) rather than dispatching to a dedicated I/O thread
// the way QuotaManager does. Correct but not necessarily fast under load;
// a real I/O thread is real follow-up work, not silently skipped.
class CrossOriginStoragePersistence {
 public:
  // Returns nullptr if the profile directory couldn't be resolved or
  // created (e.g. no profile in this process at all). Every other method
  // on a null instance -- callers get one via GetOrCreate() -- is simply
  // never reached; the registry falls back to Phase 1's original
  // in-memory-only behavior rather than failing the feature outright.
  static CrossOriginStoragePersistence* GetOrCreate();

  // The registry's own view of one persisted entry's metadata -- everything
  // needed to reconstruct a CrossOriginStorageRegistry::Entry except its
  // bytes, which are read from disk separately (and lazily) via ReadBytes.
  struct PersistedEntry {
    nsCString mAlgorithm;  // CanonicalHashAlgorithmName() spelling
    nsCString mValue;
    uint32_t mScopeKind = 0;  // CrossOriginStorageRegistry::Scope::Kind
    nsTArray<nsCString> mScopeList;
    nsTArray<nsCString> mStoringOrigins;
    nsTArray<nsCString> mStoringSites;
    uint64_t mByteSize = 0;
    int64_t mLastReadTime = 0;  // PR_Now()-style microseconds since epoch
  };

  // Startup: enumerate every persisted entry, deleting any orphaned
  // `.tmp` file left behind by a write that never completed (a crash
  // between WriteBytes and the atomic rename in WriteEntry below).
  void ScanPersistedEntries(nsTArray<PersistedEntry>& aOutEntries);

  // Atomically writes both the `.bytes` and `.meta` files for one entry:
  // each is written to a `.tmp` sibling first, then renamed into place --
  // a single filesystem rename is atomic, so a crash mid-write leaves only
  // an orphaned `.tmp` (cleaned up by the next ScanPersistedEntries), never
  // a half-written `.bytes`/`.meta` masquerading as complete.
  bool WriteEntry(COSHashAlgorithm aAlgorithm, const nsACString& aValue,
                  const nsTArray<uint8_t>& aBytes,
                  const PersistedEntry& aMetadata);

  // Rewrites just the `.meta` file (same atomic tmp-then-rename), for
  // metadata-only changes after the initial write (a visibility upgrade,
  // or a list-scope LRU touch the caller has chosen to flush). Returns
  // false (a no-op) if no such entry exists on disk.
  bool UpdateMetadata(COSHashAlgorithm aAlgorithm, const nsACString& aValue,
                      const PersistedEntry& aMetadata);

  bool ReadBytes(COSHashAlgorithm aAlgorithm, const nsACString& aValue,
                 nsTArray<uint8_t>& aOutBytes);

  void DeleteEntry(COSHashAlgorithm aAlgorithm, const nsACString& aValue);

  // Removes every persisted entry by deleting and recreating the whole
  // base directory, rather than iterating and deleting file by file --
  // for nsICrossOriginStorageService::clear()'s full wipe, where "every
  // last entry, including ones a concurrent write might be adding right
  // now" is exactly the intended (if inherently racy against that
  // concurrent write) behavior.
  void ClearAll();

  // The total (not free) capacity of the volume the profile directory
  // lives on, for CrossOriginStorageRegistry's storage-budget accounting.
  // Returns 0 on failure -- callers must treat that as "budget unknown",
  // not as a zero-sized disk.
  int64_t GetDiskCapacity();

 private:
  CrossOriginStoragePersistence() = default;

  bool EnsureAlgorithmDir(const nsACString& aAlgorithm, nsIFile** aOutDir);
  bool GetBytesFile(COSHashAlgorithm aAlgorithm, const nsACString& aValue,
                    nsIFile** aOutFile);
  bool GetMetaFile(COSHashAlgorithm aAlgorithm, const nsACString& aValue,
                   nsIFile** aOutFile);

  nsCOMPtr<nsIFile> mBaseDir;
};

}  // namespace mozilla::dom

#endif  // mozilla_dom_CrossOriginStoragePersistence_h
