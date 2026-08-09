/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CrossOriginStorageParent.h"

#include "CrossOriginStorageRegistry.h"
#include "mozilla/ipc/BackgroundParent.h"
#include "nsString.h"

namespace mozilla::dom {

using mozilla::ipc::IPCResult;
using mozilla::ipc::PrincipalInfo;

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
    const PrincipalInfo& aWritingPrincipal) {
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
  return IPC_OK();
}

mozilla::ipc::IPCResult CrossOriginStorageParent::RecvWriteChunk(
    uint64_t aWriteId, nsTArray<uint8_t>&& aChunk) {
  mozilla::ipc::AssertIsOnBackgroundThread();

  WriteSession* session = mWriteSessions.Get(aWriteId);
  if (!session) {
    return IPC_OK();
  }
  session->mBytes.AppendElements(aChunk);
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

  // https://wicg.github.io/cross-origin-storage/#verify-and-store
  nsresult rv = CrossOriginStorageRegistry::GetOrCreate().VerifyAndStore(
      session->mAlgorithm, session->mValue, session->mBytes,
      session->mWritingPrincipal);

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

}  // namespace mozilla::dom
