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

// RelationIo.h — typed relation exchange with a generated Souffle program.
//
// Souffle reads and writes tab-separated relation files. This is the typed
// boundary around that: input rows are written per relation in schema column
// order, and output is read back and validated against the same schema rather
// than trusted. A cell that does not belong to its column's domain, a
// relation the component does not produce, or a dense id with no mapping all
// fail the component instead of producing a plausible-looking fact.
//
// Lives in wpa rather than facts because it needs the component's dual-
// identity maps to turn dense output cells back into semantic content, and
// veritas_facts does not link veritas_wpa.

#ifndef VERITAS_WPA_RELATION_IO_H_
#define VERITAS_WPA_RELATION_IO_H_

#include <filesystem>

#include "veritas/core/Status.h"
#include "veritas/facts/Witness.h"
#include "veritas/wpa/WpaComponent.h"

namespace veritas::wpa {

class RelationIo {
 public:
  // Writes one "<Relation>.facts" file per EDB relation present in the input,
  // tab-separated, in schema column order. Fails if a symbol cell contains a
  // tab or newline, which would silently shift columns.
  static Status WriteInput(const std::filesystem::path& directory,
                           const WpaLogicalComponentInput& input);

  // Reads "<Derived>.csv" and "Witness.csv" back into a raw evaluation.
  // Results arrive as dense cells and are mapped back through the input;
  // witnesses arrive as semantic keys and are decoded against the schema.
  static StatusOr<facts::RawWpaEvaluation> ReadOutput(
      const std::filesystem::path& directory,
      const WpaLogicalComponentInput& input);
};

}  // namespace veritas::wpa

#endif  // VERITAS_WPA_RELATION_IO_H_
