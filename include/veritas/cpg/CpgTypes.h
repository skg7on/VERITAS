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

#ifndef VERITAS_CPG_CPGTYPES_H_
#define VERITAS_CPG_CPGTYPES_H_

#include <optional>
#include <string>
#include <vector>

#include "veritas/core/Ids.h"

namespace veritas::cpg {

// NodeKind is the V1 persistent CPG node allowlist.
enum class NodeKind {
  kFunction,
  kParameter,
  kGlobal,
  kCallSite,
  kMemoryObject,
  kBasicBlockSummary,
  kSummary,
  kUnknown,
};

// EdgeKind is the V1 persistent CPG edge allowlist.
enum class EdgeKind {
  kContains,
  kDeclares,
  kCalls,
  kMayCall,
  kReads,
  kWrites,
  kFlowsTo,
  kAliases,
  kDominatesSummary,
  kSummarizedBy,
  kUnknownAt,
};

// AliasState is the exact alias classification on an ALIASES edge.
enum class AliasState {
  kMustAlias,
  kMayAlias,
  kNoAlias,
  kUnknownAlias,
};

// SupportRef identifies the summary fact that produced a semantic edge: the
// originating FunctionSummaryID plus the opaque provenance reference carried by
// the fact.
struct SupportRef {
  core::StableId function_summary_id;
  std::string provenance_ref;

  auto operator<=>(const SupportRef&) const = default;
};

// CpgNode is a persistent graph node. node_id is a VERITAS stable ID; label is
// a human-readable name (e.g. a function name) and never drives identity.
struct CpgNode {
  core::StableId node_id;
  NodeKind kind;
  std::string label;
};

// CpgEdge is a persistent graph edge. alias_state is present only on kAliases
// edges. expandable marks summary edges whose targets can be traversed into a
// deeper summary. support carries the sorted, deduplicated provenance records.
struct CpgEdge {
  core::StableId edge_id;
  EdgeKind kind;
  core::StableId source_node_id;
  core::StableId target_node_id;
  std::optional<AliasState> alias_state;
  bool expandable = false;
  std::vector<SupportRef> support;
};

// ProjectionMetadata carries the revision/build/module/summary context that the
// ProjectionID and canonical graph bytes are derived from.
struct ProjectionMetadata {
  std::string schema_version = "veritas.cpg.v1";
  core::StableId revision_id;
  core::StableId build_variant_id;
  std::string module_hash;
  std::vector<core::StableId> summary_ids;
};

}  // namespace veritas::cpg

#endif  // VERITAS_CPG_CPGTYPES_H_
