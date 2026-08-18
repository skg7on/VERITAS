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

#include "veritas/summary/ComponentHash.h"

#include <cstddef>
#include <span>
#include <string>

namespace veritas::summary {

namespace {

// Serialize a component to canonical bytes for hashing.
// Semantic hash: excludes provenance_ref and display fields.
// Evidence hash: includes provenance_ref and display fields.
std::string SerializeComponentSemantic(v1::ComponentKind kind,
                                       const v1::FunctionSummary& summary) {
  std::string canonical;

  switch (kind) {
    case v1::COMPONENT_KIND_CALLS: {
      for (const auto& call : summary.calls()) {
        canonical += call.callee_symbol();
        canonical += '\0';  // Delimiter to prevent ambiguous concatenation
        canonical += call.call_site_anchor_id();
        canonical += '\0';
        canonical += std::to_string(static_cast<int>(call.epistemic()));
        canonical += '\0';
      }
      break;
    }
    case v1::COMPONENT_KIND_RANGE_FACTS: {
      for (const auto& fact : summary.range_facts()) {
        canonical += fact.variable();
        canonical += '\0';
        canonical += std::to_string(fact.min_value());
        canonical += '\0';
        canonical += std::to_string(fact.max_value());
        canonical += '\0';
        canonical += std::to_string(static_cast<int>(fact.epistemic()));
        canonical += '\0';
      }
      break;
    }
    case v1::COMPONENT_KIND_MEMORY_EFFECTS: {
      for (const auto& effect : summary.memory_effects()) {
        canonical += std::to_string(static_cast<int>(effect.kind()));
        canonical += '\0';
        canonical += effect.location();
        canonical += '\0';
        canonical += std::to_string(effect.offset());
        canonical += '\0';
        canonical += std::to_string(effect.size());
        canonical += '\0';
        canonical += std::to_string(static_cast<int>(effect.epistemic()));
        canonical += '\0';
      }
      break;
    }
    case v1::COMPONENT_KIND_VALUE_FLOW: {
      for (const auto& flow : summary.value_flows()) {
        canonical += flow.source();
        canonical += '\0';
        canonical += flow.sink();
        canonical += '\0';
        canonical += std::to_string(static_cast<int>(flow.epistemic()));
        canonical += '\0';
      }
      break;
    }
    case v1::COMPONENT_KIND_CONTROL_FLOW: {
      for (const auto& cf : summary.control_flow()) {
        const auto& block = cf.block();
        canonical += block.basic_block_summary_id();
        canonical += '\0';
        canonical += block.function_variant_id();
        canonical += '\0';
        for (const auto& anchor : block.semantic_source_anchor_ids()) {
          canonical += anchor;
          canonical += '\0';
        }
        for (const auto& pred : block.predecessor_anchor_ids()) {
          canonical += pred;
          canonical += '\0';
        }
        for (const auto& succ : block.successor_anchor_ids()) {
          canonical += succ;
          canonical += '\0';
        }
        for (const auto& dom : cf.dominators()) {
          canonical += dom.dominator();
          canonical += '\0';
          canonical += dom.dominated();
          canonical += '\0';
          canonical += std::to_string(static_cast<int>(dom.epistemic()));
          canonical += '\0';
        }
      }
      break;
    }
    case v1::COMPONENT_KIND_ALIAS_FACTS: {
      for (const auto& alias : summary.alias_facts()) {
        canonical += alias.location_a();
        canonical += '\0';
        canonical += alias.location_b();
        canonical += '\0';
        canonical += std::to_string(static_cast<int>(alias.epistemic()));
        canonical += '\0';
      }
      break;
    }
    case v1::COMPONENT_KIND_TAINT: {
      for (const auto& taint : summary.taint_transfers()) {
        canonical += taint.source();
        canonical += '\0';
        canonical += taint.sink();
        canonical += '\0';
        canonical += taint.taint_kind();
        canonical += '\0';
        canonical += std::to_string(static_cast<int>(taint.epistemic()));
        canonical += '\0';
      }
      break;
    }
    case v1::COMPONENT_KIND_OWNERSHIP: {
      for (const auto& ownership : summary.ownership_effects()) {
        canonical += std::to_string(static_cast<int>(ownership.kind()));
        canonical += '\0';
        canonical += ownership.object();
        canonical += '\0';
        canonical += std::to_string(static_cast<int>(ownership.epistemic()));
        canonical += '\0';
      }
      break;
    }
    case v1::COMPONENT_KIND_LOCKS: {
      for (const auto& lock : summary.lock_effects()) {
        canonical += std::to_string(static_cast<int>(lock.kind()));
        canonical += '\0';
        canonical += lock.lock_object();
        canonical += '\0';
        canonical += std::to_string(static_cast<int>(lock.epistemic()));
        canonical += '\0';
      }
      break;
    }
    case v1::COMPONENT_KIND_STATE: {
      for (const auto& state : summary.state_transitions()) {
        canonical += state.from_state();
        canonical += '\0';
        canonical += state.to_state();
        canonical += '\0';
        canonical += state.event();
        canonical += '\0';
        canonical += std::to_string(static_cast<int>(state.epistemic()));
        canonical += '\0';
      }
      break;
    }
    case v1::COMPONENT_KIND_UNKNOWNS: {
      for (const auto& unknown : summary.unknowns()) {
        canonical += unknown.kind();
        canonical += '\0';
        canonical += unknown.reason();
        canonical += '\0';
        canonical += unknown.scope();
        canonical += '\0';
      }
      break;
    }
    case v1::COMPONENT_KIND_ASSUMPTIONS: {
      for (const auto& assumption : summary.assumptions()) {
        canonical += assumption.description();
        canonical += '\0';
        canonical += assumption.condition();
        canonical += '\0';
      }
      break;
    }
    case v1::COMPONENT_KIND_DEPENDENCIES: {
      for (const auto& dep : summary.dependencies()) {
        canonical += dep.symbol();
        canonical += '\0';
        canonical += dep.kind();
        canonical += '\0';
      }
      break;
    }
    case v1::COMPONENT_KIND_PROVENANCE: {
      // Provenance semantic hash is empty; only evidence hash matters
      break;
    }
    default:
      break;
  }

  return canonical;
}

std::string SerializeComponentEvidence(v1::ComponentKind kind,
                                       const v1::FunctionSummary& summary) {
  std::string canonical = SerializeComponentSemantic(kind, summary);

  // Add provenance and display fields for evidence hash
  switch (kind) {
    case v1::COMPONENT_KIND_CALLS: {
      for (const auto& call : summary.calls()) {
        canonical += call.provenance_ref();
      }
      break;
    }
    case v1::COMPONENT_KIND_RANGE_FACTS: {
      for (const auto& fact : summary.range_facts()) {
        canonical += fact.provenance_ref();
      }
      break;
    }
    case v1::COMPONENT_KIND_MEMORY_EFFECTS: {
      for (const auto& effect : summary.memory_effects()) {
        canonical += effect.provenance_ref();
      }
      break;
    }
    case v1::COMPONENT_KIND_VALUE_FLOW: {
      for (const auto& flow : summary.value_flows()) {
        canonical += flow.provenance_ref();
      }
      break;
    }
    case v1::COMPONENT_KIND_CONTROL_FLOW: {
      for (const auto& cf : summary.control_flow()) {
        for (const auto& dom : cf.dominators()) {
          canonical += dom.provenance_ref();
        }
      }
      break;
    }
    case v1::COMPONENT_KIND_ALIAS_FACTS: {
      for (const auto& alias : summary.alias_facts()) {
        canonical += alias.provenance_ref();
      }
      break;
    }
    case v1::COMPONENT_KIND_TAINT: {
      for (const auto& taint : summary.taint_transfers()) {
        canonical += taint.provenance_ref();
      }
      break;
    }
    case v1::COMPONENT_KIND_OWNERSHIP: {
      for (const auto& ownership : summary.ownership_effects()) {
        canonical += ownership.provenance_ref();
      }
      break;
    }
    case v1::COMPONENT_KIND_LOCKS: {
      for (const auto& lock : summary.lock_effects()) {
        canonical += lock.provenance_ref();
      }
      break;
    }
    case v1::COMPONENT_KIND_STATE: {
      for (const auto& state : summary.state_transitions()) {
        canonical += state.provenance_ref();
      }
      break;
    }
    case v1::COMPONENT_KIND_UNKNOWNS: {
      for (const auto& unknown : summary.unknowns()) {
        canonical += unknown.provenance_ref();
      }
      break;
    }
    case v1::COMPONENT_KIND_ASSUMPTIONS: {
      for (const auto& assumption : summary.assumptions()) {
        canonical += assumption.provenance_ref();
      }
      break;
    }
    case v1::COMPONENT_KIND_DEPENDENCIES: {
      for (const auto& dep : summary.dependencies()) {
        canonical += dep.provenance_ref();
      }
      break;
    }
    case v1::COMPONENT_KIND_PROVENANCE: {
      for (const auto& prov : summary.provenance_refs()) {
        canonical += prov.id();
        canonical += prov.file_path();
        canonical += std::to_string(prov.line());
        canonical += std::to_string(prov.column());
        canonical += prov.analysis_step();
        canonical += prov.display_text();
      }
      break;
    }
    default:
      break;
  }

  return canonical;
}

int32_t GetItemCount(v1::ComponentKind kind,
                     const v1::FunctionSummary& summary) {
  switch (kind) {
    case v1::COMPONENT_KIND_CALLS:
      return summary.calls_size();
    case v1::COMPONENT_KIND_MEMORY_EFFECTS:
      return summary.memory_effects_size();
    case v1::COMPONENT_KIND_VALUE_FLOW:
      return summary.value_flows_size();
    case v1::COMPONENT_KIND_CONTROL_FLOW:
      return summary.control_flow_size();
    case v1::COMPONENT_KIND_RANGE_FACTS:
      return summary.range_facts_size();
    case v1::COMPONENT_KIND_ALIAS_FACTS:
      return summary.alias_facts_size();
    case v1::COMPONENT_KIND_TAINT:
      return summary.taint_transfers_size();
    case v1::COMPONENT_KIND_OWNERSHIP:
      return summary.ownership_effects_size();
    case v1::COMPONENT_KIND_LOCKS:
      return summary.lock_effects_size();
    case v1::COMPONENT_KIND_STATE:
      return summary.state_transitions_size();
    case v1::COMPONENT_KIND_UNKNOWNS:
      return summary.unknowns_size();
    case v1::COMPONENT_KIND_ASSUMPTIONS:
      return summary.assumptions_size();
    case v1::COMPONENT_KIND_DEPENDENCIES:
      return summary.dependencies_size();
    case v1::COMPONENT_KIND_PROVENANCE:
      return summary.provenance_refs_size();
    default:
      return 0;
  }
}

}  // namespace

ComponentDigestInfo ComputeComponentDigest(v1::ComponentKind kind,
                                           const v1::FunctionSummary& summary) {
  std::string semantic_bytes = SerializeComponentSemantic(kind, summary);
  std::string evidence_bytes = SerializeComponentEvidence(kind, summary);

  auto semantic_span = std::as_bytes(std::span(semantic_bytes));
  auto evidence_span = std::as_bytes(std::span(evidence_bytes));

  ComponentDigestInfo info;
  info.kind = kind;
  info.semantic_hash = core::ComputeSHA256(semantic_span);
  info.evidence_hash = core::ComputeSHA256(evidence_span);
  info.item_count = GetItemCount(kind, summary);
  info.payload_offset = 0;  // Will be computed by serialization layer
  info.payload_length = static_cast<int64_t>(semantic_bytes.size());

  return info;
}

std::vector<ComponentDigestInfo> ComputeComponentDigests(
    const v1::FunctionSummary& summary) {
  std::vector<ComponentDigestInfo> digests;

  // Compute digests for all component kinds
  for (int i = v1::COMPONENT_KIND_CALLS; i <= v1::COMPONENT_KIND_PROVENANCE; ++i) {
    auto kind = static_cast<v1::ComponentKind>(i);
    digests.push_back(ComputeComponentDigest(kind, summary));
  }

  return digests;
}

}  // namespace veritas::summary
