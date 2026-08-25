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

#include "veritas/facts/RelationSchema.h"

#include <array>
#include <cstddef>

namespace veritas::facts {

namespace {

using RelationTable = std::array<RelationSchema, kRelationCountV2>;

const RelationTable& Table() {
  static const RelationTable table = {
      RelationSchema{"FunctionMap", RelationOwnership::kEdb,
                     {{"function_id", ColumnDomain::kFunctionId},
                      {"function_stable_id", ColumnDomain::kString}}},
      RelationSchema{"ValueMap", RelationOwnership::kEdb,
                     {{"value_id", ColumnDomain::kValueId},
                      {"value_stable_id", ColumnDomain::kString}}},
      RelationSchema{"MemoryMap", RelationOwnership::kEdb,
                     {{"memory_id", ColumnDomain::kMemoryId},
                      {"memory_stable_id", ColumnDomain::kString}}},
      RelationSchema{"CallSiteMap", RelationOwnership::kEdb,
                     {{"call_site_id", ColumnDomain::kCallSiteId},
                      {"call_site_stable_id", ColumnDomain::kString}}},
      RelationSchema{"FactMap", RelationOwnership::kEdb,
                     {{"fact_id", ColumnDomain::kFactId},
                      {"fact_stable_id", ColumnDomain::kString}}},
      RelationSchema{"DirectCall", RelationOwnership::kEdb,
                     {{"call_site_id", ColumnDomain::kCallSiteId},
                      {"caller_id", ColumnDomain::kFunctionId},
                      {"callee_id", ColumnDomain::kFunctionId},
                      {"dispatch", ColumnDomain::kDispatchKind},
                      {"epistemic", ColumnDomain::kEpistemic}}},
      RelationSchema{"UnknownCall", RelationOwnership::kEdb,
                     {{"call_site_id", ColumnDomain::kCallSiteId},
                      {"caller_id", ColumnDomain::kFunctionId},
                      {"reason", ColumnDomain::kString},
                      {"epistemic", ColumnDomain::kEpistemic}}},
      RelationSchema{"DirectRead", RelationOwnership::kEdb,
                     {{"function_id", ColumnDomain::kFunctionId},
                      {"memory_id", ColumnDomain::kMemoryId},
                      {"range_kind", ColumnDomain::kByteRangeKind},
                      {"offset", ColumnDomain::kInt64},
                      {"size", ColumnDomain::kUint64},
                      {"epistemic", ColumnDomain::kEpistemic}}},
      RelationSchema{"DirectWrite", RelationOwnership::kEdb,
                     {{"function_id", ColumnDomain::kFunctionId},
                      {"memory_id", ColumnDomain::kMemoryId},
                      {"range_kind", ColumnDomain::kByteRangeKind},
                      {"offset", ColumnDomain::kInt64},
                      {"size", ColumnDomain::kUint64},
                      {"epistemic", ColumnDomain::kEpistemic}}},
      RelationSchema{"Alias", RelationOwnership::kEdb,
                     {{"left_memory_id", ColumnDomain::kMemoryId},
                      {"right_memory_id", ColumnDomain::kMemoryId},
                      {"alias_kind", ColumnDomain::kAliasKind},
                      {"epistemic", ColumnDomain::kEpistemic}}},
      RelationSchema{"LocalFlow", RelationOwnership::kEdb,
                     {{"function_id", ColumnDomain::kFunctionId},
                      {"source_id", ColumnDomain::kValueId},
                      {"destination_id", ColumnDomain::kValueId},
                      {"flow_kind", ColumnDomain::kString},
                      {"epistemic", ColumnDomain::kEpistemic}}},
      RelationSchema{"ParameterFlow", RelationOwnership::kEdb,
                     {{"call_site_id", ColumnDomain::kCallSiteId},
                      {"actual_id", ColumnDomain::kValueId},
                      {"formal_id", ColumnDomain::kValueId},
                      {"epistemic", ColumnDomain::kEpistemic}}},
      RelationSchema{"ReturnFlow", RelationOwnership::kEdb,
                     {{"call_site_id", ColumnDomain::kCallSiteId},
                      {"return_id", ColumnDomain::kValueId},
                      {"result_id", ColumnDomain::kValueId},
                      {"epistemic", ColumnDomain::kEpistemic}}},
      RelationSchema{"ModeledEffect", RelationOwnership::kEdb,
                     {{"model_id", ColumnDomain::kModelId},
                      {"function_id", ColumnDomain::kFunctionId},
                      {"effect_kind", ColumnDomain::kString},
                      {"subject_id", ColumnDomain::kString},
                      {"epistemic", ColumnDomain::kEpistemic}}},
      RelationSchema{"UnsupportedFeature", RelationOwnership::kEdb,
                     {{"node_id", ColumnDomain::kString},
                      {"feature_kind", ColumnDomain::kString},
                      {"soundness_policy", ColumnDomain::kString}}},
      RelationSchema{"ReachableCall", RelationOwnership::kIdb,
                     {{"source_id", ColumnDomain::kFunctionId},
                      {"target_id", ColumnDomain::kFunctionId},
                      {"epistemic", ColumnDomain::kEpistemic}}},
      RelationSchema{"MayWrite", RelationOwnership::kIdb,
                     {{"function_id", ColumnDomain::kFunctionId},
                      {"memory_id", ColumnDomain::kMemoryId},
                      {"epistemic", ColumnDomain::kEpistemic}}},
      // Support relations carry successor-SCC results into a component. They
      // mirror their IDB counterpart's columns but are EDB: a component cites
      // them as inputs and never claims ownership of successor results.
      RelationSchema{"SupportReachableCall", RelationOwnership::kEdb,
                     {{"source_id", ColumnDomain::kFunctionId},
                      {"target_id", ColumnDomain::kFunctionId},
                      {"epistemic", ColumnDomain::kEpistemic}}},
      RelationSchema{"SupportMayWrite", RelationOwnership::kEdb,
                     {{"function_id", ColumnDomain::kFunctionId},
                      {"memory_id", ColumnDomain::kMemoryId},
                      {"epistemic", ColumnDomain::kEpistemic}}},
  };
  return table;
}

}  // namespace

const RelationSchema& RelationRegistry::Get(RelationId id) const {
  return Table()[static_cast<std::size_t>(id)];
}

const RelationRegistry& RelationsV2() {
  static const RelationRegistry registry;
  return registry;
}

}  // namespace veritas::facts
