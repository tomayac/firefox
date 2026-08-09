/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_CrossOriginStorageWritableFileStream_h
#define mozilla_dom_CrossOriginStorageWritableFileStream_h

#include "mozilla/dom/WritableStream.h"

namespace mozilla::dom {

class UnderlyingSinkAlgorithmsWrapper;

// https://wicg.github.io/cross-origin-storage/#creating-and-writing-files
// A minimal, Phase 1-scoped stand-in for FileSystemWritableFileStream (see
// CrossOriginStorageRequestHandler::GetWritable() for the full reasoning):
// adds just the write(data) convenience method the File System Standard's
// FileSystemWritableFileStream has, implemented the same way that spec
// defines it -- acquire a writer, write, release the writer -- on top of
// the same native UnderlyingSinkAlgorithmsWrapper machinery the base
// WritableStream already provides. Deliberately omits
// FileSystemWritableFileStream's seek()/truncate() and its real
// disk-backed RandomAccessStreamParams machinery, neither of which this
// phase's in-memory, capped-size writes need.
class CrossOriginStorageWritableFileStream final : public WritableStream {
 public:
  static already_AddRefed<CrossOriginStorageWritableFileStream> Create(
      JSContext* aCx, nsIGlobalObject* aGlobal,
      UnderlyingSinkAlgorithmsWrapper& aAlgorithms, ErrorResult& aRv);

  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;

  // https://fs.spec.whatwg.org/#dom-filesystemwritablefilestream-write
  MOZ_CAN_RUN_SCRIPT already_AddRefed<Promise> Write(
      JSContext* aCx, JS::Handle<JS::Value> aData, ErrorResult& aRv);

 private:
  explicit CrossOriginStorageWritableFileStream(nsIGlobalObject* aGlobal);
  ~CrossOriginStorageWritableFileStream() override = default;
};

}  // namespace mozilla::dom

#endif  // mozilla_dom_CrossOriginStorageWritableFileStream_h
