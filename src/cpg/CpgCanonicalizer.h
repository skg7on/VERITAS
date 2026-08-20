// Copyright 2026 VERITAS Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef VERITAS_CPG_CPGCANONICALIZER_H_
#define VERITAS_CPG_CPGCANONICALIZER_H_

#include <string>

#include "veritas/core/Ids.h"
#include "veritas/cpg/ThinCpg.h"

namespace veritas::cpg {

// CpgCanonicalizer produces the deterministic canonical graph bytes and the
// content-addressed ProjectionID from an in-memory ThinCpg. Insertion order,
// native pointers, and hash-table order never affect the result: nodes, edges,
// summary IDs, and support records are sorted by canonical key first.
class CpgCanonicalizer {
 public:
  // Deterministic length-prefixed byte string covering schema version, revision
  // and build-variant identity, module hash, sorted summary IDs, sorted nodes,
  // and sorted edges (including support records).
  static std::string CanonicalBytes(const ThinCpg& graph);

  // Content-addressed ProjectionID derived from the canonical bytes.
  static core::StableId ProjectionId(const ThinCpg& graph);
};

}  // namespace veritas::cpg

#endif  // VERITAS_CPG_CPGCANONICALIZER_H_
