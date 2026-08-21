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

#ifndef VERITAS_WPA_SCC_STATE_REPOSITORY_H_
#define VERITAS_WPA_SCC_STATE_REPOSITORY_H_

#include <cstddef>
#include <optional>
#include <string>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/summary/v1/summary.pb.h"
#include "veritas/summarydb/MetadataStore.h"
#include "veritas/wpa/CallGraph.h"
#include "veritas/wpa/FixpointEngine.h"
#include "veritas/wpa/SccGraph.h"

namespace veritas::wpa {

struct SccContext {
  std::string revision_id;
  std::string build_variant_id;
};

struct StoredSccState {
  core::StableId scc_id;
  summary::v1::ComponentKind component_kind;
  std::string input_hash;
  std::string fixpoint_hash;
  std::string externally_visible_hash;
  std::size_t iteration_count;
  SccStatus status;
};

enum class ExternalChange {
  kUnchanged,
  kChanged,
};

class SccStateRepository {
 public:
  explicit SccStateRepository(summarydb::MetadataStore& metadata_store)
      : metadata_store_(metadata_store) {}

  Status PublishGraph(const SccContext& context,
                      const CallGraph& call_graph,
                      const SccGraph& scc_graph);
  StatusOr<std::optional<StoredSccState>> LoadState(
      const SccContext& context, core::StableId scc_id,
      summary::v1::ComponentKind component_kind) const;
  StatusOr<ExternalChange> StoreState(const SccContext& context,
                                      const SccResult& result);

 private:
  summarydb::MetadataStore& metadata_store_;
};

}  // namespace veritas::wpa

#endif  // VERITAS_WPA_SCC_STATE_REPOSITORY_H_
