/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_CrossOriginStoragePublicHashList_h
#define mozilla_dom_CrossOriginStoragePublicHashList_h

#include <cstdint>

#include "CrossOriginStorageUtils.h"
#include "nsStringFwd.h"

namespace mozilla::dom {

// https://wicg.github.io/cross-origin-storage/#availability-gating
// The Public Hash List half of wildcard-scope's privacy gate (GREASE'ing,
// the other half, is CrossOriginStorageGrease below): a resource whose
// hash is listed here is safe to disclose to any requesting origin
// unconditionally, since its content is by definition already public and
// independently verifiable.
//
// The compiled-in list below is currently an empty seed. The gating
// *mechanism* here is the real, complete implementation of this half of
// the spec; populating the list with real, independently-verifiable
// public hashes is a separate concern (a fetch/verify/build-time-embed
// pipeline, per the Ladybird implementation notes) that this class
// deliberately doesn't fake with placeholder data. An empty list is
// strictly more correct than skipping the gate: it means "nothing is
// PHL-listed yet", not "everything is" -- so today, wildcard-scoped
// disclosure to a non-storing origin depends entirely on
// CrossOriginStorageGrease below, until the list is populated.
class CrossOriginStoragePublicHashList {
 public:
  static bool Contains(COSHashAlgorithm aAlgorithm,
                       const nsACString& aValueHex);
};

// https://wicg.github.io/cross-origin-storage/#grease
// An independent, randomized, size-capped chance of disclosure for
// wildcard-scoped entries not on the Public Hash List, so a requesting
// origin can never distinguish "not on the PHL" from "not GREASE'd this
// particular request" by retrying -- both must look identically like an
// ordinary not-found. Probability and size ceiling match Servo's and
// Ladybird's own chosen constants (neither is spec-mandated).
class CrossOriginStorageGrease {
 public:
  static bool ShouldDisclose(uint64_t aByteSize);
};

}  // namespace mozilla::dom

#endif  // mozilla_dom_CrossOriginStoragePublicHashList_h
