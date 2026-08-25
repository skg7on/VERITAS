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
#include <string_view>
#include <type_traits>
#include <variant>

#include "veritas/facts/RelationSchema.h"

namespace veritas::facts {

namespace {

// Length-prefixed field. The length is what makes the encoding injective:
// a cell can contain ':' or '|' or a leading digit without being able to
// imitate a field boundary, because the reader never scans for a delimiter.
void AppendField(std::string* out, std::string_view value) {
  out->append(std::to_string(value.size()));
  out->push_back(':');
  out->append(value);
}

}  // namespace

std::string EncodeSemanticKey(const SemanticRow& row) {
  std::string bytes;
  bytes.append("veritas.semantic-key.v1");
  bytes.push_back('|');
  AppendField(&bytes, RelationsV2().Get(row.relation).name);
  AppendField(&bytes, std::to_string(row.cells.size()));
  for (const auto& cell : row.cells) {
    std::visit(
        [&](const auto& value) {
          using T = std::decay_t<decltype(value)>;
          // The type tag keeps domains apart, so the string "1" and the
          // number 1 never encode alike.
          if constexpr (std::is_same_v<T, core::StableId>) {
            bytes.push_back('I');
            AppendField(&bytes, core::ToString(value));
          } else if constexpr (std::is_same_v<T, std::string>) {
            bytes.push_back('S');
            AppendField(&bytes, value);
          } else if constexpr (std::is_same_v<T, std::int64_t>) {
            bytes.push_back('N');
            AppendField(&bytes, std::to_string(value));
          } else if constexpr (std::is_same_v<T, std::uint64_t>) {
            bytes.push_back('U');
            AppendField(&bytes, std::to_string(value));
          } else {
            bytes.push_back('E');
            AppendField(&bytes,
                        std::to_string(static_cast<std::uint64_t>(value)));
          }
        },
        cell);
  }
  return bytes;
}

}  // namespace veritas::facts
