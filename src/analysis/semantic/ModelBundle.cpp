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

#include "veritas/analysis/semantic/ModelBundle.h"

#include <array>
#include <cstddef>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>

#include "veritas/core/Hash.h"

namespace veritas::analysis::semantic {
namespace {

using veritas::core::MakeStableId;

// Read an entire file into a string. Returns nullopt on I/O failure.
std::optional<std::string> ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// True when `c` is an ASCII control character (C0 or DEL). Fields are
// rejected when they contain any control character so the bundle can never
// smuggle formatting or control bytes into a stable identity.
bool IsControl(char c) {
  const auto value = static_cast<unsigned char>(c);
  return value < 0x20 || value == 0x7F;
}

std::optional<ModelEffectKind> ParseEffectKind(std::string_view token) {
  if (token == "read") return ModelEffectKind::kRead;
  if (token == "write") return ModelEffectKind::kWrite;
  if (token == "allocate") return ModelEffectKind::kAllocate;
  if (token == "deallocate") return ModelEffectKind::kDeallocate;
  if (token == "unknown") return ModelEffectKind::kUnknown;
  return std::nullopt;
}

std::optional<EpistemicState> ParseEpistemic(std::string_view token) {
  if (token == "must") return EpistemicState::kMust;
  if (token == "may") return EpistemicState::kMay;
  if (token == "must_not") return EpistemicState::kMustNot;
  if (token == "inferred") return EpistemicState::kInferred;
  if (token == "assumed") return EpistemicState::kAssumed;
  if (token == "unknown") return EpistemicState::kUnknown;
  return std::nullopt;
}

// Split `line` into exactly `count` tab-separated fields. A field may not
// contain a control character (which also rules out embedded tabs, which
// would otherwise change the field count). Returns nullopt on any violation.
std::optional<std::array<std::string, 5>> SplitFields(std::string_view line) {
  std::array<std::string, 5> fields;
  std::size_t field = 0;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= line.size(); ++i) {
    const bool at_end = (i == line.size());
    if (!at_end && line[i] != '\t') {
      if (IsControl(line[i])) return std::nullopt;
      continue;
    }
    if (field >= fields.size()) return std::nullopt;  // too many fields
    fields[field] = std::string(line.substr(start, i - start));
    ++field;
    start = i + 1;
  }
  if (field != fields.size()) return std::nullopt;  // too few fields
  return fields;
}

}  // namespace

StatusOr<ModelBundle> ModelBundle::Load(const std::filesystem::path& rows,
                                        const std::filesystem::path& manifest) {
  // 1. Read and parse the one-line manifest.
  auto manifest_bytes = ReadFile(manifest);
  if (!manifest_bytes.has_value()) {
    return Status::InvalidArgument("cannot read model bundle manifest");
  }
  std::string_view manifest_line = *manifest_bytes;
  // Strip a trailing newline and surrounding whitespace.
  while (!manifest_line.empty() &&
         (manifest_line.back() == '\n' || manifest_line.back() == '\r')) {
    manifest_line.remove_suffix(1);
  }
  constexpr std::string_view kVersionPrefix = "model_bundle_version=";
  if (!manifest_line.starts_with(kVersionPrefix) ||
      manifest_line.size() == kVersionPrefix.size()) {
    return Status::InvalidArgument("malformed model bundle manifest");
  }
  const std::string version(manifest_line.substr(kVersionPrefix.size()));

  // 2. Read the raw TSV bytes; they form the canonical content for the hash.
  auto rows_bytes = ReadFile(rows);
  if (!rows_bytes.has_value()) {
    return Status::InvalidArgument("cannot read model bundle rows");
  }
  const std::string canonical_bytes = *rows_bytes;

  // 3. Parse and validate each row.
  ModelBundle bundle;
  bundle.version_ = version;

  std::set<std::string> seen_seeds;
  std::optional<std::string> previous_symbol;
  std::optional<std::string> previous_seed;

  std::size_t line_start = 0;
  while (line_start <= canonical_bytes.size()) {
    std::size_t line_end = canonical_bytes.find('\n', line_start);
    if (line_end == std::string::npos) line_end = canonical_bytes.size();
    std::string_view line(canonical_bytes.data() + line_start,
                          line_end - line_start);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

    if (!line.empty()) {
      auto fields = SplitFields(line);
      if (!fields.has_value()) {
        return Status::InvalidArgument(
            "model row is not five control-free tab-separated fields");
      }
      const std::string& seed = (*fields)[0];
      const std::string& symbol = (*fields)[1];
      const auto effect = ParseEffectKind((*fields)[2]);
      const auto epistemic = ParseEpistemic((*fields)[4]);
      if (!effect.has_value()) {
        return Status::InvalidArgument("unknown model effect kind: " +
                                       (*fields)[2]);
      }
      if (!epistemic.has_value()) {
        return Status::InvalidArgument("unknown model epistemic state: " +
                                       (*fields)[4]);
      }

      // Canonical order is lexicographic by (symbol, seed).
      if (previous_symbol.has_value()) {
        const bool sorted =
            *previous_symbol < symbol ||
            (*previous_symbol == symbol && *previous_seed < seed);
        if (!sorted) {
          return Status::InvalidArgument("model rows are not sorted");
        }
      }

      // Model IDs are derived from the seed and must be globally unique.
      if (!seen_seeds.insert(seed).second) {
        return Status::InvalidArgument("duplicate model ID seed: " + seed);
      }

      FunctionModel model;
      model.model_id = MakeStableId(
          core::IdKind::kModel,
          std::as_bytes(std::span(seed.data(), seed.size())));
      model.symbol = symbol;
      model.effect = *effect;
      model.subject = (*fields)[3];
      model.epistemic = *epistemic;
      bundle.models_.push_back(std::move(model));

      previous_symbol = symbol;
      previous_seed = seed;
    }

    if (line_end == canonical_bytes.size()) break;
    line_start = line_end + 1;
  }

  // 4. Build the symbol -> range index over the sorted model table.
  for (std::size_t i = 0; i < bundle.models_.size(); ++i) {
    const std::string& symbol = bundle.models_[i].symbol;
    auto it = bundle.index_.find(symbol);
    if (it == bundle.index_.end()) {
      bundle.index_.emplace(symbol, std::make_pair(i, i + 1));
    } else {
      it->second.second = i + 1;
    }
  }

  // 5. Content hash = version + canonical TSV bytes.
  std::string hash_input = version;
  hash_input += canonical_bytes;
  const auto digest = core::ComputeSHA256(
      std::as_bytes(std::span(hash_input.data(), hash_input.size())));
  bundle.hash_ = core::DigestToHex(digest);

  return bundle;
}

std::span<const FunctionModel> ModelBundle::Lookup(
    std::string_view symbol) const {
  const auto it = index_.find(std::string(symbol));
  if (it == index_.end()) {
    return {};
  }
  const auto [begin, end] = it->second;
  return std::span<const FunctionModel>(models_.data() + begin, end - begin);
}

}  // namespace veritas::analysis::semantic
