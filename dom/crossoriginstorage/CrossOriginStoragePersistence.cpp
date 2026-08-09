/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CrossOriginStoragePersistence.h"

#include <algorithm>

#include "mozilla/SyncRunnable.h"
#include "mozilla/UniquePtr.h"
#include "nsAppDirectoryServiceDefs.h"
#include "nsComponentManagerUtils.h"
#include "nsDirectoryServiceUtils.h"
#include "nsIBinaryInputStream.h"
#include "nsIBinaryOutputStream.h"
#include "nsIDirectoryEnumerator.h"
#include "nsIFile.h"
#include "nsNetUtil.h"
#include "nsReadableUtils.h"
#include "nsString.h"
#include "nsThreadUtils.h"

namespace mozilla::dom {

namespace {

// Not exported via a public header (nsBinaryStream.h, which declares
// these, is xpcom/io-internal); their values are stable, well-known
// XPCOM contract IDs, safe to duplicate here.
constexpr char kBinaryOutputStreamContractId[] =
    "@mozilla.org/binaryoutputstream;1";
constexpr char kBinaryInputStreamContractId[] =
    "@mozilla.org/binaryinputstream;1";

// Bumped if the on-disk shape below ever changes; a mismatched version is
// treated as "no entry" (the safest failure mode -- an unreadable entry is
// simply not disclosed, never a crash).
constexpr uint32_t kMetaVersion = 1;

bool WriteAllBytes(nsIOutputStream* aStream, const uint8_t* aData,
                   uint64_t aLength) {
  uint64_t offset = 0;
  while (offset < aLength) {
    uint32_t chunk =
        static_cast<uint32_t>(std::min<uint64_t>(aLength - offset, 1u << 20));
    uint32_t written = 0;
    nsresult rv = aStream->Write(reinterpret_cast<const char*>(aData) + offset,
                                 chunk, &written);
    if (NS_FAILED(rv) || written == 0) {
      return false;
    }
    offset += written;
  }
  return true;
}

// Runs aWriteFn(stream) against a fresh `.tmp` sibling of aFinalFile, then
// atomically renames it over aFinalFile on success. A crash partway
// through leaves only the `.tmp` sibling (cleaned up by the next
// ScanPersistedEntries), never a half-written aFinalFile.
template <typename WriteFn>
bool AtomicWriteFile(nsIFile* aFinalFile, WriteFn aWriteFn) {
  nsAutoString finalLeaf;
  nsresult rv = aFinalFile->GetLeafName(finalLeaf);
  if (NS_FAILED(rv)) {
    return false;
  }

  nsCOMPtr<nsIFile> tmpFile;
  rv = aFinalFile->Clone(getter_AddRefs(tmpFile));
  if (NS_FAILED(rv)) {
    return false;
  }
  nsAutoString tmpLeaf = finalLeaf + u".tmp"_ns;
  rv = tmpFile->SetLeafName(tmpLeaf);
  if (NS_FAILED(rv)) {
    return false;
  }

  bool ok;
  {
    nsCOMPtr<nsIOutputStream> out;
    rv = NS_NewLocalFileOutputStream(getter_AddRefs(out), tmpFile);
    if (NS_FAILED(rv)) {
      return false;
    }
    ok = aWriteFn(out.get());
    out->Close();
  }
  if (!ok) {
    tmpFile->Remove(false);
    return false;
  }

  rv = tmpFile->MoveTo(nullptr, finalLeaf);
  if (NS_FAILED(rv)) {
    tmpFile->Remove(false);
    return false;
  }
  return true;
}

bool WriteMetaBytes(
    nsIOutputStream* aStream,
    const CrossOriginStoragePersistence::PersistedEntry& aMetadata) {
  nsCOMPtr<nsIBinaryOutputStream> stream =
      do_CreateInstance(kBinaryOutputStreamContractId);
  if (!stream || NS_FAILED(stream->SetOutputStream(aStream))) {
    return false;
  }

  auto writeList = [&](const nsTArray<nsCString>& aList) {
    if (NS_FAILED(stream->Write32(aList.Length()))) {
      return false;
    }
    for (const auto& s : aList) {
      if (NS_FAILED(stream->WriteCString(s))) {
        return false;
      }
    }
    return true;
  };

  return NS_SUCCEEDED(stream->Write32(kMetaVersion)) &&
         NS_SUCCEEDED(stream->WriteCString(aMetadata.mAlgorithm)) &&
         NS_SUCCEEDED(stream->WriteCString(aMetadata.mValue)) &&
         NS_SUCCEEDED(stream->Write32(aMetadata.mScopeKind)) &&
         writeList(aMetadata.mScopeList) &&
         writeList(aMetadata.mStoringOrigins) &&
         writeList(aMetadata.mStoringSites) &&
         NS_SUCCEEDED(stream->Write64(aMetadata.mByteSize)) &&
         NS_SUCCEEDED(
             stream->Write64(static_cast<uint64_t>(aMetadata.mLastReadTime)));
}

bool ReadMetaFile(nsIFile* aMetaFile,
                  CrossOriginStoragePersistence::PersistedEntry& aOut) {
  nsCOMPtr<nsIInputStream> rawStream;
  nsresult rv =
      NS_NewLocalFileInputStream(getter_AddRefs(rawStream), aMetaFile);
  if (NS_FAILED(rv)) {
    return false;
  }
  // nsIBinaryInputStream::ReadCString() needs ReadSegments(), which a raw
  // local-file stream deliberately doesn't implement (nsFileStreamBase::
  // ReadSegments() is an unconditional NS_ERROR_NOT_IMPLEMENTED) --
  // wrapping it in a buffered stream, exactly as that class's own comment
  // recommends, is what actually makes ReadSegments() work.
  nsCOMPtr<nsIInputStream> fileStream;
  rv = NS_NewBufferedInputStream(getter_AddRefs(fileStream), rawStream.forget(),
                                 4096);
  if (NS_FAILED(rv)) {
    return false;
  }
  nsCOMPtr<nsIBinaryInputStream> stream =
      do_CreateInstance(kBinaryInputStreamContractId);
  if (!stream || NS_FAILED(stream->SetInputStream(fileStream))) {
    return false;
  }

  uint32_t version = 0;
  if (NS_FAILED(stream->Read32(&version)) || version != kMetaVersion) {
    return false;
  }

  auto readList = [&](nsTArray<nsCString>& aOutList) {
    uint32_t count = 0;
    if (NS_FAILED(stream->Read32(&count)) || count > 1000000) {
      return false;
    }
    for (uint32_t i = 0; i < count; i++) {
      nsCString s;
      if (NS_FAILED(stream->ReadCString(s))) {
        return false;
      }
      aOutList.AppendElement(s);
    }
    return true;
  };

  uint64_t lastReadTime = 0;
  bool ok = NS_SUCCEEDED(stream->ReadCString(aOut.mAlgorithm)) &&
            NS_SUCCEEDED(stream->ReadCString(aOut.mValue)) &&
            NS_SUCCEEDED(stream->Read32(&aOut.mScopeKind)) &&
            readList(aOut.mScopeList) && readList(aOut.mStoringOrigins) &&
            readList(aOut.mStoringSites) &&
            NS_SUCCEEDED(stream->Read64(&aOut.mByteSize)) &&
            NS_SUCCEEDED(stream->Read64(&lastReadTime));
  aOut.mLastReadTime = static_cast<int64_t>(lastReadTime);
  return ok;
}

}  // namespace

/* static */
CrossOriginStoragePersistence* CrossOriginStoragePersistence::GetOrCreate() {
  static UniquePtr<CrossOriginStoragePersistence> sInstance;
  static bool sAttempted = false;
  if (sAttempted) {
    return sInstance.get();
  }
  sAttempted = true;

  // NS_GetSpecialDirectory must run on the main thread; this round trip
  // happens once (the first GetOrCreate() call from the PBackground
  // thread), and the resolved path is reused for every later call.
  nsCOMPtr<nsIFile> profileDir;
  nsCOMPtr<nsIRunnable> runnable = NS_NewRunnableFunction(
      "CrossOriginStoragePersistence::ResolveProfileDir", [&profileDir]() {
        NS_GetSpecialDirectory(NS_APP_USER_PROFILE_50_DIR,
                               getter_AddRefs(profileDir));
      });
  nsCOMPtr<nsIThread> mainThread;
  NS_GetMainThread(getter_AddRefs(mainThread));
  if (!mainThread) {
    return nullptr;
  }
  SyncRunnable::DispatchToThread(mainThread, runnable);

  if (!profileDir) {
    return nullptr;
  }

  nsresult rv = profileDir->AppendNative("cross-origin-storage"_ns);
  if (NS_FAILED(rv)) {
    return nullptr;
  }
  rv = profileDir->Create(nsIFile::DIRECTORY_TYPE, 0700);
  if (NS_FAILED(rv) && rv != NS_ERROR_FILE_ALREADY_EXISTS) {
    return nullptr;
  }

  sInstance = WrapUnique(new CrossOriginStoragePersistence());
  sInstance->mBaseDir = profileDir;
  return sInstance.get();
}

bool CrossOriginStoragePersistence::EnsureAlgorithmDir(
    const nsACString& aAlgorithm, nsIFile** aOutDir) {
  nsCOMPtr<nsIFile> dir;
  nsresult rv = mBaseDir->Clone(getter_AddRefs(dir));
  if (NS_FAILED(rv)) {
    return false;
  }
  rv = dir->AppendNative(aAlgorithm);
  if (NS_FAILED(rv)) {
    return false;
  }
  rv = dir->Create(nsIFile::DIRECTORY_TYPE, 0700);
  if (NS_FAILED(rv) && rv != NS_ERROR_FILE_ALREADY_EXISTS) {
    return false;
  }
  dir.forget(aOutDir);
  return true;
}

bool CrossOriginStoragePersistence::GetBytesFile(COSHashAlgorithm aAlgorithm,
                                                 const nsACString& aValue,
                                                 nsIFile** aOutFile) {
  nsCOMPtr<nsIFile> dir;
  if (!EnsureAlgorithmDir(
          nsDependentCString(CanonicalHashAlgorithmName(aAlgorithm)),
          getter_AddRefs(dir))) {
    return false;
  }
  nsAutoCString leaf(aValue);
  leaf.AppendLiteral(".bytes");
  nsresult rv = dir->AppendNative(leaf);
  if (NS_FAILED(rv)) {
    return false;
  }
  dir.forget(aOutFile);
  return true;
}

bool CrossOriginStoragePersistence::GetMetaFile(COSHashAlgorithm aAlgorithm,
                                                const nsACString& aValue,
                                                nsIFile** aOutFile) {
  nsCOMPtr<nsIFile> dir;
  if (!EnsureAlgorithmDir(
          nsDependentCString(CanonicalHashAlgorithmName(aAlgorithm)),
          getter_AddRefs(dir))) {
    return false;
  }
  nsAutoCString leaf(aValue);
  leaf.AppendLiteral(".meta");
  nsresult rv = dir->AppendNative(leaf);
  if (NS_FAILED(rv)) {
    return false;
  }
  dir.forget(aOutFile);
  return true;
}

bool CrossOriginStoragePersistence::WriteEntry(
    COSHashAlgorithm aAlgorithm, const nsACString& aValue,
    const nsTArray<uint8_t>& aBytes, const PersistedEntry& aMetadata) {
  nsCOMPtr<nsIFile> bytesFile, metaFile;
  if (!GetBytesFile(aAlgorithm, aValue, getter_AddRefs(bytesFile)) ||
      !GetMetaFile(aAlgorithm, aValue, getter_AddRefs(metaFile))) {
    return false;
  }
  if (!AtomicWriteFile(bytesFile, [&](nsIOutputStream* aStream) {
        return WriteAllBytes(aStream, aBytes.Elements(), aBytes.Length());
      })) {
    return false;
  }
  if (!AtomicWriteFile(metaFile, [&](nsIOutputStream* aStream) {
        return WriteMetaBytes(aStream, aMetadata);
      })) {
    // Bytes are already on disk but metadata isn't -- leaves an orphaned
    // `.bytes` file with no matching `.meta`; ScanPersistedEntries skips
    // any `.bytes` file without a readable sibling `.meta`, so this just
    // wastes disk space rather than corrupting registry state. Acceptable
    // Phase 1 behavior for a failure this unlikely (metadata is tiny).
    return false;
  }
  return true;
}

bool CrossOriginStoragePersistence::UpdateMetadata(
    COSHashAlgorithm aAlgorithm, const nsACString& aValue,
    const PersistedEntry& aMetadata) {
  nsCOMPtr<nsIFile> metaFile;
  if (!GetMetaFile(aAlgorithm, aValue, getter_AddRefs(metaFile))) {
    return false;
  }
  bool exists = false;
  metaFile->Exists(&exists);
  if (!exists) {
    return false;
  }
  return AtomicWriteFile(metaFile, [&](nsIOutputStream* aStream) {
    return WriteMetaBytes(aStream, aMetadata);
  });
}

bool CrossOriginStoragePersistence::ReadBytes(COSHashAlgorithm aAlgorithm,
                                              const nsACString& aValue,
                                              nsTArray<uint8_t>& aOutBytes) {
  nsCOMPtr<nsIFile> bytesFile;
  if (!GetBytesFile(aAlgorithm, aValue, getter_AddRefs(bytesFile))) {
    return false;
  }
  nsCOMPtr<nsIInputStream> in;
  nsresult rv = NS_NewLocalFileInputStream(getter_AddRefs(in), bytesFile);
  if (NS_FAILED(rv)) {
    return false;
  }
  nsCString data;
  rv = NS_ReadInputStreamToString(in, data, -1);
  if (NS_FAILED(rv)) {
    return false;
  }
  aOutBytes.Clear();
  aOutBytes.AppendElements(reinterpret_cast<const uint8_t*>(data.get()),
                           data.Length());
  return true;
}

void CrossOriginStoragePersistence::DeleteEntry(COSHashAlgorithm aAlgorithm,
                                                const nsACString& aValue) {
  nsCOMPtr<nsIFile> bytesFile, metaFile;
  if (GetBytesFile(aAlgorithm, aValue, getter_AddRefs(bytesFile))) {
    bytesFile->Remove(false);
  }
  if (GetMetaFile(aAlgorithm, aValue, getter_AddRefs(metaFile))) {
    metaFile->Remove(false);
  }
}

void CrossOriginStoragePersistence::ScanPersistedEntries(
    nsTArray<PersistedEntry>& aOutEntries) {
  nsCOMPtr<nsIDirectoryEnumerator> algorithmDirs;
  if (NS_FAILED(mBaseDir->GetDirectoryEntries(getter_AddRefs(algorithmDirs))) ||
      !algorithmDirs) {
    return;
  }

  nsCOMPtr<nsIFile> algorithmDir;
  while (
      NS_SUCCEEDED(algorithmDirs->GetNextFile(getter_AddRefs(algorithmDir))) &&
      algorithmDir) {
    bool isDir = false;
    algorithmDir->IsDirectory(&isDir);
    if (!isDir) {
      continue;
    }

    nsCOMPtr<nsIDirectoryEnumerator> files;
    if (NS_FAILED(algorithmDir->GetDirectoryEntries(getter_AddRefs(files))) ||
        !files) {
      continue;
    }

    nsCOMPtr<nsIFile> file;
    while (NS_SUCCEEDED(files->GetNextFile(getter_AddRefs(file))) && file) {
      nsAutoString leaf;
      file->GetLeafName(leaf);
      if (StringEndsWith(leaf, u".tmp"_ns)) {
        // Orphaned by a write interrupted between AtomicWriteFile's rename
        // steps for `.bytes` and `.meta` (a crash, kill -9, or disk-full
        // mid-write) -- always safe to discard, the entry it belonged to
        // was never fully committed.
        file->Remove(false);
        continue;
      }
      if (!StringEndsWith(leaf, u".meta"_ns)) {
        continue;  // Either a `.bytes` file (visited via its `.meta`
                   // sibling below) or an orphan with no `.meta` at all.
      }

      PersistedEntry entry;
      if (!ReadMetaFile(file, entry)) {
        // Unreadable/corrupt metadata: skip it rather than propagate a
        // parse failure into the registry. The matching `.bytes` file (if
        // any) becomes unreachable disk space, not a crash.
        continue;
      }
      aOutEntries.AppendElement(std::move(entry));
    }
  }
}

int64_t CrossOriginStoragePersistence::GetDiskCapacity() {
  int64_t capacity = 0;
  nsresult rv = mBaseDir->GetDiskCapacity(&capacity);
  if (NS_FAILED(rv) || capacity < 0) {
    return 0;
  }
  return capacity;
}

}  // namespace mozilla::dom
