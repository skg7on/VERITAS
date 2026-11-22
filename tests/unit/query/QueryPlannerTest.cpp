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

// QueryPlannerTest.cpp — query identity, relevance scope and explicit
// frontiers. The planner is exercised without an engine: these tests pin the
// decisions that no later execution step may silently change.

#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "veritas/core/Ids.h"
#include "veritas/query/QueryPlanner.h"
#include "veritas/query/QueryTypes.h"
#include "veritas/summary/v2/summary.pb.h"

namespace veritas::query {
namespace {

namespace v1 = summary::v1;
namespace v2 = summary::v2;

core::StableId Id(core::IdKind kind, std::string_view text) {
  return core::MakeStableId(kind,
                            std::as_bytes(std::span(text.data(), text.size())));
}

TypedRef FunctionRef(std::string_view name) {
  const auto id = Id(core::IdKind::kFunctionVariant, name);
  return TypedRef{core::IdKind::kFunctionVariant, id, id};
}

TypedRef ValueRef(std::string_view value, std::string_view function) {
  return TypedRef{core::IdKind::kValueRef, Id(core::IdKind::kValueRef, value),
                  Id(core::IdKind::kFunctionVariant, function)};
}

TypedRef MemoryRef(std::string_view memory, std::string_view function) {
  return TypedRef{core::IdKind::kMemoryRef,
                  Id(core::IdKind::kMemoryRef, memory),
                  Id(core::IdKind::kFunctionVariant, function)};
}

facts::AnalysisRunSemanticDescriptor Semantics() {
  facts::AnalysisRunSemanticDescriptor semantics;
  semantics.build_variant_id = Id(core::IdKind::kBuildVariant, "bv");
  semantics.summary_schema_version = "summary.v2";
  semantics.relation_schema_version = "relations.v2";
  semantics.rule_bundle_version = "rules.v2";
  semantics.model_bundle_version = "models.v1";
  semantics.svf_configuration_hash = std::string(64, 'a');
  semantics.wpa_configuration_hash = std::string(64, 'b');
  return semantics;
}

review::ReviewBinding Binding() {
  review::ReviewBinding binding;
  binding.repository_id = "repository";
  binding.base_revision_id = "revision";
  binding.working_tree_digest = std::string(64, 'c');
  binding.build_context_digest = std::string(64, 'd');
  return binding;
}

QueryDescriptor Descriptor(QueryPredicate predicate,
                           std::vector<TypedRef> sources,
                           std::vector<TypedRef> sinks = {},
                           std::vector<TypedRef> subjects = {}) {
  QueryDescriptor descriptor;
  descriptor.binding = Binding();
  descriptor.predicate = predicate;
  descriptor.sources = std::move(sources);
  descriptor.sinks = std::move(sinks);
  descriptor.subjects = std::move(subjects);
  descriptor.producers = DefaultProducers(Semantics());
  descriptor.scope_digest = std::string(64, 'e');
  return descriptor;
}

v2::FunctionSummary Summary(std::string_view name) {
  v2::FunctionSummary summary;
  summary.mutable_header()->set_schema_version("summary.v2");
  summary.mutable_identity()->set_function_variant_id(
      core::ToString(Id(core::IdKind::kFunctionVariant, name)));
  return summary;
}

void AddCall(v2::FunctionSummary *summary, std::string_view from,
             std::string_view to) {
  auto *call = summary->add_calls();
  call->set_call_site_id(core::ToString(
      Id(core::IdKind::kCallSite, std::string(from) + "->" + std::string(to))));
  call->set_callee_symbol(std::string(to));
  call->set_resolved_callee_function_variant_id(
      core::ToString(Id(core::IdKind::kFunctionVariant, to)));
  call->set_dispatch(v2::DISPATCH_KIND_DIRECT);
  call->set_epistemic(v1::EPISTEMIC_STATE_MUST);
  call->set_provenance_ref("test:call");
}

void AddOpenCall(v2::FunctionSummary *summary, std::string_view from,
                 std::string_view callsite, v2::DispatchKind dispatch) {
  auto *call = summary->add_calls();
  call->set_call_site_id(core::ToString(Id(core::IdKind::kCallSite, callsite)));
  call->set_callee_symbol(std::string(callsite));
  call->set_dispatch(dispatch);
  call->set_epistemic(v1::EPISTEMIC_STATE_MAY);
  call->set_provenance_ref("test:open-call");
}

DependencySnapshot Snapshot(std::vector<summary::SummaryArtifact> summaries) {
  DependencySnapshot snapshot;
  snapshot.revision_id = Id(core::IdKind::kRevision, "revision");
  snapshot.semantics = Semantics();
  snapshot.summaries = std::move(summaries);
  return snapshot;
}

const QueryFrontierEntry *FindFrontier(const QueryPlan &plan,
                                       QueryFrontierKind kind) {
  for (const auto &entry : plan.frontier) {
    if (entry.kind == kind) {
      return &entry;
    }
  }
  return nullptr;
}

// Parses the query-seed manifest's predicate blocks into an ordered table.
struct ManifestSeed {
  std::string predicate;
  std::string component;
  std::string relation;
  std::string source_kind;
  std::string sink_kind;
  std::string baseline_bundle;
};

std::vector<ManifestSeed> ParseSeedManifest(const std::string &path) {
  std::ifstream in(path);
  std::vector<ManifestSeed> seeds;
  ManifestSeed current;
  bool in_block = false;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line.front() == '#') {
      continue;
    }
    const auto equals = line.find('=');
    if (equals == std::string::npos) {
      continue;
    }
    const std::string key = line.substr(0, equals);
    const std::string value = line.substr(equals + 1);
    if (key == "predicate") {
      if (in_block) {
        seeds.push_back(current);
      }
      current = ManifestSeed{};
      current.predicate = value;
      in_block = true;
    } else if (key == "component") {
      current.component = value;
    } else if (key == "relation") {
      current.relation = value;
    } else if (key == "source_kind") {
      current.source_kind = value;
    } else if (key == "sink_kind") {
      current.sink_kind = value;
    } else if (key == "baseline_bundle") {
      current.baseline_bundle = value;
    }
  }
  if (in_block) {
    seeds.push_back(current);
  }
  return seeds;
}

