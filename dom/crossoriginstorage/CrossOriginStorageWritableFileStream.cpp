/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CrossOriginStorageWritableFileStream.h"

#include "WritableStreamDefaultWriterAbstract.h"
#include "mozilla/dom/CrossOriginStorageBinding.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/UnderlyingSinkCallbackHelpers.h"
#include "mozilla/dom/WritableStreamDefaultWriter.h"

namespace mozilla::dom {

CrossOriginStorageWritableFileStream::CrossOriginStorageWritableFileStream(
    nsIGlobalObject* aGlobal)
    : WritableStream(aGlobal, HoldDropJSObjectsCaller::Implicit) {}

// static
already_AddRefed<CrossOriginStorageWritableFileStream>
CrossOriginStorageWritableFileStream::Create(
    JSContext* aCx, nsIGlobalObject* aGlobal,
    UnderlyingSinkAlgorithmsWrapper& aAlgorithms, ErrorResult& aRv) {
  RefPtr<CrossOriginStorageWritableFileStream> stream =
      new CrossOriginStorageWritableFileStream(aGlobal);
  stream->SetUpNative(aCx, aAlgorithms, Nothing(), nullptr, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }
  return stream.forget();
}

JSObject* CrossOriginStorageWritableFileStream::WrapObject(
    JSContext* aCx, JS::Handle<JSObject*> aGivenProto) {
  return CrossOriginStorageWritableFileStream_Binding::Wrap(aCx, this,
                                                            aGivenProto);
}

// https://fs.spec.whatwg.org/#dom-filesystemwritablefilestream-write
already_AddRefed<Promise> CrossOriginStorageWritableFileStream::Write(
    JSContext* aCx, JS::Handle<JS::Value> aData, ErrorResult& aRv) {
  // Step 1: let writer be the result of getting a writer for this.
  RefPtr<WritableStreamDefaultWriter> writer =
      new WritableStreamDefaultWriter(GetParentObject());
  streams_abstract::SetUpWritableStreamDefaultWriter(writer, this, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  // Step 2: let result be the result of writing to writer given data.
  RefPtr<Promise> result = streams_abstract::WritableStreamDefaultWriterWrite(
      aCx, writer, aData, aRv);

  // Step 3: release writer.
  streams_abstract::WritableStreamDefaultWriterRelease(aCx, writer);

  // Step 4: return result.
  return result.forget();
}

}  // namespace mozilla::dom
