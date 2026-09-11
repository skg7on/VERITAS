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

// QueryCompletion.h — the M9-backed evidence.query_completion.v1 fact.
//
// Every complete or truncated query result carries a completion certificate: a
// canonical M9 AnalysisFact whose semantic cells record exactly what the query
// was asked, how it was bounded, and what it returned. The fact is published
// through M9 (MakeFact), so its identity is witness-independent and its run
// binding and selected witness resolve in the provenance closure.

#ifndef VERITAS_EVIDENCE_QUERY_COMPLETION_H_
#define VERITAS_EVIDENCE_QUERY_COMPLETION_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/evidence/SliceTypes.h"
#include "veritas/facts/AnalysisFact.h"
#include "veritas/facts/FactStore.h"
#include "veritas/facts/ProvenanceStore.h"

namespace veritas::evidence {

// The nine canonical cells of evidence.query_completion.v1 (test-contract
// section 4.1). ordered_scope_refs and ordered_truncation_reasons are ordered
// lists whose order is semantically significant and therefore part of the fact
// identity.
struct QueryCompletionDescriptor {
  std::string query_kind;
  std::vector<core::StableId> ordered_scope_refs;
  EvidenceQueryBudget budget;
  std::string query_implementation_version;
  std::string input_snapshot_fingerprint;
  QueryCompleteness completeness;
  std::vector<TruncationReason> ordered_truncation_reasons;
  std::size_t examined_items = 0;
  std::string returned_member_digest;
};

// Builds the canonical evidence.query_completion.v1 fact row and derives its
// witness-independent fact ID through M9 MakeFact. Returns InvalidArgument when
// the descriptor is not a canonical completion certificate (for example, an
// unspecified completeness or a budget with a non-positive limit).
StatusOr<facts::AnalysisFact> MakeQueryCompletionFact(
    const QueryCompletionDescriptor& descriptor);

// Validates a completion fact, its run binding, and its selected witness
// against the result metadata and the canonical payload it certifies. Rejects a
// fact whose cells do not re-derive to the same fact ID, a binding or witness
// bound to a different run or fact, a non-selected witness, a producer
// mismatch, or metadata that disagrees with the descriptor's completeness,
// reason order, or examined count. The witness is never synthesized here: a
// missing witness is the caller's responsibility to supply.
Status ValidateQueryCompletion(const facts::AnalysisFact& completion_fact,
                               const facts::RunFactBinding& binding,
                               const facts::FactWitness& witness,
                               const QueryResultMetadata& metadata,
                               const QueryCompletionDescriptor& descriptor);

}  // namespace veritas::evidence

#endif  // VERITAS_EVIDENCE_QUERY_COMPLETION_H_