TEST(QueryPlannerTest, DoesNotCutOversizedScc) {
  // One recursive SCC of 65 functions: every function calls the next, and the
  // last calls the first. The default function limit is 64.
  constexpr int kFunctions = 65;
  std::vector<summary::SummaryArtifact> summaries;
  for (int i = 0; i < kFunctions; ++i) {
    auto summary = Summary("f" + std::to_string(i));
    AddCall(&summary, "f" + std::to_string(i),
            "f" + std::to_string((i + 1) % kFunctions));
    summaries.push_back(summary);
  }
  auto snapshot = Snapshot(std::move(summaries));
  auto descriptor = Descriptor(QueryPredicate::kReachableCall,
                               {FunctionRef("f0")}, {FunctionRef("f32")});
  auto plan = PlanQuery(descriptor, snapshot);
  ASSERT_TRUE(plan.ok()) << plan.status().message();

  EXPECT_FALSE(plan->scope_closed);
  EXPECT_TRUE(plan->sccs.empty());
  ASSERT_EQ(plan->excluded_sccs.size(), 1u);
  EXPECT_EQ(plan->excluded_sccs.front().members.size(),
            static_cast<std::size_t>(kFunctions));
  const auto *oversized = FindFrontier(*plan, QueryFrontierKind::kOversizedScc);
  ASSERT_NE(oversized, nullptr);
  ASSERT_TRUE(oversized->ref.has_value());
  EXPECT_EQ(*oversized->ref, plan->excluded_sccs.front().scc_id);
}

