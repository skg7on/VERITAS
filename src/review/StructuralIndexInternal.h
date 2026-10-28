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

#ifndef VERITAS_REVIEW_STRUCTURAL_INDEX_INTERNAL_H_
#define VERITAS_REVIEW_STRUCTURAL_INDEX_INTERNAL_H_
#include "veritas/review/StructuralIndex.h"
namespace veritas::review {
struct GuardRecord {
  uint32_t condition;
  bool dominates;
};
struct CopyRecord {
  uint32_t callsite;
  bool admitted = false;
  std::optional<uint64_t> extent;
  std::vector<GuardRecord> guards;
  std::vector<std::string> reasons;
};
struct StructuralIndexData {
  ReviewSnapshot snapshot;
  StructuralIndexId id;
  ReviewBinding binding;
  ProviderVersion producer;
  std::vector<StructuralEntity> entities;
  std::map<uint32_t, CopyRecord> copies;
  Coverage coverage;
  Usage usage;
  StructuralMetrics metrics;
};
Json CoverageJson(const Coverage &);
Json UsageJson(const Usage &);
Json RefJson(const EntityRef &);
} // namespace veritas::review
#endif
