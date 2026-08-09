/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CrossOriginStorageWritableFileStream.h"

#include "WritableStreamDefaultWriterAbstract.h"
#include "js/PropertyAndElement.h"
#include "jsapi.h"
#include "mozilla/dom/CrossOriginStorageBinding.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/ScriptSettings.h"
#include "mozilla/dom/UnderlyingSinkCallbackHelpers.h"
#include "mozilla/dom/WritableStreamDefaultWriter.h"

namespace mozilla::dom {

namespace {

// Builds a plain {type, <key>: value} object -- the same discriminated
// shape the File System Standard's own write() sink accepts
// (https://fs.spec.whatwg.org/#dictdef-writeparams) -- so seek()/
// truncate() can be expressed as ordinary writes through the same
// underlying sink queue as real data, keeping them correctly serialized
// relative to write() calls. See
// CrossOriginStorageSinkAlgorithms::WriteCallbackImpl for the other end.
bool MakeCommandObject(JSContext* aCx, const char* aType, const char* aKey,
                       uint64_t aValue, JS::MutableHandle<JS::Value> aOut) {
  JS::Rooted<JSObject*> obj(aCx, JS_NewPlainObject(aCx));
  if (!obj) {
    return false;
  }
  JS::Rooted<JS::Value> typeVal(aCx);
  JSString* typeStr = JS_NewStringCopyZ(aCx, aType);
  if (!typeStr) {
    return false;
  }
  typeVal.setString(typeStr);
  if (!JS_DefineProperty(aCx, obj, "type", typeVal, JSPROP_ENUMERATE)) {
    return false;
  }
  JS::Rooted<JS::Value> value(aCx);
  value.setNumber(static_cast<double>(aValue));
  if (!JS_DefineProperty(aCx, obj, aKey, value, JSPROP_ENUMERATE)) {
    return false;
  }
  aOut.setObject(*obj);
  return true;
}

}  // namespace

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

// https://fs.spec.whatwg.org/#dom-filesystemwritablefilestream-seek
already_AddRefed<Promise> CrossOriginStorageWritableFileStream::Seek(
    uint64_t aPosition, ErrorResult& aRv) {
  AutoJSAPI jsapi;
  if (!jsapi.Init(GetParentObject())) {
    aRv.ThrowUnknownError("Internal error");
    return nullptr;
  }
  JSContext* cx = jsapi.cx();

  JS::Rooted<JS::Value> command(cx);
  if (!MakeCommandObject(cx, "seek", "position", aPosition, &command)) {
    aRv.StealExceptionFromJSContext(cx);
    return nullptr;
  }
  return Write(cx, command, aRv);
}

// https://fs.spec.whatwg.org/#dom-filesystemwritablefilestream-truncate
already_AddRefed<Promise> CrossOriginStorageWritableFileStream::Truncate(
    uint64_t aSize, ErrorResult& aRv) {
  AutoJSAPI jsapi;
  if (!jsapi.Init(GetParentObject())) {
    aRv.ThrowUnknownError("Internal error");
    return nullptr;
  }
  JSContext* cx = jsapi.cx();

  JS::Rooted<JS::Value> command(cx);
  if (!MakeCommandObject(cx, "truncate", "size", aSize, &command)) {
    aRv.StealExceptionFromJSContext(cx);
    return nullptr;
  }
  return Write(cx, command, aRv);
}

}  // namespace mozilla::dom
