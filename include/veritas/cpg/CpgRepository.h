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

#ifndef VERITAS_CPG_CPGREPOSITORY_H_
#define VERITAS_CPG_CPGREPOSITORY_H_

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/cpg/ThinCpg.h"

namespace veritas::summarydb {
class MetadataStore;
}  // namespace veritas::summarydb

namespace veritas::cpg {

// CpgRepository persists validated CPG projections and their current bindings
// in SQLite. It shares a MetadataStore with SummaryRepository so the publication
// coordinator can stage summaries and the graph in one transaction.
class CpgRepository {
 public:
  explicit CpgRepository(summarydb::MetadataStore& metadata_store);

  // Stage a validated projection within the current transaction. Assumes
  // BeginTransaction has already been called on the shared MetadataStore.
  Status StageProjection(const ThinCpg& graph);

  // Load a historical projection by its ProjectionID.
  StatusOr<ThinCpg> LoadProjection(const core::StableId& projection_id) const;

  // Resolve the current projection ID for (revision, build variant).
  StatusOr<core::StableId> OpenCurrent(
      const core::StableId& revision_id,
      const core::StableId& build_variant_id) const;

 private:
  summarydb::MetadataStore& metadata_store_;
};

}  // namespace veritas::cpg

#endif  // VERITAS_CPG_CPGREPOSITORY_H_
