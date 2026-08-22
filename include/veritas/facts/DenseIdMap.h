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

// DenseIdMap.h — deterministic run-local dense-ID mapping.
//
// Dense IDs are assigned in sorted stable-ID order so assignments are
// independent of input order and reproducible across runs. The map rejects
// stable IDs whose kind does not match the map's declared domain.

#ifndef VERITAS_FACTS_DENSE_ID_MAP_H_
#define VERITAS_FACTS_DENSE_ID_MAP_H_

#include <algorithm>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/facts/RelationSchema.h"

namespace veritas::facts {

template <typename Dense, core::IdKind Kind>
class DenseIdMap {
 public:
  static StatusOr<DenseIdMap> Build(std::vector<core::StableId> stable_ids) {
    for (const auto& id : stable_ids) {
      if (id.kind != Kind) {
        return Status::InvalidArgument("stable id kind mismatch");
      }
    }
    std::sort(stable_ids.begin(), stable_ids.end());
    stable_ids.erase(std::unique(stable_ids.begin(), stable_ids.end()),
                     stable_ids.end());

    DenseIdMap map;
    map.dense_to_stable_ = stable_ids;
    for (std::uint32_t i = 0; i < stable_ids.size(); ++i) {
      map.stable_to_dense_.emplace(stable_ids[i], Dense{i});
    }
    return map;
  }

  StatusOr<Dense> ToDense(const core::StableId& stable) const {
    const auto it = stable_to_dense_.find(stable);
    if (it == stable_to_dense_.end()) {
      return Status::NotFound("stable id is not in the dense map");
    }
    return it->second;
  }

  StatusOr<core::StableId> ToStable(Dense dense) const {
    if (dense.value >= dense_to_stable_.size()) {
      return Status::NotFound("dense id is out of range");
    }
    return dense_to_stable_[dense.value];
  }

  std::span<const core::StableId> StableIds() const { return dense_to_stable_; }

 private:
  std::vector<core::StableId> dense_to_stable_;
  std::map<core::StableId, Dense> stable_to_dense_;
};

using FunctionDenseMap = DenseIdMap<FunctionId, core::IdKind::kFunctionVariant>;
using ValueDenseMap = DenseIdMap<ValueId, core::IdKind::kValueRef>;
using MemoryDenseMap = DenseIdMap<MemoryId, core::IdKind::kMemoryRef>;
using CallSiteDenseMap = DenseIdMap<CallSiteId, core::IdKind::kCallSite>;

}  // namespace veritas::facts

#endif  // VERITAS_FACTS_DENSE_ID_MAP_H_
