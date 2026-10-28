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

#ifndef VERITAS_REVIEW_STRUCTURAL_INDEX_H_
#define VERITAS_REVIEW_STRUCTURAL_INDEX_H_
#include "veritas/review/SnapshotCapture.h"
#include <memory>
namespace veritas::review {
struct StructuralBudget {
  uint64_t max_tus = 16, max_entities = 10000,
           max_source_bytes = 16 * 1024 * 1024;
  uint64_t wall_ms = 60000, output_bytes = 4 * 1024 * 1024;
  uint64_t child_rss_bytes = 1024 * 1024 * 1024;
};
struct StructuralMetrics {
  uint64_t compiler_invocations = 0, svf_invocations = 0,
           wpa_components_executed = 0;
};
struct StructuralEntity {
  uint32_t ordinal;
  std::string kind, name, path, type;
  uint64_t start, end;
  ContextId source_ref;
  std::optional<uint32_t> operand_index;
  bool direct_call = false;
  std::vector<uint32_t> operands;
};
struct StructuralIndexData;
struct LocalRequest;
struct LocalEvidence;
class StructuralIndex {
public:
  const StructuralIndexId &id() const;
  const ReviewBinding &binding() const;
  const std::vector<StructuralEntity> &entities() const;
  const Coverage &coverage() const;
  const Usage &usage() const;
  const StructuralMetrics &metrics() const;
  // Context is pinned to captured UTF-8 bytes and advertises only contained
  // refs.
  StatusOr<ContextArtifact> Context(std::string_view path, uint64_t start,
                                    uint64_t end) const;

private:
  explicit StructuralIndex(std::shared_ptr<const StructuralIndexData> data)
      : data_(std::move(data)) {}
  std::shared_ptr<const StructuralIndexData> data_;
  friend StatusOr<StructuralIndex>
  BuildStructuralIndex(const ReviewSnapshot &, const StructuralBudget &);
  friend StatusOr<LocalEvidence> InvestigateLocal(const LocalRequest &);
};
// Invalid/unsafe commands and nonpositive limits are refused. Compiler errors
// yield an explicitly partial index; budget exhaustion never certifies absence.
StatusOr<StructuralIndex> BuildStructuralIndex(const ReviewSnapshot &,
                                               const StructuralBudget &);
} // namespace veritas::review
#endif
