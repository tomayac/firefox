/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CrossOriginStorageParent.h"

#include "CrossOriginStorageRegistry.h"
#include "mozilla/PodOperations.h"
#include "mozilla/ipc/BackgroundParent.h"
#include "nsString.h"

namespace mozilla::dom {

using mozilla::ipc::IPCResult;
using mozilla::ipc::PrincipalInfo;

// A flat placeholder ceiling on a single write session's in-memory buffer,
// matching Servo's and Ladybird's own chosen starting point (both call this
// out as a stopgap, not real storage-budget accounting): without it,
// seek()/truncate() can grow WriteSession::mBytes without bound, an
// unbounded-allocation DoS. Checked before ever growing the buffer, not
// after -- the point is to never actually perform the oversized allocation.
constexpr uint64_t kMaxCOSWriteBytes = 4ull * 1024 * 1024 * 1024;  // 4 GiB

void CrossOriginStorageParent::ActorDestroy(ActorDestroyReason aWhy) {
  mozilla::ipc::AssertIsOnBackgroundThread();

  CrossOriginStorageRegistry& registry =
      CrossOriginStorageRegistry::GetOrCreate();
  for (const auto& entry : mWriteSessions) {
    const WriteSession* session = entry.GetData().get();
    registry.ReleaseOutstandingWriter(session->mAlgorithm, session->mValue);
  }
  mWriteSessions.Clear();
}

mozilla::ipc::IPCResult CrossOriginStorageParent::RecvRequestFileHandle(
    const nsCString& aAlgorithm, const nsCString& aValue, bool aCreate,
    const PrincipalInfo& aRequestingPrincipal,
    RequestFileHandleResolver&& aResolve) {
  mozilla::ipc::AssertIsOnBackgroundThread();

  // The child already validated this; re-parsing rather than trusting the
  // wire value guards against a compromised or simply buggy content
  // process reaching this trusted-side handler directly.
  Maybe<COSHashAlgorithm> algorithm =
      ParseHashAlgorithm(NS_ConvertUTF8toUTF16(aAlgorithm));
  if (!algorithm) {
    COSRequestFileHandleResult result;
    result = nsresult(NS_ERROR_DOM_NOT_FOUND_ERR);
    aResolve(result);
    return IPC_OK();
  }

  CrossOriginStorageRegistry& registry =
      CrossOriginStorageRegistry::GetOrCreate();

  if (aCreate) {
    // https://wicg.github.io/cross-origin-storage/#complete-a-create-request
    bool written = registry.CompleteCreateRequest(*algorithm, aValue);
    COSRequestFileHandleSuccess success;
    success.written() = written;
    COSRequestFileHandleResult result;
    result = success;
    aResolve(result);
    return IPC_OK();
  }

  // https://wicg.github.io/cross-origin-storage/#complete-a-read-request
  COSRequestFileHandleResult result;
  switch (
      registry.CompleteReadRequest(*algorithm, aValue, aRequestingPrincipal)) {
    case CrossOriginStorageRegistry::ReadOutcome::Found: {
      COSRequestFileHandleSuccess success;
      success.written() = true;
      result = success;
      break;
    }
    case CrossOriginStorageRegistry::ReadOutcome::Pending:
      result = nsresult(NS_ERROR_DOM_NOT_ALLOWED_ERR);
      break;
    case CrossOriginStorageRegistry::ReadOutcome::NotFound:
      result = nsresult(NS_ERROR_DOM_NOT_FOUND_ERR);
      break;
  }
  aResolve(result);
  return IPC_OK();
}

mozilla::ipc::IPCResult CrossOriginStorageParent::RecvBeginWrite(
    uint64_t aWriteId, const nsCString& aAlgorithm, const nsCString& aValue,
    const PrincipalInfo& aWritingPrincipal,
    const COSRequestedOrigins& aRequestedOrigins) {
  mozilla::ipc::AssertIsOnBackgroundThread();

  Maybe<COSHashAlgorithm> algorithm =
      ParseHashAlgorithm(NS_ConvertUTF8toUTF16(aAlgorithm));
  if (!algorithm) {
    // Nothing to resolve/reject -- BeginWrite has no response (see
    // PCrossOriginStorage.ipdl) -- so simply refuse to create a session;
    // the eventual FinishWrite for this writeId will find none and fail.
    return IPC_OK();
  }

  auto* session = mWriteSessions.GetOrInsertNew(aWriteId);
  session->mAlgorithm = *algorithm;
  session->mValue = aValue;
  session->mWritingPrincipal = aWritingPrincipal;
  session->mRequestedOrigins = aRequestedOrigins;
  return IPC_OK();
}

mozilla::ipc::IPCResult CrossOriginStorageParent::RecvWriteChunk(
    uint64_t aWriteId, nsTArray<uint8_t>&& aChunk) {
  mozilla::ipc::AssertIsOnBackgroundThread();

  WriteSession* session = mWriteSessions.Get(aWriteId);
  if (!session || session->mWriteTargetTooLarge) {
    return IPC_OK();
  }

  uint64_t endPosition = session->mPosition + aChunk.Length();
  if (endPosition > kMaxCOSWriteBytes) {
    session->mWriteTargetTooLarge = true;
    return IPC_OK();
  }

  // Positioned write: extends and zero-fills up to mPosition first if the
  // cursor was moved past the current end by a prior Seek (nsTArray::
  // SetLength value-initializes new elements -- zero for uint8_t).
  if (endPosition > session->mBytes.Length()) {
    session->mBytes.SetLength(endPosition);
  }
  PodCopy(session->mBytes.Elements() + session->mPosition, aChunk.Elements(),
          aChunk.Length());
  session->mPosition = endPosition;
  return IPC_OK();
}

mozilla::ipc::IPCResult CrossOriginStorageParent::RecvSeek(uint64_t aWriteId,
                                                           uint64_t aPosition) {
  mozilla::ipc::AssertIsOnBackgroundThread();

  WriteSession* session = mWriteSessions.Get(aWriteId);
  if (!session) {
    return IPC_OK();
  }
  session->mPosition = aPosition;
  return IPC_OK();
}

mozilla::ipc::IPCResult CrossOriginStorageParent::RecvTruncate(
    uint64_t aWriteId, uint64_t aSize) {
  mozilla::ipc::AssertIsOnBackgroundThread();

  WriteSession* session = mWriteSessions.Get(aWriteId);
  if (!session || session->mWriteTargetTooLarge) {
    return IPC_OK();
  }

  if (aSize > kMaxCOSWriteBytes) {
    session->mWriteTargetTooLarge = true;
    return IPC_OK();
  }

  session->mBytes.SetLength(aSize);
  if (session->mPosition > aSize) {
    session->mPosition = aSize;
  }
  return IPC_OK();
}

mozilla::ipc::IPCResult CrossOriginStorageParent::RecvFinishWrite(
    uint64_t aWriteId, FinishWriteResolver&& aResolve) {
  mozilla::ipc::AssertIsOnBackgroundThread();

  WriteSession* session = mWriteSessions.Get(aWriteId);
  if (!session) {
    aResolve(nsresult(NS_ERROR_UNEXPECTED));
    return IPC_OK();
  }

  if (session->mWriteTargetTooLarge) {
    // Mirrors VerifyAndStore's own hash-mismatch cleanup below: same
    // DataError, same outstanding-writer release, since no entry was ever
    // written.
    CrossOriginStorageRegistry::GetOrCreate().ReleaseOutstandingWriter(
        session->mAlgorithm, session->mValue);
    mWriteSessions.Remove(aWriteId);
    aResolve(nsresult(NS_ERROR_DOM_DATA_ERR));
    return IPC_OK();
  }

  COSRequestedOriginsValue requestedOrigins;
  if (session->mRequestedOrigins.wildcard()) {
    requestedOrigins.mKind = COSRequestedOriginsValue::Kind::Wildcard;
  } else if (!session->mRequestedOrigins.list().IsEmpty()) {
    requestedOrigins.mKind = COSRequestedOriginsValue::Kind::List;
    requestedOrigins.mList = session->mRequestedOrigins.list().Clone();
  }

  // https://wicg.github.io/cross-origin-storage/#verify-and-store
  nsresult rv = CrossOriginStorageRegistry::GetOrCreate().VerifyAndStore(
      session->mAlgorithm, session->mValue, session->mBytes,
      session->mWritingPrincipal, requestedOrigins);

  mWriteSessions.Remove(aWriteId);

  aResolve(rv);
  return IPC_OK();
}

mozilla::ipc::IPCResult CrossOriginStorageParent::RecvAbortWrite(
    uint64_t aWriteId) {
  mozilla::ipc::AssertIsOnBackgroundThread();

  WriteSession* session = mWriteSessions.Get(aWriteId);
  if (!session) {
    return IPC_OK();
  }

  // The explicit-abandonment fast path; see
  // CrossOriginStorageRegistry::ReleaseOutstandingWriter.
  CrossOriginStorageRegistry::GetOrCreate().ReleaseOutstandingWriter(
      session->mAlgorithm, session->mValue);

  mWriteSessions.Remove(aWriteId);
  return IPC_OK();
}

mozilla::ipc::IPCResult CrossOriginStorageParent::RecvGetFileBytes(
    const nsCString& aAlgorithm, const nsCString& aValue,
    GetFileBytesResolver&& aResolve) {
  mozilla::ipc::AssertIsOnBackgroundThread();

  Maybe<COSHashAlgorithm> algorithm =
      ParseHashAlgorithm(NS_ConvertUTF8toUTF16(aAlgorithm));
  if (!algorithm) {
    COSGetFileBytesResult result;
    result = nsresult(NS_ERROR_DOM_NOT_FOUND_ERR);
    aResolve(result);
    return IPC_OK();
  }

  nsTArray<uint8_t> bytes;
  CrossOriginStorageRegistry::ReadOutcome outcome =
      CrossOriginStorageRegistry::GetOrCreate().GetFileBytes(*algorithm, aValue,
                                                             bytes);

  COSGetFileBytesResult result;
  switch (outcome) {
    case CrossOriginStorageRegistry::ReadOutcome::Found:
      result = std::move(bytes);
      break;
    case CrossOriginStorageRegistry::ReadOutcome::Pending:
      result = nsresult(NS_ERROR_DOM_NOT_ALLOWED_ERR);
      break;
    case CrossOriginStorageRegistry::ReadOutcome::NotFound:
      result = nsresult(NS_ERROR_DOM_NOT_FOUND_ERR);
      break;
  }
  aResolve(result);
  return IPC_OK();
}

}  // namespace mozilla::dom
