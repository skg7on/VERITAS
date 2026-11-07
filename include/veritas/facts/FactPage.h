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

#ifndef VERITAS_FACTS_FACT_PAGE_H_
#define VERITAS_FACTS_FACT_PAGE_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "veritas/facts/AnalysisFact.h"

namespace veritas::facts {

struct CellEquality {
  std::uint32_t column;
  ColumnDomain domain;
  SemanticCellValue value;
};

struct FactSelector {
  RelationId relation;
  std::vector<CellEquality> equalities;
};

inline constexpr std::uint32_t kFactPageOrderingVersion = 1;
inline constexpr std::uint32_t kMaxFactPageRows = 4096;

struct PageCursor {
  std::string snapshot_digest;
  std::string selector_digest;
  core::StableId last_consumed;
  std::uint32_t ordering_version = kFactPageOrderingVersion;
};

struct PageRequest {
  std::uint32_t limit = kMaxFactPageRows;
  std::uint64_t max_examined = 100000;
  std::uint64_t max_output_bytes = 1024 * 1024;
  std::optional<PageCursor> after;
};

struct FactPage {
  std::vector<AnalysisFact> facts;
  std::uint64_t examined = 0;
  // Sum of the canonical serialized Fact message sizes.
  std::uint64_t output_bytes = 0;
  std::optional<PageCursor> continuation;
  // Exhaustion only describes traversal of stored rows, not semantic closure.
  bool exhausted = false;
  bool work_exhausted = false;
  // Total allowance needed to include the next matching fact in this page.
  std::optional<std::uint64_t> required_output_bytes;
};

} // namespace veritas::facts

#endif // VERITAS_FACTS_FACT_PAGE_H_
