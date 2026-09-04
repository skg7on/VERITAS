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

// WpaQualificationSupport.h — shared building blocks for the WPA qualification
// corpus (Task 16). The differential, determinism, and failure tests share the
// same summary builders, logical-input materialization, and dual-engine
// execution so every aggregate gate member exercises the same engine-neutral
// input seam.
//
// This header is test-only and header-only on purpose: each test target
// compiles it into its own translation unit, so the helpers are declared
// inline to stay ODR-clean.

#ifndef VERITAS_TESTS_QUALIFICATION_WPA_WPA_QUALIFICATION_SUPPORT_H_
#define VERITAS_TESTS_QUALIFICATION_WPA_WPA_QUALIFICATION_SUPPORT_H_

#include <chrono>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/facts/AnalysisRun.h"
#include "veritas/facts/ResultCanonicalizer.h"
#include "veritas/facts/Witness.h"
#include "veritas/summary/SummaryArtifact.h"
#include "veritas/wpa/CallGraph.h"
#include "veritas/wpa/CppConformanceExecutor.h"
#include "veritas/wpa/SccGraph.h"
#include "veritas/wpa/SouffleWpaExecutor.h"
#include "veritas/wpa/WpaInputMaterializer.h"

namespace veritas::wpa::qualification {

namespace v1 = summary::v1;
namespace v2 = summary::v2;
namespace sem = analysis::semantic;

inline core::StableId FunctionId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kFunctionVariant,
                            std::as_bytes(std::span(name.data(), name.size())));
}

inline core::StableId CallSiteId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kCallSite,
                            std::as_bytes(std::span(name.data(), name.size())));
}

inline core::StableId MemoryId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kMemoryRef,
                            std::as_bytes(std::span(name.data(), name.size())));
}

inline facts::AnalysisRunSemanticDescriptor Semantics() {
  facts::AnalysisRunSemanticDescriptor semantics;
  semantics.build_variant_id = core::MakeStableId(
      core::IdKind::kBuildVariant, std::as_bytes(std::span("bv", 2)));
  semantics.summary_schema_version = "summary.v2";
  semantics.relation_schema_version = "relations.v2";
  semantics.rule_bundle_version = "rules.v2";
  semantics.model_bundle_version = "models.v1";
  semantics.svf_configuration_hash = std::string(64, 'a');
  semantics.wpa_configuration_hash = std::string(64, 'b');
  return semantics;
}

inline facts::AnalysisRunManifest MakeManifest(facts::EngineIdentity engine,
                                               std::string toolchain_identity) {
  facts::AnalysisRunDescriptor descriptor;
  descriptor.revision_id = core::MakeStableId(
      core::IdKind::kRevision, std::as_bytes(std::span("rev", 3)));
  descriptor.build_variant_id = core::MakeStableId(
      core::IdKind::kBuildVariant, std::as_bytes(std::span("bv", 2)));
  descriptor.summary_schema_version = "summary.v2";
  descriptor.relation_schema_version = "relations.v2";
  descriptor.rule_bundle_version = "rules.v2";
  descriptor.model_bundle_version = "models.v1";
  descriptor.svf_configuration_hash = std::string(64, 'a');
  descriptor.wpa_configuration_hash = std::string(64, 'b');
  descriptor.engine = engine;
  descriptor.engine_toolchain_identity = std::move(toolchain_identity);
  auto manifest = facts::MakeAnalysisRun(descriptor);
  return std::move(manifest).value();
}

inline v2::FunctionSummary V2Summary(std::string_view name) {
  v2::FunctionSummary summary;
  summary.mutable_header()->set_schema_version("summary.v2");
  summary.mutable_identity()->set_function_variant_id(
      core::ToString(FunctionId(name)));
  return summary;
}

inline void AddDirectCall(v2::FunctionSummary* summary, std::string_view from,
                          std::string_view to) {
  auto* call = summary->add_calls();
  call->set_call_site_id(
      core::ToString(CallSiteId(std::string(from) + "->" + std::string(to))));
  call->set_callee_symbol(std::string(to));
  call->set_resolved_callee_function_variant_id(core::ToString(FunctionId(to)));
  call->set_dispatch(v2::DISPATCH_KIND_DIRECT);
  call->set_epistemic(v1::EPISTEMIC_STATE_MUST);
  call->set_provenance_ref("test:call");
}

// An indirect (MAY) call to a resolved target: the same call site admits
// several targets, each a distinct direct-call row with dispatch=indirect and
// epistemic=may, matching how SVF normalization emits resolved MAY edges.
inline void AddIndirectCall(v2::FunctionSummary* summary, std::string_view from,
                            std::string_view to) {
  auto* call = summary->add_calls();
  call->set_call_site_id(
      core::ToString(CallSiteId(std::string(from) + "->fp")));
  call->set_callee_symbol(std::string(to));
  call->set_resolved_callee_function_variant_id(core::ToString(FunctionId(to)));
  call->set_dispatch(v2::DISPATCH_KIND_INDIRECT);
  call->set_epistemic(v1::EPISTEMIC_STATE_MAY);
  call->set_provenance_ref("test:indirect");
}

