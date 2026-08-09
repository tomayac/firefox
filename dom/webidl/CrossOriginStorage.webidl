/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * The origin of this IDL file is
 * https://wicg.github.io/cross-origin-storage/
 */

[Exposed=(Window,Worker), SecureContext, Pref="dom.crossOriginStorage.enabled"]
interface CrossOriginStorageManager {
  [NewObject]
  Promise<FileSystemFileHandle> requestFileHandle(
      CrossOriginStorageRequestFileHandleHash hash,
      optional CrossOriginStorageRequestFileHandleOptions options = {});
};

dictionary CrossOriginStorageRequestFileHandleHash {
  required DOMString value;
  required DOMString algorithm;
};

dictionary CrossOriginStorageRequestFileHandleOptions {
  boolean create = false;
  (DOMString or sequence<DOMString>) origins;
};

[SecureContext]
interface mixin NavigatorCrossOriginStorage {
  [SameObject, Pref="dom.crossOriginStorage.enabled"]
  readonly attribute CrossOriginStorageManager crossOriginStorage;
};

// A Phase 1-scoped stand-in for FileSystemFileHandle.createWritable()'s
// real return type, FileSystemWritableFileStream (see
// CrossOriginStorageRequestHandler::GetWritable() for why): adds the same
// write(data) convenience method, without FileSystemWritableFileStream's
// seek()/truncate(), which this phase's in-memory, capped-size writes
// don't need.
[Exposed=(Window,Worker), SecureContext, Pref="dom.crossOriginStorage.enabled"]
interface CrossOriginStorageWritableFileStream : WritableStream {
  [NewObject]
  Promise<undefined> write(any data);
};
