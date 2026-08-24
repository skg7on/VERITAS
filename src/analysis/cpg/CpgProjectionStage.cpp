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

#include "analysis/cpg/CpgProjectionStage.h"

#include <algorithm>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>

#include "veritas/core/Hash.h"
#include "veritas/summary/FunctionSummary.h"

namespace veritas::analysis::cpg {
namespace {

namespace v1 = veritas::summary::v1;
namespace v2 = veritas::summary::v2;
using ::veritas::cpg::AliasState;
using ::veritas::cpg::CpgEdge;
using ::veritas::cpg::CpgNode;
using ::veritas::cpg::EdgeKind;
using ::veritas::cpg::NodeKind;
using ::veritas::cpg::SupportRef;
using ::veritas::cpg::ThinCpg;

core::StableId MakeId(core::IdKind kind, std::string_view value) {
  return core::MakeStableId(
      kind, std::as_bytes(std::span(value.data(), value.size())));
}

// Parse a stable-ID string and reject it unless it is a well-formed ID of the
// expected kind. Used for V2 structured memory/value identities so the CPG
// never re-derives an identity from a diagnostic string.
StatusOr<core::StableId> ParseIdOfKind(std::string_view text,
                                       core::IdKind expected) {
  auto parsed = core::ParseStableId(text);
  if (!parsed.ok()) {
    return parsed.status();
  }
  if (!core::HexToDigest(parsed->digest_hex).has_value()) {
    return Status::InvalidArgument("invalid CPG identity: invalid SHA-256");
  }
  if (parsed->kind != expected) {
    return Status::InvalidArgument("invalid CPG identity: wrong ID kind");
  }
  return *parsed;
}

// The V2 alias kind is a semantic AliasKind; map it directly to the CPG alias
// state rather than re-deriving it from epistemic certainty.
AliasState ToAliasState(v2::AliasKind kind) {
  switch (kind) {
  case v2::ALIAS_KIND_MUST_ALIAS:
    return AliasState::kMustAlias;
  case v2::ALIAS_KIND_MAY_ALIAS:
    return AliasState::kMayAlias;
  case v2::ALIAS_KIND_NO_ALIAS:
    return AliasState::kNoAlias;
  case v2::ALIAS_KIND_UNKNOWN_ALIAS:
    return AliasState::kUnknownAlias;
  default:
    return AliasState::kUnknownAlias;
  }
}

// A deterministic edge ID derived from the edge's semantic content.
core::StableId EdgeId(EdgeKind kind, const core::StableId &source,
                      const core::StableId &target,
                      std::optional<AliasState> alias_state,
                      const std::vector<SupportRef> &support) {
  std::string content = std::to_string(static_cast<int>(kind)) +
                        core::ToString(source) + core::ToString(target);
  if (alias_state.has_value()) {
    content += std::to_string(static_cast<int>(*alias_state));
  }
  for (const auto &record : support) {
    content +=
        core::ToString(record.function_summary_id) + record.provenance_ref;
  }
  return MakeId(core::IdKind::kCpgEdge, content);
}

Status AddSemanticEdge(ThinCpg *graph, EdgeKind kind,
                       const core::StableId &source,
                       const core::StableId &target,
                       std::optional<AliasState> alias_state,
                       std::vector<SupportRef> support) {
  CpgEdge edge;
  edge.edge_id = EdgeId(kind, source, target, alias_state, support);
  edge.kind = kind;
  edge.source_node_id = source;
  edge.target_node_id = target;
  edge.alias_state = alias_state;
  edge.expandable = (kind == EdgeKind::kSummarizedBy);
  edge.support = std::move(support);
  return graph->AddEdge(std::move(edge));
}

} // namespace

StatusOr<ThinCpg> BuildThinCpg(const CpgProjectionInput &input) {
  ThinCpg graph;

  ::llvm::Module *module = input.program_ir.GetModule();
  if (!module) {
    return Status::FailedPrecondition("ProgramIr has no module");
  }

  // 1. Function nodes and a mangled-name -> node-id lookup.
  std::map<std::string, core::StableId> function_ids;
  const auto &origin_map = input.program_ir.origin_map();
  for (const auto &function : *module) {
    if (function.isDeclaration())
      continue;
    auto origin_id = origin_map.GetSymbolId(&function);
    if (!origin_id.has_value()) {
      return Status::FailedPrecondition(
          "CPG function is missing its OriginMap identity");
    }
    auto node_id = core::ParseStableId(*origin_id);
    if (!node_id.ok()) {
      return Status::InvalidArgument("invalid OriginMap function identity: " +
                                     std::string(node_id.status().message()));
    }
    if (!core::HexToDigest(node_id->digest_hex).has_value()) {
      return Status::InvalidArgument(
          "invalid OriginMap function identity: invalid SHA-256 digest");
    }
    if (node_id->kind != core::IdKind::kFunctionVariant) {
      return Status::InvalidArgument(
          "OriginMap function identity is not a function-variant ID");
    }
    function_ids.emplace(*origin_id, *node_id);
    auto status = graph.AddNode(
        CpgNode{*node_id, NodeKind::kFunction, std::move(*origin_id)});
    if (!status.ok())
      return status;
  }

  // 2. Summary nodes and semantic edges from completed summary.v2 facts.
  std::vector<core::StableId> summary_ids;
  summary_ids.reserve(input.completed_summaries.size());
  for (const auto &summary : input.completed_summaries) {
    auto summary_id_result = summary::ComputeFunctionSummaryId(summary);
    if (!summary_id_result.ok())
      return summary_id_result.status();
    const core::StableId summary_id = *summary_id_result;
    summary_ids.push_back(summary_id);

    const std::string &fn = summary.identity().function_variant_id();
    auto add_summary_node =
        graph.AddNode(CpgNode{summary_id, NodeKind::kSummary, fn});
    if (!add_summary_node.ok())
      return add_summary_node;

    // Attribute semantic edges to the owning function; if the summary's
    // function is not in this module, keep the Summary node but emit no edges.
    auto fn_it = function_ids.find(fn);
    if (fn_it == function_ids.end()) {
      continue;
    }

    // SUMMARIZED_BY: function -> its summary.
    {
      auto status = AddSemanticEdge(&graph, EdgeKind::kSummarizedBy,
                                    fn_it->second, summary_id, std::nullopt,
                                    {SupportRef{summary_id, ""}});
      if (!status.ok())
        return status;
    }

    // CALLS / MAY_CALL.
    for (const auto &call : summary.calls()) {
      auto callee_it = function_ids.end();
      if (!call.resolved_callee_function_variant_id().empty()) {
        auto callee_id =
            core::ParseStableId(call.resolved_callee_function_variant_id());
        if (!callee_id.ok() ||
            !core::HexToDigest(callee_id->digest_hex).has_value() ||
            callee_id->kind != core::IdKind::kFunctionVariant) {
          return Status::InvalidArgument(
              "invalid resolved CPG callee function-variant identity");
        }
        callee_it =
            function_ids.find(call.resolved_callee_function_variant_id());
      }
      if (callee_it != function_ids.end()) {
        const EdgeKind kind = call.epistemic() == v1::EPISTEMIC_STATE_MUST
                                  ? EdgeKind::kCalls
                                  : EdgeKind::kMayCall;
        auto status = AddSemanticEdge(
            &graph, kind, fn_it->second, callee_it->second, std::nullopt,
            {SupportRef{summary_id, call.provenance_ref()}});
        if (!status.ok())
          return status;
      } else {
        // Unknown callee -> bounded Unknown node, never whole-program fanout.
        core::StableId unknown =
            MakeId(core::IdKind::kUnknownNode, call.callee_symbol());
        auto add_unknown = graph.AddNode(
            CpgNode{unknown, NodeKind::kUnknown, call.callee_symbol()});
        if (!add_unknown.ok())
          return add_unknown;
        auto status = AddSemanticEdge(
            &graph, EdgeKind::kMayCall, fn_it->second, unknown, std::nullopt,
            {SupportRef{summary_id, call.provenance_ref()}});
        if (!status.ok())
          return status;
      }
    }

    // READS / WRITES. V2 memory effects carry a structured MemoryLocation
    // whose memory_location_id is the durable kMemoryRef identity; the CPG
    // uses that identity directly instead of re-deriving one from a string.
    for (const auto &effect : summary.memory_effects()) {
      auto memory = ParseIdOfKind(effect.location().memory_location_id(),
                                  core::IdKind::kMemoryRef);
      if (!memory.ok())
        return memory.status();
      const std::string &diagnostic = effect.location().object().diagnostic_name();
      const std::string label =
          diagnostic.empty() ? effect.location().memory_location_id()
                             : diagnostic;
      auto add_memory = graph.AddNode(
          CpgNode{*memory, NodeKind::kMemoryObject, label});
      if (!add_memory.ok())
        return add_memory;
      const EdgeKind kind = effect.kind() == v1::EFFECT_KIND_WRITE
                                ? EdgeKind::kWrites
                                : EdgeKind::kReads;
      auto status =
          AddSemanticEdge(&graph, kind, fn_it->second, *memory, std::nullopt,
                          {SupportRef{summary_id, effect.provenance_ref()}});
      if (!status.ok())
        return status;
    }

    // FLOWS_TO. V2 value flows carry stable kValueRef identities.
    for (const auto &flow : summary.value_flows()) {
      auto source = ParseIdOfKind(flow.source_value_id(), core::IdKind::kValueRef);
      auto sink =
          ParseIdOfKind(flow.destination_value_id(), core::IdKind::kValueRef);
      if (!source.ok())
        return source.status();
      if (!sink.ok())
        return sink.status();
      auto add_src = graph.AddNode(
          CpgNode{*source, NodeKind::kParameter, flow.source_value_id()});
      auto add_sink = graph.AddNode(
          CpgNode{*sink, NodeKind::kParameter, flow.destination_value_id()});
      if (!add_src.ok())
        return add_src;
      if (!add_sink.ok())
        return add_sink;
      auto status = AddSemanticEdge(
          &graph, EdgeKind::kFlowsTo, *source, *sink, std::nullopt,
          {SupportRef{summary_id, flow.provenance_ref()}});
      if (!status.ok())
        return status;
    }

    // ALIASES. V2 alias facts carry structured locations and a semantic
    // AliasKind, both projected directly without reverting to strings.
    for (const auto &alias : summary.alias_facts()) {
      auto left = ParseIdOfKind(alias.left().memory_location_id(),
                                core::IdKind::kMemoryRef);
      auto right = ParseIdOfKind(alias.right().memory_location_id(),
                                 core::IdKind::kMemoryRef);
      if (!left.ok())
        return left.status();
      if (!right.ok())
        return right.status();
      const std::string &left_diag = alias.left().object().diagnostic_name();
      const std::string &right_diag = alias.right().object().diagnostic_name();
      auto add_left = graph.AddNode(CpgNode{
          *left, NodeKind::kMemoryObject,
          left_diag.empty() ? alias.left().memory_location_id() : left_diag});
      auto add_right = graph.AddNode(CpgNode{
          *right, NodeKind::kMemoryObject,
          right_diag.empty() ? alias.right().memory_location_id() : right_diag});
      if (!add_left.ok())
        return add_left;
      if (!add_right.ok())
        return add_right;
      auto status = AddSemanticEdge(
          &graph, EdgeKind::kAliases, *left, *right, ToAliasState(alias.kind()),
          {SupportRef{summary_id, alias.provenance_ref()}});
      if (!status.ok())
        return status;
    }

    // UNKNOWN_AT.
    for (const auto &unknown : summary.unknowns()) {
      core::StableId unknown_id = MakeId(
          core::IdKind::kUnknownNode, unknown.scope() + ":" + unknown.reason());
      auto add_unknown = graph.AddNode(
          CpgNode{unknown_id, NodeKind::kUnknown, unknown.kind()});
      if (!add_unknown.ok())
        return add_unknown;
      auto status = AddSemanticEdge(
          &graph, EdgeKind::kUnknownAt, fn_it->second, unknown_id, std::nullopt,
          {SupportRef{summary_id, unknown.provenance_ref()}});
      if (!status.ok())
        return status;
    }
  }

  // 3. Projection metadata: revision/build/module/sorted summary IDs.
  std::sort(summary_ids.begin(), summary_ids.end());
  ::veritas::cpg::ProjectionMetadata meta;
  meta.revision_id = input.revision_id;
  meta.build_variant_id = input.build_variant_id;
  meta.module_hash = std::string(input.program_ir.module_hash());
  meta.summary_ids = std::move(summary_ids);
  graph.SetMetadata(std::move(meta));

  auto validate = graph.Validate();
  if (!validate.ok())
    return validate;
  return graph;
}

} // namespace veritas::analysis::cpg
