/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_CrossOriginStorageRequestHandler_h
#define mozilla_dom_CrossOriginStorageRequestHandler_h

#include "CrossOriginStorageUtils.h"
#include "fs/FileSystemRequestHandler.h"
#include "mozilla/RefPtr.h"
#include "nsString.h"

namespace mozilla::dom {

class CrossOriginStorageChild;

// Backs GetFile()/CreateWritable() for a FileSystemFileHandle obtained from
// navigator.crossOriginStorage.requestFileHandle(), routing those calls to
// PCrossOriginStorage instead of the default FileSystemRequestHandler's
// PFileSystemManager/QuotaManager-backed path. One instance per handle,
// constructed with the hash it addresses -- unlike the default handler, the
// FileSystemManager& every method below still takes is unused (always
// null; see CrossOriginStorageManager::EnsureActor()'s caller).
class CrossOriginStorageRequestHandler final
    : public fs::FileSystemRequestHandler {
 public:
  CrossOriginStorageRequestHandler(RefPtr<CrossOriginStorageChild> aActor,
                                   COSHashAlgorithm aAlgorithm,
                                   const nsACString& aValue);

  // https://wicg.github.io/cross-origin-storage/#cos-file-system
  void GetFile(RefPtr<FileSystemManager>& aManager,
               const fs::FileSystemEntryMetadata& aFile,
               RefPtr<Promise> aPromise, ErrorResult& aError) override;

  // https://wicg.github.io/cross-origin-storage/#creating-and-writing-files
  void GetWritable(RefPtr<FileSystemManager>& aManager,
                   const fs::FileSystemEntryMetadata& aFile, bool aKeepData,
                   const RefPtr<Promise>& aPromise,
                   ErrorResult& aError) override;

 private:
  RefPtr<CrossOriginStorageChild> mActor;
  COSHashAlgorithm mAlgorithm;
  nsCString mValue;
};

}  // namespace mozilla::dom

#endif  // mozilla_dom_CrossOriginStorageRequestHandler_h