TEST(QueryPlannerTest, UnresolvedCallbackKeepsFrontierOpen) {
  auto f = Summary("f");
  AddOpenCall(&f, "f", "f-callback", v2::DISPATCH_KIND_CALLBACK);
  auto snapshot = Snapshot({f});
  auto descriptor =
      Descriptor(QueryPredicate::kReachableCall, {FunctionRef("f")});
  auto plan = PlanQuery(descriptor, snapshot);
  ASSERT_TRUE(plan.ok()) << plan.status().message();

  EXPECT_FALSE(plan->scope_closed);
  const auto *unresolved =
      FindFrontier(*plan, QueryFrontierKind::kUnresolvedCall);
  ASSERT_NE(unresolved, nullptr);
  ASSERT_TRUE(unresolved->ref.has_value());
  EXPECT_EQ(*unresolved->ref, Id(core::IdKind::kCallSite, "f-callback"));
}

TEST(QueryPlannerTest, BudgetsAndProducerVersionsMoveQueryIdentity) {
  auto descriptor =
      Descriptor(QueryPredicate::kReachableCall, {FunctionRef("f")});
  auto base = ComputeQueryId(descriptor);
  ASSERT_TRUE(base.ok()) << base.status().message();

  auto budget = descriptor;
  budget.limits.max_sccs = 7;
  auto moved = ComputeQueryId(budget);
  ASSERT_TRUE(moved.ok());
  EXPECT_NE(*base, *moved);

  auto producers = descriptor;
  producers.producers.front().version = "rules.v3";
  producers.producers.front().config_digest = std::string(64, 'f');
  auto producer_moved = ComputeQueryId(producers);
  ASSERT_TRUE(producer_moved.ok());
  EXPECT_NE(*base, *producer_moved);
}

TEST(QueryPlannerTest, TimingDoesNotMoveQueryIdentity) {
  auto descriptor =
      Descriptor(QueryPredicate::kReachableCall, {FunctionRef("f")});
  auto base = ComputeQueryId(descriptor);
  ASSERT_TRUE(base.ok());
  descriptor.runtime.timeout_ms = 123456;
  descriptor.runtime.memory_mb = 4096;
  auto timed = ComputeQueryId(descriptor);
  ASSERT_TRUE(timed.ok());
  EXPECT_EQ(*base, *timed);
}

TEST(QueryPlannerTest, RejectsEndpointKindThatDisagreesWithThePredicate) {
  // A global-flow endpoint is a value ref, and a may-read sink is a memory ref.
  const auto global_flow = Descriptor(
      QueryPredicate::kGlobalFlow, {ValueRef("v", "f")}, {ValueRef("w", "g")});
  EXPECT_TRUE(ComputeQueryId(global_flow).ok());

  const auto may_read = Descriptor(QueryPredicate::kMayRead, {FunctionRef("f")},
                                   {MemoryRef("m", "f")});
  EXPECT_TRUE(ComputeQueryId(may_read).ok());

  // The same shapes with the wrong endpoint kind are rejected before planning.
  auto wrong_flow_source =
      Descriptor(QueryPredicate::kGlobalFlow, {FunctionRef("f")});
  auto flow_plan = PlanQuery(wrong_flow_source, Snapshot({Summary("f")}));
  ASSERT_FALSE(flow_plan.ok());
  EXPECT_EQ(flow_plan.status().code(), StatusCode::kInvalidArgument);

  auto wrong_memory_sink = Descriptor(QueryPredicate::kMayRead,
                                      {FunctionRef("f")}, {ValueRef("v", "f")});
  auto memory_plan = PlanQuery(wrong_memory_sink, Snapshot({Summary("f")}));
  ASSERT_FALSE(memory_plan.ok());
  EXPECT_EQ(memory_plan.status().code(), StatusCode::kInvalidArgument);
}

