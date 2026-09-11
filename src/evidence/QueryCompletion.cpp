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

#include "veritas/evidence/QueryCompletion.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "veritas/facts/RelationSchema.h"

namespace veritas::evidence {

namespace {

// The nine canonical cells of evidence.query_completion.v1. Lists are carried
// as comma-joined canonical text; the separator never collides with the stable
// ID spelling (<kind>:sha256:<hex>) or the enum spellings above.
std::string EncodeScopeRefs(const std::vector<core::StableId>& refs) {
  std::string out;
  for (std::size_t i = 0; i < refs.size(); ++i) {
    if (i != 0) out.push_back(',');
    out += core::ToString(refs[i]);
  }
  return out;
}

std::string EncodeReasons(const std::vector<TruncationReason>& reasons) {
  std::string out;
  for (std::size_t i = 0; i < reasons.size(); ++i) {
    if (i != 0) out.push_back(',');
    out += ToString(reasons[i]);
  }
  return out;
}

std::string EncodeBudget(const EvidenceQueryBudget& budget) {
  std::string out;
  out += std::to_string(budget.max_depth);
  out.push_back(',');
  out += std::to_string(budget.max_nodes);
  out.push_back(',');
  out += std::to_string(budget.max_paths);
  out.push_back(',');
  out += std::to_string(budget.max_facts_per_query);
  out.push_back(',');
  out += std::to_string(budget.max_provenance_depth);
  return out;
}

bool HasDuplicate(const std::vector<TruncationReason>& reasons) {
  for (std::size_t i = 0; i < reasons.size(); ++i) {
    for (std::size_t j = i + 1; j < reasons.size(); ++j) {
      if (reasons[i] == reasons[j]) return true;
    }
  }
  return false;
}

Status ValidateDescriptor(const QueryCompletionDescriptor& descriptor) {
  if (descriptor.completeness == QueryCompleteness::kUnspecified)
    return Status::InvalidArgument("completion certificate is unspecified");
  if (Status s = ValidateEvidenceQueryBudget(descriptor.budget); !s.ok())
    return s;
  if (descriptor.completeness == QueryCompleteness::kComplete &&
      !descriptor.ordered_truncation_reasons.empty())
    return Status::InvalidArgument("complete certificate carries reasons");
  if (descriptor.completeness == QueryCompleteness::kTruncated &&
      descriptor.ordered_truncation_reasons.empty())
    return Status::InvalidArgument("truncated certificate carries no reason");
  for (const TruncationReason reason : descriptor.ordered_truncation_reasons) {
    if (reason == TruncationReason::kUnspecified)
      return Status::InvalidArgument("unspecified truncation reason");
  }
  if (HasDuplicate(descriptor.ordered_truncation_reasons))
    return Status::InvalidArgument("duplicate truncation reason");
  return Status::Ok();
}

}  // namespace

StatusOr<facts::AnalysisFact> MakeQueryCompletionFact(
    const QueryCompletionDescriptor& descriptor) {
  if (Status s = ValidateDescriptor(descriptor); !s.ok()) return s;

  facts::SemanticRow row;
  row.relation = facts::RelationId::kQueryCompletion;
  row.cells = {
      descriptor.query_kind,
      EncodeScopeRefs(descriptor.ordered_scope_refs),
      EncodeBudget(descriptor.budget),
      descriptor.query_implementation_version,
      descriptor.input_snapshot_fingerprint,
      std::string(ToString(descriptor.completeness)),
      EncodeReasons(descriptor.ordered_truncation_reasons),
      static_cast<std::uint64_t>(descriptor.examined_items),
      descriptor.returned_member_digest,
  };
  return facts::MakeFact(row);
}

Status ValidateQueryCompletion(const facts::AnalysisFact& completion_fact,
                               const facts::RunFactBinding& binding,
                               const facts::FactWitness& witness,
                               const QueryResultMetadata& metadata,
                               const QueryCompletionDescriptor& descriptor) {
  if (completion_fact.row.relation != facts::RelationId::kQueryCompletion)
    return Status::InvalidArgument("fact is not a query completion fact");
  if (Status s = facts::ValidateSemanticRow(completion_fact.row); !s.ok())
    return s;

  if (completion_fact.fact_id != metadata.query_provenance_id)
    return Status::InvalidArgument(
        "metadata query provenance does not name the completion fact");

  // The descriptor is the canonical payload; the fact must re-derive to the
  // same witness-independent ID or one of its cells disagrees.
  auto expected = MakeQueryCompletionFact(descriptor);
  if (!expected.ok()) return expected.status();
  if (expected->fact_id != completion_fact.fact_id)
    return Status::InvalidArgument(
        "completion fact does not match its descriptor");

  if (metadata.completeness != descriptor.completeness)
    return Status::InvalidArgument("metadata completeness disagrees with fact");
  if (metadata.truncation_reasons != descriptor.ordered_truncation_reasons)
    return Status::InvalidArgument("metadata reason order disagrees with fact");
  if (metadata.examined_items != descriptor.examined_items)
    return Status::InvalidArgument("metadata examined count disagrees with fact");

  if (binding.run_id != metadata.analysis_run_id)
    return Status::InvalidArgument("binding run does not match metadata");
  if (binding.fact_id != completion_fact.fact_id)
    return Status::InvalidArgument("binding fact does not match completion fact");

  if (witness.output_fact_id != completion_fact.fact_id)
    return Status::InvalidArgument("witness output fact does not match");
  if (witness.run_id != binding.run_id)
    return Status::InvalidArgument("witness run does not match binding");
  if (!witness.selected)
    return Status::InvalidArgument("witness is not the selected witness");
  if (witness.producer_kind != binding.producer_kind)
    return Status::InvalidArgument("witness producer does not match binding");

  return Status::Ok();
}

}  // namespace veritas::evidence
