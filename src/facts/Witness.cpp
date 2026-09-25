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

#include "veritas/facts/Witness.h"

#include <cstdint>
#include <string>
#include <type_traits>
#include <variant>

#include "veritas/facts/RelationSchema.h"
#include "veritas/facts/SemanticKeyCodec.h"

namespace veritas::facts {

// A cell alternative this encoder does not know. A new alternative in
// `SemanticCellValue` must be encoded here; leaving it out would let two rows
// that differ only in it derive the same semantic key and, through
// `DeriveWitnessId`, the same witness id, which merges provenance rather than
// rejecting a batch. The dependent-false form is what makes the `static_assert`
// below fire instead of being accepted as an unreachable statement; it mirrors
// the preimage encoder in `AnalysisFact.cpp`.
template <typename T>
[[maybe_unused]] inline constexpr bool kUnencodedCellKind = false;

void AppendSemanticKey(std::string* out, const SemanticRow& row) {
  // Delegates to the field codec rather than carrying a second encoding: the
  // Souffle functors expose exactly these primitives, so a key built here and
  // a key built by a generated program are the same bytes by construction.
  const auto& schema = RelationsV2().Get(row.relation);
  AppendKeyHeader(out, schema.name, row.cells.size());
  for (const auto& cell : row.cells) {
    std::visit(
        [out](const auto& value) {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, core::StableId>) {
            AppendField(out, KeyFieldTag::kId, core::ToString(value));
          } else if constexpr (std::is_same_v<T, std::string>) {
            AppendField(out, KeyFieldTag::kSymbol, value);
          } else if constexpr (std::is_same_v<T, std::int64_t>) {
            AppendField(out, KeyFieldTag::kNumber, std::to_string(value));
          } else if constexpr (std::is_same_v<T, std::uint64_t>) {
            AppendField(out, KeyFieldTag::kUnsigned, std::to_string(value));
          } else if constexpr (std::is_same_v<T, semantic::DispatchKind> ||
                               std::is_same_v<T, semantic::AliasKind> ||
                               std::is_same_v<T, semantic::ByteRangeKind> ||
                               std::is_same_v<T, semantic::EpistemicState>) {
            // The typed semantic enums travel as their ordinal, which is the
            // same encoding the Datalog side uses for these columns.
            AppendField(out, KeyFieldTag::kEnum,
                        std::to_string(static_cast<std::uint64_t>(value)));
          } else {
            static_assert(kUnencodedCellKind<T>,
                          "SemanticCellValue gained an alternative the "
                          "semantic-key encoding does not know, so rows "
                          "differing only in that cell would derive one key");
          }
        },
        cell);
  }
}

std::string EncodeSemanticKey(const SemanticRow& row) {
  std::string key;
  AppendSemanticKey(&key, row);
  return key;
}

}  // namespace veritas::facts