TEST(QueryPlannerTest, GlobalFlowRelevanceIsUndirectedAcrossTheCallEdge) {
  // f calls g. A value-flow question from a value in g to a value in f must
  // admit both SCCs: value flow crosses parameter and return edges in either
  // direction, so a plain forward slice would drop the caller.
  auto f = Summary("f");
  AddCall(&f, "f", "g");
  auto snapshot = Snapshot({f, Summary("g")});
  auto descriptor = Descriptor(QueryPredicate::kGlobalFlow,
                               {ValueRef("v-g", "g")}, {ValueRef("v-f", "f")});
  auto plan = PlanQuery(descriptor, snapshot);
  ASSERT_TRUE(plan.ok()) << plan.status().message();

  EXPECT_TRUE(plan->scope_closed);
  EXPECT_EQ(plan->sccs.size(), 2u);
  EXPECT_EQ(plan->component, wpa::WpaComponentKind::kFlow);
  EXPECT_EQ(plan->relation, facts::RelationId::kGlobalFlow);
}

TEST(QueryPlannerTest, MemoryEffectRelevanceFollowsCallees) {
  auto f = Summary("f");
  AddCall(&f, "f", "g");
  auto snapshot = Snapshot({f, Summary("g")});
  auto descriptor = Descriptor(QueryPredicate::kMayRead, {FunctionRef("f")},
                               {MemoryRef("m-f", "f")});
  auto plan = PlanQuery(descriptor, snapshot);
  ASSERT_TRUE(plan.ok()) << plan.status().message();

  EXPECT_TRUE(plan->scope_closed);
  EXPECT_EQ(plan->sccs.size(), 2u);
  EXPECT_EQ(plan->component, wpa::WpaComponentKind::kMemoryEffects);
  EXPECT_EQ(plan->relation, facts::RelationId::kMayRead);
}

TEST(QueryPlannerTest, DisconnectedFunctionsStayOutOfScope) {
  auto f = Summary("f");
  AddCall(&f, "f", "g");
  std::vector<summary::SummaryArtifact> summaries = {f, Summary("g")};
  for (int i = 0; i < 20; ++i) {
    summaries.push_back(Summary("unrelated" + std::to_string(i)));
  }
  auto snapshot = Snapshot(std::move(summaries));
  auto descriptor =
      Descriptor(QueryPredicate::kReachableCall, {FunctionRef("f")});
  auto plan = PlanQuery(descriptor, snapshot);
  ASSERT_TRUE(plan.ok()) << plan.status().message();

  EXPECT_TRUE(plan->scope_closed);
  EXPECT_TRUE(plan->frontier.empty());
  EXPECT_EQ(plan->sccs.size(), 2u);
  EXPECT_EQ(
      plan->relevant_functions,
      (std::vector<core::StableId>{Id(core::IdKind::kFunctionVariant, "f"),
                                   Id(core::IdKind::kFunctionVariant, "g")}));
}

TEST(QueryPlannerTest, SccBudgetKeepsAnswerIncomplete) {
  std::vector<summary::SummaryArtifact> summaries;
  for (int i = 0; i < 10; ++i) {
    auto summary = Summary("f" + std::to_string(i));
    if (i + 1 < 10) {
      AddCall(&summary, "f" + std::to_string(i), "f" + std::to_string(i + 1));
    }
    summaries.push_back(summary);
  }
  auto snapshot = Snapshot(std::move(summaries));
  auto descriptor = Descriptor(QueryPredicate::kReachableCall,
                               {FunctionRef("f0")}, {FunctionRef("f9")});
  descriptor.limits.max_sccs = 3;
  auto plan = PlanQuery(descriptor, snapshot);
  ASSERT_TRUE(plan.ok()) << plan.status().message();

  EXPECT_FALSE(plan->scope_closed);
  EXPECT_EQ(plan->sccs.size(), 3u);
  EXPECT_EQ(plan->excluded_sccs.size(), 7u);
  EXPECT_NE(FindFrontier(*plan, QueryFrontierKind::kSccBudget), nullptr);
}

