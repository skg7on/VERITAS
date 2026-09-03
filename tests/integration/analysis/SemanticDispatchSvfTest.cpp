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

#include "WpaFixtureHarness.h"

#include <algorithm>
#include <map>
#include <set>
#include <string_view>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "analysis/llvm/StableValueMapper.h"
#include "veritas/analysis/semantic/NormalizedAnalysisFacts.h"
#include "veritas/analysis/semantic/SemanticTypes.h"
#include "veritas/wpa/CallGraph.h"
#include "veritas/wpa/SccGraph.h"

namespace veritas::testing {
namespace {

namespace sem = analysis::semantic;
namespace v1 = summary::v1;
namespace v2 = summary::v2;

std::string FunctionIdForSymbol(const AnalyzedFixtureSnapshot& snapshot,
                                std::string_view symbol) {
  const auto it =
      snapshot.function_variant_ids_by_symbol.find(std::string(symbol));
  EXPECT_NE(it, snapshot.function_variant_ids_by_symbol.end()) << symbol;
  if (it == snapshot.function_variant_ids_by_symbol.end()) return {};
  return it->second;
}

std::string FunctionIdForSymbol(const LocalFixtureSnapshot& snapshot,
                                std::string_view symbol) {
  const auto it =
      snapshot.function_variant_ids_by_symbol.find(std::string(symbol));
  EXPECT_NE(it, snapshot.function_variant_ids_by_symbol.end()) << symbol;
  if (it == snapshot.function_variant_ids_by_symbol.end()) return {};
  return it->second;
}

std::string FunctionIdForSymbol(const SvfFixtureSnapshot& snapshot,
                                std::string_view symbol) {
  const auto it =
      snapshot.function_variant_ids_by_symbol.find(std::string(symbol));
  EXPECT_NE(it, snapshot.function_variant_ids_by_symbol.end()) << symbol;
  if (it == snapshot.function_variant_ids_by_symbol.end()) return {};
  return it->second;
}

std::string FunctionValueRefForSymbol(const SvfFixtureSnapshot& snapshot,
                                      std::string_view symbol) {
  const std::string function_id = FunctionIdForSymbol(snapshot, symbol);
  if (function_id.empty()) return {};
  return core::ToString(
      analysis::llvm::StableValueMapper::FunctionValueRef(function_id));
}

std::vector<const sem::NormalizedCallTarget*> SvfCallsForSymbol(
    const SvfFixtureSnapshot& snapshot, std::string_view symbol) {
  std::vector<const sem::NormalizedCallTarget*> calls;
  const std::string caller = FunctionValueRefForSymbol(snapshot, symbol);
  if (caller.empty()) return calls;
  for (const auto& call : snapshot.mapping.facts.calls) {
    if (core::ToString(call.caller) == caller) {
      calls.push_back(&call);
    }
  }
  return calls;
}

std::set<std::string> StableTargetIds(
    std::span<const sem::NormalizedCallTarget* const> calls) {
  std::set<std::string> targets;
  for (const auto* call : calls) {
    if (call->callee.has_value()) {
      targets.insert(core::ToString(*call->callee));
    }
  }
  return targets;
}

void ExpectSvfCallSite(const SvfFixtureSnapshot& snapshot,
                       std::string_view caller, sem::DispatchKind dispatch) {
  const auto calls = SvfCallsForSymbol(snapshot, caller);
  ASSERT_FALSE(calls.empty()) << caller;

  const core::StableId call_site_id = calls.front()->call_site;
  EXPECT_EQ(call_site_id.kind, core::IdKind::kCallSite) << caller;
  for (const auto* call : calls) {
    EXPECT_EQ(call->call_site, call_site_id) << caller;
    EXPECT_EQ(call->dispatch, dispatch) << caller;
    EXPECT_EQ(call->epistemic, sem::EpistemicState::kMay) << caller;
    EXPECT_TRUE(call->callee.has_value()) << caller;
  }
}

void ExpectSvfCallSiteTargets(const SvfFixtureSnapshot& snapshot,
                              std::string_view caller,
                              sem::DispatchKind dispatch,
                              const std::set<std::string>& expected_targets) {
  const auto calls = SvfCallsForSymbol(snapshot, caller);
  ASSERT_EQ(calls.size(), expected_targets.size()) << caller;
  ExpectSvfCallSite(snapshot, caller, dispatch);
  EXPECT_EQ(StableTargetIds(calls), expected_targets) << caller;
}

std::vector<const v2::Call*> CallsForSymbol(
    const AnalyzedFixtureSnapshot& snapshot, std::string_view symbol) {
  std::vector<const v2::Call*> calls;
  const std::string function_id = FunctionIdForSymbol(snapshot, symbol);
  if (function_id.empty()) return calls;
  for (const auto& artifact : snapshot.summaries) {
    const auto* summary = std::get_if<v2::FunctionSummary>(&artifact);
    if (summary == nullptr ||
        summary->identity().function_variant_id() != function_id) {
      continue;
    }
    for (const auto& call : summary->calls()) {
      calls.push_back(&call);
    }
    return calls;
  }
  ADD_FAILURE() << "missing summary for " << symbol;
  return calls;
}

std::vector<const v2::Call*> CallsForSymbol(
    const LocalFixtureSnapshot& snapshot, std::string_view symbol) {
  std::vector<const v2::Call*> calls;
  const std::string function_id = FunctionIdForSymbol(snapshot, symbol);
  if (function_id.empty()) return calls;
  for (const auto& summary : snapshot.summaries) {
    if (summary.identity().function_variant_id() != function_id) {
      continue;
    }
    for (const auto& call : summary.calls()) {
      calls.push_back(&call);
    }
    return calls;
  }
  ADD_FAILURE() << "missing summary for " << symbol;
  return calls;
}

std::set<std::string> StableTargetIds(std::span<const v2::Call* const> calls) {
  std::set<std::string> targets;
  for (const auto* call : calls) {
    if (!call->resolved_callee_function_variant_id().empty()) {
      targets.insert(call->resolved_callee_function_variant_id());
    }
  }
  return targets;
}

void ExpectSingleStableCall(const AnalyzedFixtureSnapshot& snapshot,
                            std::string_view caller, v2::DispatchKind dispatch,
                            v1::EpistemicState epistemic,
                            const std::set<std::string>& expected_targets) {
  const auto calls = CallsForSymbol(snapshot, caller);
  const std::size_t expected_call_rows =
      expected_targets.empty() ? 1u : expected_targets.size();
  ASSERT_EQ(calls.size(), expected_call_rows) << caller;
  ASSERT_FALSE(calls.empty()) << caller;

  const std::string call_site_id = calls.front()->call_site_id();
  ASSERT_TRUE(call_site_id.starts_with("callsite:sha256:")) << caller;
  for (const auto* call : calls) {
    EXPECT_EQ(call->call_site_id(), call_site_id) << caller;
    EXPECT_EQ(call->dispatch(), dispatch) << caller;
    EXPECT_EQ(call->epistemic(), epistemic) << caller;
  }
  EXPECT_EQ(StableTargetIds(calls), expected_targets) << caller;
}

void ExpectSingleStableCall(const LocalFixtureSnapshot& snapshot,
                            std::string_view caller, v2::DispatchKind dispatch,
                            v1::EpistemicState epistemic,
                            const std::set<std::string>& expected_targets) {
  const auto calls = CallsForSymbol(snapshot, caller);
  const std::size_t expected_call_rows =
      expected_targets.empty() ? 1u : expected_targets.size();
  ASSERT_EQ(calls.size(), expected_call_rows) << caller;
  ASSERT_FALSE(calls.empty()) << caller;

  const std::string call_site_id = calls.front()->call_site_id();
  ASSERT_TRUE(call_site_id.starts_with("callsite:sha256:")) << caller;
  for (const auto* call : calls) {
    EXPECT_EQ(call->call_site_id(), call_site_id) << caller;
    EXPECT_EQ(call->dispatch(), dispatch) << caller;
    EXPECT_EQ(call->epistemic(), epistemic) << caller;
  }
  EXPECT_EQ(StableTargetIds(calls), expected_targets) << caller;
}

TEST(SemanticDispatchSvfTest, DistinguishesDirectIndirectCallbackAndVirtual) {
  auto callbacks = AnalyzeAndLoadFixture("callback_dispatch",
                                         analysis::AnalysisConfig::Default());
  ASSERT_TRUE(callbacks.ok()) << callbacks.status().message();
  const std::string callback_left =
      FunctionIdForSymbol(*callbacks, "callback_left");
  const std::string callback_right =
      FunctionIdForSymbol(*callbacks, "callback_right");
  const std::string callback_parameter =
      FunctionIdForSymbol(*callbacks, "callback_parameter");

  ExpectSingleStableCall(*callbacks, "callback_direct",
                         v2::DISPATCH_KIND_DIRECT, v1::EPISTEMIC_STATE_MUST,
                         {callback_left});
  ExpectSingleStableCall(*callbacks, "callback_parameter_entry",
                         v2::DISPATCH_KIND_DIRECT, v1::EPISTEMIC_STATE_MUST,
                         {callback_parameter});
  ExpectSingleStableCall(*callbacks, "callback_select",
                         v2::DISPATCH_KIND_INDIRECT,
                         v1::EPISTEMIC_STATE_UNKNOWN, {});

  auto callback_mapping = MapFixtureWithSvf("callback_dispatch");
  ASSERT_TRUE(callback_mapping.ok()) << callback_mapping.status().message();
  ExpectSvfCallSite(*callback_mapping, "callback_parameter",
                    sem::DispatchKind::kCallback);
  ExpectSvfCallSite(*callback_mapping, "callback_indirect",
                    sem::DispatchKind::kIndirect);
  ExpectSvfCallSite(*callback_mapping, "callback_cast_data_pointer",
                    sem::DispatchKind::kIndirect);
  ExpectSvfCallSite(*callback_mapping, "callback_forwarding_slot",
                    sem::DispatchKind::kIndirect);

  auto virtuals = MapFixtureWithSvf("virtual_dispatch");
  ASSERT_TRUE(virtuals.ok()) << virtuals.status().message();
  const std::string dispatch_left_write =
      FunctionValueRefForSymbol(*virtuals, "_ZN12DispatchLeft5writeEPi");
  const std::string dispatch_right_write =
      FunctionValueRefForSymbol(*virtuals, "_ZN13DispatchRight5writeEPi");

  ExpectSvfCallSiteTargets(*virtuals, "_Z11virtual_twobPi",
                           sem::DispatchKind::kVirtual,
                           {dispatch_left_write, dispatch_right_write});

  auto virtual_local = AnalyzeLocalFixture("virtual_dispatch");
  ASSERT_TRUE(virtual_local.ok()) << virtual_local.status().message();
  const std::string dispatch_right_direct =
      FunctionIdForSymbol(*virtual_local, "_ZN13DispatchRight6directEPi");
  ExpectSingleStableCall(*virtual_local, "_Z10nonvirtualP13DispatchRightPi",
                         v2::DISPATCH_KIND_DIRECT, v1::EPISTEMIC_STATE_MUST,
                         {dispatch_right_direct});
}

TEST(SccGraphTest, FocusedRecursionHasExpectedSccShapes) {
  auto artifacts = AnalyzeAndLoadFixture("recursive_calls",
                                         analysis::AnalysisConfig::Default());
  ASSERT_TRUE(artifacts.ok()) << artifacts.status().message();
  auto graph = wpa::CallGraph::FromSummaries(artifacts->summaries);
  ASSERT_TRUE(graph.ok()) << graph.status().message();
  auto sccs = wpa::SccGraph::Build(*graph);
  ASSERT_TRUE(sccs.ok()) << sccs.status().message();

  bool has_recursive_singleton = false;
  bool has_two_member_scc = false;
  for (const auto& function : graph->Functions()) {
    auto scc = sccs->SccForFunction(function);
    ASSERT_TRUE(scc.ok()) << scc.status().message();
    auto members = sccs->Members(*scc);
    ASSERT_TRUE(members.ok()) << members.status().message();
    if (members->size() == 1 &&
        std::ranges::any_of(graph->Outgoing(function),
                            [&](const auto& edge) {
                              return edge.callee == function;
                            })) {
      has_recursive_singleton = true;
    }
    if (members->size() == 2) has_two_member_scc = true;
  }
  EXPECT_TRUE(has_recursive_singleton);
  EXPECT_TRUE(has_two_member_scc);

  const auto self = artifacts->function_variant_ids_by_symbol.find(
      "recursive_self");
  const auto leaf = artifacts->function_variant_ids_by_symbol.find(
      "recursive_leaf");
  ASSERT_NE(self, artifacts->function_variant_ids_by_symbol.end());
  ASSERT_NE(leaf, artifacts->function_variant_ids_by_symbol.end());
  auto self_id = core::ParseStableId(self->second);
  auto leaf_id = core::ParseStableId(leaf->second);
  ASSERT_TRUE(self_id.ok()) << self_id.status().message();
  ASSERT_TRUE(leaf_id.ok()) << leaf_id.status().message();
  EXPECT_TRUE(std::ranges::any_of(graph->Outgoing(*self_id),
                                  [&](const auto& edge) {
                                    return edge.callee == *leaf_id;
                                  }));
}

TEST(CallGraphTest, UnknownExternalDoesNotFanOut) {
  auto artifacts = AnalyzeAndLoadFixture("unknown_external",
                                         analysis::AnalysisConfig::Default());
  ASSERT_TRUE(artifacts.ok()) << artifacts.status().message();
  auto graph = wpa::CallGraph::FromSummaries(artifacts->summaries);
  ASSERT_TRUE(graph.ok()) << graph.status().message();
  const auto entry = artifacts->function_variant_ids_by_symbol.find(
      "unknown_external_entry");
  ASSERT_NE(entry, artifacts->function_variant_ids_by_symbol.end());
  auto entry_id = core::ParseStableId(entry->second);
  ASSERT_TRUE(entry_id.ok()) << entry_id.status().message();
  EXPECT_EQ(graph->UnknownCalls(*entry_id).size(), 1u);
  EXPECT_TRUE(graph->Outgoing(*entry_id).empty());
}

}  // namespace
}  // namespace veritas::testing
