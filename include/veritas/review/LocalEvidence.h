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

#ifndef VERITAS_REVIEW_LOCAL_EVIDENCE_H_
#define VERITAS_REVIEW_LOCAL_EVIDENCE_H_
#include "veritas/review/StructuralIndex.h"
namespace veritas::review {
struct LocalRequest {
  // Native capability resolved by the backend, never supplied on the wire.
  const StructuralIndex &index;
  ReviewBinding binding;
  std::string predicate_kind;
  EntityRef callsite_ref;
  std::vector<std::string> requested_slots;
  uint64_t max_examined;
};
struct CopyOperands {
  EntityRef destination, source, length;
};
struct ByteExtent {
  uint64_t bytes;
};
struct LocalGuard {
  EntityRef condition_ref;
  bool dominates;
  std::vector<EntityRef> operand_refs;
};
struct LocalObservation {
  std::string slot;
  EntityRef subject_ref;
  std::variant<CopyOperands, ByteExtent, LocalGuard> value;
  std::vector<ContextId> source_refs;
};
struct LocalEvidence {
  LocalEvidenceId local_evidence_id;
  ReviewBinding binding;
  std::string predicate_kind;
  EntityRef callsite_ref;
  std::vector<LocalObservation> observations;
  std::vector<std::string> assumptions;
  Coverage coverage;
  ProviderVersion producer;
  Usage usage;
};
// Rejects stale/foreign/non-callsite refs, unregistered predicates and invalid
// budgets/slots. Unsupported slots remain absent with a coverage reason.
StatusOr<LocalEvidence> InvestigateLocal(const LocalRequest &);
Json ToJson(const LocalEvidence &);
} // namespace veritas::review
#endif