TEST(QueryPlannerTest, BoundaryCoveringAnExternalCallClosesTheFrontier) {
  auto f = Summary("f");
  AddOpenCall(&f, "f", "f-external", v2::DISPATCH_KIND_EXTERNAL);

  BoundarySummary boundary;
  boundary.function_variant_id = Id(core::IdKind::kFunctionVariant, "f");
  boundary.configuration_digest = std::string(64, 'a');
  boundary.layout_digest = std::string(64, 'b');
  boundary.supported_predicates = {QueryPredicate::kReachableCall};
  boundary.covered_call_sites = {Id(core::IdKind::kCallSite, "f-external")};
  boundary.producer_version = "boundary-producer.v1";
  auto boundary_id = ComputeBoundarySummaryId(boundary);
  ASSERT_TRUE(boundary_id.ok()) << boundary_id.status().message();

  auto snapshot = Snapshot({f});
  snapshot.boundary_summaries.push_back(boundary);
  auto descriptor =
      Descriptor(QueryPredicate::kReachableCall, {FunctionRef("f")});
  descriptor.boundary_summaries.push_back(*boundary_id);
  auto plan = PlanQuery(descriptor, snapshot);
  ASSERT_TRUE(plan.ok()) << plan.status().message();

  EXPECT_TRUE(plan->scope_closed);
  EXPECT_TRUE(plan->frontier.empty());
  ASSERT_EQ(plan->boundary_summaries.size(), 1u);
  EXPECT_EQ(plan->boundary_summaries.front(), boundary);
}

TEST(QueryPlannerTest, BoundarySummaryRoundTripsWithItsOwnVersion) {
  BoundarySummary boundary;
  boundary.function_variant_id = Id(core::IdKind::kFunctionVariant, "f");
  boundary.configuration_digest = std::string(64, 'a');
  boundary.layout_digest = std::string(64, 'b');
  boundary.supported_predicates = {QueryPredicate::kReachableCall};
  boundary.producer_version = "boundary-producer.v1";
  boundary.assumptions = {"assume-no-hidden-callback"};
  boundary.dependency_digests = {std::string(64, 'c')};

  facts::SemanticRow row;
  row.relation = facts::RelationId::kReachableCall;
  row.cells = {Id(core::IdKind::kFunctionVariant, "f"),
               Id(core::IdKind::kFunctionVariant, "g"),
               analysis::semantic::EpistemicState::kMust};
  auto fact = facts::MakeFact(row);
  ASSERT_TRUE(fact.ok()) << fact.status().message();
  boundary.facts.push_back(*fact);

  auto bytes = SerializeBoundarySummary(boundary);
  ASSERT_TRUE(bytes.ok()) << bytes.status().message();
  auto parsed = ParseBoundarySummary(*bytes);
  ASSERT_TRUE(parsed.ok()) << parsed.status().message();
  EXPECT_EQ(*parsed, boundary);

  auto first_id = ComputeBoundarySummaryId(boundary);
  auto second_id = ComputeBoundarySummaryId(*parsed);
  ASSERT_TRUE(first_id.ok());
  ASSERT_TRUE(second_id.ok());
  EXPECT_EQ(*first_id, *second_id);

  auto wrong_version = boundary;
  wrong_version.schema_version = "summary.v2";
  EXPECT_FALSE(ValidateBoundarySummary(wrong_version).ok());
  EXPECT_FALSE(ParseBoundarySummary("").ok());
}

TEST(QueryPlannerTest, QuerySeedRegistryMatchesManifest) {
  const auto seeds = ParseSeedManifest(std::string(VERITAS_LOGIC_DIR) +
                                       "/query/query_seed.v1.manifest");
  ASSERT_EQ(seeds.size(), RegisteredQuerySeeds().size());
  for (std::size_t i = 0; i < seeds.size(); ++i) {
    const auto &compiled = RegisteredQuerySeeds()[i];
    const auto predicate = ParsePredicate(seeds[i].predicate);
    ASSERT_TRUE(predicate.ok()) << seeds[i].predicate;
    EXPECT_EQ(*predicate, compiled.predicate);
    EXPECT_EQ(wpa::ComponentKindName(compiled.component), seeds[i].component);
    EXPECT_EQ(facts::RelationsV2().Get(compiled.relation).name,
              seeds[i].relation);
    EXPECT_EQ(core::IdKindToString(compiled.source_kind), seeds[i].source_kind);
    if (compiled.sink_kind.has_value()) {
      EXPECT_EQ(core::IdKindToString(*compiled.sink_kind), seeds[i].sink_kind);
    } else {
      EXPECT_EQ(seeds[i].sink_kind, "-");
    }
    EXPECT_EQ(compiled.baseline_bundle, seeds[i].baseline_bundle);
  }
}

