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

#include "SvfFactMapper.h"

#include <algorithm>
#include <optional>
#include <unordered_map>

#include <SVF-LLVM/LLVMModule.h>
#include <SVFIR/SVFIR.h>
#include <WPA/Andersen.h>
#include <Graphs/SVFG.h>

#include "analysis/pipeline/ProgramIr.h"

namespace veritas::analysis::svf {
namespace {

// Resolve an SVF value to a VERITAS ValueRef through LLVM and M4 origin maps.
//
// Uses the session-scoped LLVMModuleSet carried by `view` rather than the
// process-wide singleton. This keeps value resolution tied to the live SVF
// session and prevents mapping SVF values against stale module state from a
// previous session.
std::optional<summary::ValueRef> ResolveValue(
    const SVF::SVFValue* svf_value,
    const SvfSessionView& view,
    const pipeline::ProgramIr& program_ir) {
  if (!svf_value || !view.module_set) {
    return std::nullopt;
  }

  if (!view.module_set->hasLLVMValue(svf_value)) {
    return std::nullopt;
  }

  const ::llvm::Value* llvm_value = view.module_set->getLLVMValue(svf_value);
  if (!llvm_value) {
    return std::nullopt;
  }

  // In real implementation, would query program_ir.origin_map()
  // For now, use LLVM value name as placeholder
  std::string name;
  if (llvm_value->hasName()) {
    name = llvm_value->getName().str();
  } else {
    name = "<unnamed>";
  }

  return summary::ValueRef{name};
}

// Create provenance string for a mapped fact
std::string MakeProvenance(const AnalyzerRunContext& run_context,
                           const SvfConfig& config) {
  return "analyzer=" + run_context.analyzer_run_id +
         ";config=" + config.CanonicalAnalyzerConfig();
}

// Collect SVFG edges in deterministic order
std::vector<const SVF::SVFGEdge*> CollectSvfgEdges(const SVF::SVFG& svfg) {
  std::vector<const SVF::SVFGEdge*> edges;

  // Iterate through all SVFG nodes and collect their outgoing edges
  for (auto it = svfg.begin(); it != svfg.end(); ++it) {
    const SVF::SVFGNode* node = it->second;
    for (auto edge_it = node->OutEdgeBegin();
         edge_it != node->OutEdgeEnd();
         ++edge_it) {
      edges.push_back(*edge_it);
    }
  }

  return edges;
}

// Get the SVF value for an SVFG node
const SVF::SVFValue* SvfValueForNode(const SVF::SVFGNode* node) {
  if (!node) {
    return nullptr;
  }
  // In real implementation, would extract the SVFValue from the node
  // This is a placeholder that returns nullptr
  return nullptr;
}

}  // namespace

Status MapSvfFacts(const pipeline::ProgramIr& program_ir,
                   const SvfSessionView& view,
                   const AnalyzerRunContext& run_context,
                   const SvfConfig& config,
                   SvfMappingResult* result) {
  if (!result) {
    return Status::Internal("result is null");
  }
  if (!view.svf_ir || !view.andersen || !view.svfg) {
    return Status::Internal("incomplete SVF session view");
  }

  SvfFacts facts;
  const std::string provenance = MakeProvenance(run_context, config);
  int unmapped_count = 0;

  // Step 1: Map value-flow edges from SVFG
  auto edges = CollectSvfgEdges(*view.svfg);
  for (const auto* edge : edges) {
    if (!edge) continue;

    auto source = ResolveValue(
        SvfValueForNode(edge->getSrcNode()), view, program_ir);
    auto dest = ResolveValue(
        SvfValueForNode(edge->getDstNode()), view, program_ir);

    if (!source || !dest) {
      ++unmapped_count;
      facts.unknowns.push_back(summary::UnknownFact{
          .scope = "value_flow_edge",
          .reason = "unmapped_svf_node",
          .provenance = provenance,
      });
      continue;
    }

    facts.value_flows.push_back(summary::ValueFlowFact{
        .source = *source,
        .destination = *dest,
        .provenance = provenance,
    });
  }

  // Step 2: Query alias relationships for pointer pairs
  // In real implementation, would build candidate pairs from value flows
  // For now, just demonstrate the structure
  std::vector<std::pair<summary::MemoryRef, summary::MemoryRef>> alias_pairs;

  for (const auto& [left, right] : alias_pairs) {
    // Query SVF Andersen analysis
    // In real implementation: view.andersen->alias(left_svf, right_svf)
    std::string relationship = "MAY_ALIAS";

    facts.aliases.push_back(summary::AliasFact{
        .left = left,
        .right = right,
        .relationship = relationship,
        .provenance = provenance,
    });
  }

  // Step 3: Sort and deduplicate all facts
  auto canonicalize = [](auto& fact_vec) {
    std::ranges::sort(fact_vec);
    auto [first, last] = std::ranges::unique(fact_vec);
    fact_vec.erase(first, last);
  };

  canonicalize(facts.value_flows);
  canonicalize(facts.aliases);
  canonicalize(facts.refined_memory_effects);
  canonicalize(facts.refined_calls);
  canonicalize(facts.unknowns);
  canonicalize(facts.dependencies);

  // Determine completion status
  auto completion = unmapped_count > 0
      ? SvfMappingCompletion::kCompleteWithUnknowns
      : SvfMappingCompletion::kComplete;

  *result = SvfMappingResult{
      .completion = completion,
      .facts = std::move(facts),
  };

  return Status::Ok();
}

}  // namespace veritas::analysis::svf