inline void AddMemoryWrite(v2::FunctionSummary* summary,
                           std::string_view memory, bool known_range) {
  auto* effect = summary->add_memory_effects();
  effect->set_kind(v1::EFFECT_KIND_WRITE);
  effect->mutable_location()->set_memory_location_id(
      core::ToString(MemoryId(memory)));
  auto* range = effect->mutable_location()->mutable_byte_range();
  range->set_offset_known(known_range);
  range->set_offset(known_range ? 0 : 0);
  range->set_size_known(known_range);
  range->set_size(known_range ? 8 : 0);
  effect->set_epistemic(v1::EPISTEMIC_STATE_MUST);
  effect->set_provenance_ref("test:write");
}

// Materializes the logical input for one component rooted at `root`.
inline StatusOr<WpaLogicalComponentInput> InputFor(
    const std::vector<summary::SummaryArtifact>& artifacts,
    WpaComponentKind component, std::string_view root) {
  auto graph = CallGraph::FromSummaries(artifacts);
  if (!graph.ok())
    return graph.status();
  auto scc = SccGraph::Build(*graph);
  if (!scc.ok())
    return scc.status();
  auto scc_id = scc->SccForFunction(FunctionId(root));
  if (!scc_id.ok())
    return scc_id.status();

  WpaMaterializationRequest request;
  request.semantics = Semantics();
  request.scc_id = *scc_id;
  request.component = component;
  request.summaries = artifacts;
  return WpaInputMaterializer::Build(request);
}

inline StatusOr<facts::CanonicalizedResult> Canonicalize(
    const WpaLogicalComponentInput& logical,
    const facts::RawWpaEvaluation& raw) {
  facts::CanonicalizationRequest request;
  request.local_roots = logical.local_roots;
  request.successor_roots = logical.successor_roots;
  request.evaluation = &raw;
  return facts::ResultCanonicalizer::Canonicalize(request);
}

// The two canonical results for one logical input under its two execution
// envelopes. The two runs must carry distinct run IDs and distinct engine
// identities, but byte-identical logical input.
struct CanonicalPair {
  facts::CanonicalizedResult souffle;
  facts::CanonicalizedResult cpp;
};

inline StatusOr<CanonicalPair> RunBothEngines(
    const WpaLogicalComponentInput& logical) {
  const auto souffle_manifest =
      MakeManifest(facts::EngineIdentity::kSouffle, "souffle-toolchain");
  const auto cpp_manifest = MakeManifest(facts::EngineIdentity::kCppConformance,
                                         "cpp-toolchain");

  WpaExecutionEnvelope souffle_envelope{souffle_manifest, logical};
  WpaExecutionEnvelope cpp_envelope{cpp_manifest, logical};

  SouffleWpaExecutor souffle(VERITAS_SOUFFLE_WORKER, "souffle-toolchain");
  auto cpp = CppConformanceExecutor::Create(
      facts::EngineIdentity::kCppConformance, "cpp-toolchain");
  if (!cpp.ok())
    return cpp.status();

  const WpaExecutionLimits limits{std::chrono::seconds(30), 0, 1};
  auto souffle_raw = souffle.Execute(souffle_envelope, limits);
  if (!souffle_raw.ok())
    return souffle_raw.status();
  auto cpp_raw = cpp->Execute(cpp_envelope, limits);
  if (!cpp_raw.ok())
    return cpp_raw.status();

  auto souffle_canonical = Canonicalize(logical, *souffle_raw);
  if (!souffle_canonical.ok())
    return souffle_canonical.status();
  auto cpp_canonical = Canonicalize(logical, *cpp_raw);
  if (!cpp_canonical.ok())
    return cpp_canonical.status();

  return CanonicalPair{std::move(*souffle_canonical),
                       std::move(*cpp_canonical)};
}

// True when any EDB row carries a ByteRangeKind cell of the requested kind.
inline bool ContainsRangeKind(const WpaLogicalComponentInput& logical,
                              sem::ByteRangeKind kind) {
  for (const auto& row : logical.edb) {
    for (const auto& cell : row.cells) {
      if (const auto* range = std::get_if<sem::ByteRangeKind>(&cell)) {
        if (*range == kind)
          return true;
      }
    }
  }
  return false;
}

}  // namespace veritas::wpa::qualification

#endif  // VERITAS_TESTS_QUALIFICATION_WPA_WPA_QUALIFICATION_SUPPORT_H_