TEST(QueryPlannerTest, ModelsMoveQueryIdentity) {
  auto descriptor =
      Descriptor(QueryPredicate::kReachableCall, {FunctionRef("f")});
  auto base = ComputeQueryId(descriptor);
  ASSERT_TRUE(base.ok()) << base.status().message();

  bool found = false;
  for (auto &producer : descriptor.producers) {
    if (producer.name == "veritas.models") {
      producer.version = "models.v2";
      producer.config_digest = std::string(64, 'f');
      found = true;
    }
  }
  ASSERT_TRUE(found) << "the canonical producer set must bind the model bundle";
  auto moved = ComputeQueryId(descriptor);
  ASSERT_TRUE(moved.ok());
  EXPECT_NE(*base, *moved);
}

TEST(QueryPlannerTest, AdmitsExactlyTheFunctionBudget) {
  // A two-function recursive SCC is admitted at exactly the limit and excluded
  // whole one function below it.
  auto f = Summary("f");
  AddCall(&f, "f", "g");
  auto g = Summary("g");
  AddCall(&g, "g", "f");
  const DependencySnapshot snapshot = Snapshot({f, g});

  auto at_limit =
      Descriptor(QueryPredicate::kReachableCall, {FunctionRef("f")});
  at_limit.limits.max_functions = 2;
  auto admitted = PlanQuery(at_limit, snapshot);
  ASSERT_TRUE(admitted.ok()) << admitted.status().message();
  EXPECT_TRUE(admitted->scope_closed);
  EXPECT_EQ(admitted->sccs.size(), 1u);
  EXPECT_TRUE(admitted->excluded_sccs.empty());

  auto below_limit = at_limit;
  below_limit.limits.max_functions = 1;
  auto excluded = PlanQuery(below_limit, snapshot);
  ASSERT_TRUE(excluded.ok()) << excluded.status().message();
  EXPECT_FALSE(excluded->scope_closed);
  EXPECT_TRUE(excluded->sccs.empty());
  EXPECT_NE(FindFrontier(*excluded, QueryFrontierKind::kOversizedScc), nullptr);
}

TEST(QueryPlannerTest, AdmitsExactlyTheSccBudget) {
  std::vector<summary::SummaryArtifact> summaries;
  for (int i = 0; i < 3; ++i) {
    auto summary = Summary("f" + std::to_string(i));
    if (i + 1 < 3) {
      AddCall(&summary, "f" + std::to_string(i), "f" + std::to_string(i + 1));
    }
    summaries.push_back(summary);
  }
  const DependencySnapshot snapshot = Snapshot(std::move(summaries));

  auto at_limit =
      Descriptor(QueryPredicate::kReachableCall, {FunctionRef("f0")});
  at_limit.limits.max_sccs = 3;
  auto admitted = PlanQuery(at_limit, snapshot);
  ASSERT_TRUE(admitted.ok()) << admitted.status().message();
  EXPECT_TRUE(admitted->scope_closed);
  EXPECT_EQ(admitted->sccs.size(), 3u);

  auto below_limit = at_limit;
  below_limit.limits.max_sccs = 2;
  auto excluded = PlanQuery(below_limit, snapshot);
  ASSERT_TRUE(excluded.ok()) << excluded.status().message();
  EXPECT_FALSE(excluded->scope_closed);
  EXPECT_EQ(excluded->sccs.size(), 2u);
  EXPECT_NE(FindFrontier(*excluded, QueryFrontierKind::kSccBudget), nullptr);
}

} // namespace
} // namespace veritas::query
