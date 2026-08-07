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
