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
#include <string>
#include <vector>

#include "analysis/svf/SvfBudget.h"

#include <SVF-LLVM/LLVMModule.h>
#include <SVFIR/SVFIR.h>
#include <WPA/Andersen.h>
#include <Graphs/SVFG.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Value.h>

#include "analysis/pipeline/ProgramIr.h"

namespace veritas::analysis::svf {
namespace {

// A VERITAS name for a resolved LLVM value. The name is a deterministic,
// human-readable proxy; the M4 origin map supplies true stable ValueRef
// identity for arguments and named values when present.
std::string LlvmValueName(const ::llvm::Value* value) {
  if (value->hasName()) return value->getName().str();
  if (const auto* arg = ::llvm::dyn_cast<::llvm::Argument>(value)) {
    return "arg" + std::to_string(arg->getArgNo());
  }
  if (::llvm::isa<::llvm::Instruction>(value)) {
    return "tmp";
  }
  return "value";
}

// Resolve an SVF value to a VERITAS ValueRef through the session-scoped LLVM
// module set. Returns nullopt for SVF values with no underlying LLVM value.
std::optional<summary::ValueRef> ResolveValue(const SVF::SVFValue* svf_value,
                                              const SvfSessionView& view) {
  if (!svf_value || !view.module_set) return std::nullopt;
  if (!view.module_set->hasLLVMValue(svf_value)) return std::nullopt;
  const ::llvm::Value* llvm_value = view.module_set->getLLVMValue(svf_value);
  if (!llvm_value) return std::nullopt;
  return summary::ValueRef{LlvmValueName(llvm_value)};
}

// The SVFVar backing an SVFG node, or nullptr for nodes without a direct
// value (e.g. pure memory-region nodes).
const SVF::SVFVar* SvfValueForNode(const SVF::SVFGNode* node) {
  if (!node) return nullptr;
  return node->getValue();
}

// True when the SVFVar resolves to an LLVM value of pointer type. Only pointer
// pairs are alias candidates.
bool IsPointer(const SVF::SVFVar* var, const SvfSessionView& view) {
  if (!var || !view.module_set) return false;
  if (!view.module_set->hasLLVMValue(var)) return false;
  const ::llvm::Value* llvm_value = view.module_set->getLLVMValue(var);
  return llvm_value && llvm_value->getType()->isPointerTy();
}

std::string AliasRelationship(SVF::AliasResult result) {
  switch (result) {
    case SVF::MustAlias:
      return "MUST_ALIAS";
    case SVF::NoAlias:
      return "NO_ALIAS";
    case SVF::MayAlias:
      return "MAY_ALIAS";
    case SVF::PartialAlias:
      return "MAY_ALIAS";
  }
  return "UNKNOWN_ALIAS";
}

std::string MakeProvenance(const AnalyzerRunContext& run_context,
                           const SvfConfig& config) {
  return "analyzer=" + run_context.analyzer_run_id +
         ";config=" + config.CanonicalAnalyzerConfig();
}

// Collect every SVFG edge once, in deterministic node order.
std::vector<const SVF::SVFGEdge*> CollectSvfgEdges(const SVF::SVFG& svfg) {
  std::vector<const SVF::SVFGEdge*> edges;
  for (auto it = svfg.begin(); it != svfg.end(); ++it) {
    const SVF::SVFGNode* node = it->second;
    for (auto edge_it = node->OutEdgeBegin(); edge_it != node->OutEdgeEnd();
         ++edge_it) {
      edges.push_back(*edge_it);
    }
  }
  return edges;
}

// An alias candidate retains the callback-scoped SVFVar pointers beside the
// resolved VERITAS MemoryRefs so MapAliasResult can query Andersen without
// re-resolving.
struct AliasCandidate {
  const SVF::SVFVar* left_var;
  const SVF::SVFVar* right_var;
  summary::MemoryRef left;
  summary::MemoryRef right;
};

}  // namespace

Status MapSvfFacts(const pipeline::ProgramIr& program_ir,
                   const SvfSessionView& view,
                   const AnalyzerRunContext& run_context,
                   const SvfConfig& config,
                   SvfMappingResult* result) {
  if (!result) return Status::Internal("result is null");
  if (!view.svf_ir || !view.andersen || !view.svfg) {
    return Status::Internal("incomplete SVF session view");
  }

  SvfFacts facts;
  std::vector<AliasCandidate> alias_candidates;
  const std::string provenance = MakeProvenance(run_context, config);
  int unmapped_count = 0;
  SvfBudget budget(config);
  bool truncated = false;

  for (const auto* edge : CollectSvfgEdges(*view.svfg)) {
    if (!edge) continue;

    const SVF::SVFVar* source_var = SvfValueForNode(edge->getSrcNode());
    const SVF::SVFVar* dest_var = SvfValueForNode(edge->getDstNode());
    auto source = ResolveValue(source_var, view);
    auto dest = ResolveValue(dest_var, view);

    if (!source || !dest) {
      ++unmapped_count;
      facts.unknowns.push_back(summary::UnknownFact{
          .scope = "value_flow_edge",
          .reason = "unmapped_svf_node",
          .provenance = provenance,
      });
      continue;
    }

    if (!budget.TryEmit()) {
      truncated = true;
      break;
    }
    facts.value_flows.push_back(summary::ValueFlowFact{
        .source = *source,
        .destination = *dest,
        .provenance = provenance,
    });

    if (IsPointer(source_var, view) && IsPointer(dest_var, view)) {
      alias_candidates.push_back(AliasCandidate{
          source_var, dest_var, summary::MemoryRef{source->name},
          summary::MemoryRef{dest->name}});
    }
  }

  // Query Andersen for each pointer candidate pair.
  for (const auto& candidate : alias_candidates) {
    if (!budget.TryEmit()) {
      truncated = true;
      break;
    }
    facts.aliases.push_back(summary::AliasFact{
        .left = candidate.left,
        .right = candidate.right,
        .relationship = AliasRelationship(
            view.andersen->alias(candidate.left_var, candidate.right_var)),
        .provenance = provenance,
    });
  }

  if (truncated) {
    facts.unknowns.push_back(summary::UnknownFact{
        .scope = "analysis_truncated",
        .reason = BudgetReasonName(budget.state().reason),
        .provenance = provenance,
    });
  }

  auto canonicalize = [](auto& fact_vec) {
    std::sort(fact_vec.begin(), fact_vec.end());
    fact_vec.erase(std::unique(fact_vec.begin(), fact_vec.end()),
                   fact_vec.end());
  };
  canonicalize(facts.value_flows);
  canonicalize(facts.aliases);
  canonicalize(facts.refined_memory_effects);
  canonicalize(facts.refined_calls);
  canonicalize(facts.unknowns);
  canonicalize(facts.dependencies);

  const auto completion = (unmapped_count > 0 || truncated)
                              ? SvfMappingCompletion::kCompleteWithUnknowns
                              : SvfMappingCompletion::kComplete;
  *result = SvfMappingResult{.completion = completion, .facts = std::move(facts)};
  return Status::Ok();
}

}  // namespace veritas::analysis::svf
