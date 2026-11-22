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

// QueryExecutionTest.cpp — demand-scoped execution against the production
// compiled-Souffle engine, with the whole-program WPA run as the oracle.
//
// The closed-fixture comparison is exact: same facts (including epistemic
// cells), same selected derivations, same roots. The scripted executor only
// substitutes the *failure* of a kernel (timeout, dead worker, an unproven
// result); it never changes a successful component's evaluation.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>

#include "veritas/core/Ids.h"
#include "veritas/facts/AnalysisFact.h"
#include "veritas/query/QueryExecutor.h"
#include "veritas/query/QueryPlanner.h"
#include "veritas/query/QueryTypes.h"
#include "veritas/summary/v2/summary.pb.h"
#include "veritas/wpa/SccGraph.h"
#include "veritas/wpa/SouffleWpaExecutor.h"
#include "veritas/wpa/WpaOrchestrator.h"
#include "veritas/wpa/WpaRunRepository.h"

namespace veritas::query {
namespace {

namespace v1 = summary::v1;
namespace v2 = summary::v2;

using core::StableId;
using facts::AnalysisFact;
using facts::SemanticRow;
using facts::WitnessEdge;

constexpr std::string_view kToolchain = "souffle-toolchain";

StableId Id(core::IdKind kind, std::string_view text) {
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
                           std::vector<TypedRef> sinks = {}) {
  QueryDescriptor descriptor;
  descriptor.binding = Binding();
  descriptor.predicate = predicate;
  descriptor.sources = std::move(sources);
  descriptor.sinks = std::move(sinks);
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

void AddOpenExternalCall(v2::FunctionSummary *summary,
                         std::string_view callsite) {
  auto *call = summary->add_calls();
  call->set_call_site_id(core::ToString(Id(core::IdKind::kCallSite, callsite)));
  call->set_callee_symbol(std::string(callsite));
  call->set_dispatch(v2::DISPATCH_KIND_EXTERNAL);
  call->set_epistemic(v1::EPISTEMIC_STATE_MAY);
  call->set_provenance_ref("test:external-call");
}

void AddValueFlow(v2::FunctionSummary *summary, std::string_view source,
                  std::string_view destination) {
  auto *flow = summary->add_value_flows();
  flow->set_source_value_id(
      core::ToString(Id(core::IdKind::kValueRef, source)));
  flow->set_destination_value_id(
      core::ToString(Id(core::IdKind::kValueRef, destination)));
  flow->set_epistemic(v1::EPISTEMIC_STATE_MUST);
  flow->set_provenance_ref("test:flow");
}

void AddMemoryEffect(v2::FunctionSummary *summary, std::string_view memory,
                     bool is_write) {
  auto *effect = summary->add_memory_effects();
  effect->set_kind(is_write ? v1::EFFECT_KIND_WRITE : v1::EFFECT_KIND_READ);
  effect->mutable_location()->set_memory_location_id(
      core::ToString(Id(core::IdKind::kMemoryRef, memory)));
  effect->set_epistemic(v1::EPISTEMIC_STATE_MUST);
  effect->set_provenance_ref("test:memory-effect");
}

void AddAlias(v2::FunctionSummary *summary, std::string_view left,
              std::string_view right, v2::AliasKind kind) {
  auto *alias = summary->add_alias_facts();
  alias->mutable_left()->set_memory_location_id(
      core::ToString(Id(core::IdKind::kMemoryRef, left)));
  alias->mutable_right()->set_memory_location_id(
      core::ToString(Id(core::IdKind::kMemoryRef, right)));
  alias->set_kind(kind);
  alias->set_epistemic(v1::EPISTEMIC_STATE_MAY);
  alias->set_provenance_ref("test:alias");
}

DependencySnapshot Snapshot(std::vector<summary::SummaryArtifact> summaries) {
  DependencySnapshot snapshot;
  snapshot.revision_id = Id(core::IdKind::kRevision, "revision");
  snapshot.semantics = Semantics();
  snapshot.summaries = std::move(summaries);
  return snapshot;
}

std::optional<AnalysisFact> MakeReachableFact(std::string_view source,
                                              std::string_view target) {
  SemanticRow row;
  row.relation = facts::RelationId::kReachableCall;
  row.cells = {Id(core::IdKind::kFunctionVariant, source),
               Id(core::IdKind::kFunctionVariant, target),
               analysis::semantic::EpistemicState::kMust};
  auto fact = facts::MakeFact(row);
  if (!fact.ok()) {
    return std::nullopt;
  }
  return *fact;
}

BoundarySummary MakeBoundary(std::string_view function,
                             std::vector<AnalysisFact> facts,
                             std::vector<StableId> covered_call_sites = {}) {
  BoundarySummary boundary;
  boundary.function_variant_id = Id(core::IdKind::kFunctionVariant, function);
  boundary.configuration_digest = std::string(64, 'a');
  boundary.layout_digest = std::string(64, 'b');
  boundary.supported_predicates = {QueryPredicate::kReachableCall};
  boundary.covered_call_sites = std::move(covered_call_sites);
  boundary.producer_version = "test-boundary.v1";
  boundary.facts = std::move(facts);
  return boundary;
}

facts::AnalysisRunManifest ManifestFor(const DependencySnapshot &snapshot) {
  facts::AnalysisRunDescriptor descriptor;
  static_cast<facts::AnalysisRunSemanticDescriptor &>(descriptor) =
      snapshot.semantics;
  descriptor.revision_id = snapshot.revision_id;
  descriptor.engine = facts::EngineIdentity::kSouffle;
  descriptor.engine_toolchain_identity = std::string(kToolchain);
  return std::move(facts::MakeAnalysisRun(descriptor)).value();
}

std::filesystem::path TempPath(std::string_view tag) {
  static std::uint64_t counter = 0;
  return std::filesystem::temp_directory_path() /
         ("veritas_query_" + std::string(tag) + "_" +
          std::to_string(static_cast<unsigned long long>(::getpid())) + "_" +
          std::to_string(++counter));
}

struct Baseline {
  std::vector<AnalysisFact> facts;
  std::vector<WitnessEdge> witnesses;
  std::vector<facts::RootedInputFact> roots;
  std::size_t kernels = 0;
};

Baseline RunBaseline(const DependencySnapshot &snapshot,
                     QueryPredicate predicate) {
  Baseline baseline;
  const auto db = TempPath("baseline");
  std::filesystem::remove_all(db);
  auto repository = wpa::WpaRunRepository::Open(db);
  EXPECT_TRUE(repository.ok()) << repository.status().message();
  if (!repository.ok()) {
    return baseline;
  }
  wpa::SouffleWpaExecutor engine(VERITAS_SOUFFLE_WORKER,
                                 std::string(kToolchain));
  wpa::WpaOrchestrator orchestrator(engine, *repository);

  wpa::WpaRunRequest request;
  request.run = ManifestFor(snapshot);
  request.summaries = snapshot.summaries;
  const std::array<wpa::WpaComponentKind, 1> components = {
      PredicateComponent(predicate)};
  request.components = components;
  auto result = orchestrator.Run(request);
  EXPECT_TRUE(result.ok()) << result.status().message();
  if (!result.ok()) {
    std::filesystem::remove_all(db);
    return baseline;
  }
  for (const auto &completion : result->completed_components) {
    baseline.facts.insert(baseline.facts.end(), completion.result.facts.begin(),
                          completion.result.facts.end());
    baseline.witnesses.insert(baseline.witnesses.end(),
                              completion.result.witnesses.begin(),
                              completion.result.witnesses.end());
    ++baseline.kernels;
  }
  baseline.roots = result->rooted_input_facts;
  std::sort(baseline.facts.begin(), baseline.facts.end(),
            [](const AnalysisFact &a, const AnalysisFact &b) {
              return a.fact_id < b.fact_id;
            });
  std::filesystem::remove_all(db);
  return baseline;
}

std::vector<AnalysisFact>
FilterBySourceRelation(const std::vector<AnalysisFact> &facts,
                       facts::RelationId relation, const StableId &source) {
  std::vector<AnalysisFact> filtered;
  for (const auto &fact : facts) {
    if (fact.row.relation != relation || fact.row.cells.empty()) {
      continue;
    }
    const auto *id = std::get_if<StableId>(&fact.row.cells[0]);
    if (id != nullptr && *id == source) {
      filtered.push_back(fact);
    }
  }
  std::sort(filtered.begin(), filtered.end(),
            [](const AnalysisFact &a, const AnalysisFact &b) {
              return a.fact_id < b.fact_id;
            });
  return filtered;
}

// The executor borrows its engine, so the test owns both and keeps the engine
// alive for the executor's lifetime.
struct ExecutorHandle {
  std::unique_ptr<wpa::SouffleWpaExecutor> engine;
  std::unique_ptr<WpaQueryExecutor> executor;
};

std::optional<ExecutorHandle> MakeExecutor(const DependencySnapshot &snapshot) {
  auto engine = std::make_unique<wpa::SouffleWpaExecutor>(
      VERITAS_SOUFFLE_WORKER, std::string(kToolchain));
  auto executor =
      WpaQueryExecutor::Create(*engine, snapshot.summaries, snapshot.semantics,
                               ComputeDependencySnapshotDigest(snapshot));
  if (!executor.ok()) {
    return std::nullopt;
  }
  ExecutorHandle handle;
  handle.engine = std::move(engine);
  handle.executor = std::move(executor).value();
  return handle;
}

struct QueryRun {
  QueryPlan plan;
  QueryArtifact artifact;
};

std::optional<QueryRun> RunQuery(const DependencySnapshot &snapshot,
                                 const QueryDescriptor &descriptor) {
  auto plan = PlanQuery(descriptor, snapshot);
  EXPECT_TRUE(plan.ok()) << plan.status().message();
  if (!plan.ok()) {
    return std::nullopt;
  }
  auto handle = MakeExecutor(snapshot);
  EXPECT_TRUE(handle.has_value());
  if (!handle.has_value()) {
    return std::nullopt;
  }
  auto artifact = ExecuteQuery(*plan, *handle->executor);
  EXPECT_TRUE(artifact.ok()) << artifact.status().message();
  if (!artifact.ok()) {
    return std::nullopt;
  }
  return QueryRun{std::move(plan).value(), std::move(artifact).value()};
}

class ScriptedQueryExecutor final : public QueryExecutor {
public:
  enum class Mode {
    kTimeout,
    kWorkerDeath,
    kUnprovenResult,
    kMalformedRow,
  };

  ScriptedQueryExecutor(QueryExecutor &inner, Mode mode)
      : inner_(inner), mode_(mode) {}

  facts::EngineIdentity identity() const override { return inner_.identity(); }

  std::string_view toolchain_identity() const override {
    return inner_.toolchain_identity();
  }

  std::string_view snapshot_digest() const override {
    return inner_.snapshot_digest();
  }

  StatusOr<QueryKernelResult>
  Execute(const QueryKernelRequest &request,
          const wpa::WpaExecutionLimits &limits) const override {
    ++calls;
    switch (mode_) {
    case Mode::kTimeout:
      return Status::DeadlineExceeded("scripted query timeout");
    case Mode::kWorkerDeath:
      return Status::Internal("scripted worker death");
    case Mode::kUnprovenResult:
    case Mode::kMalformedRow:
      break;
    }
    auto result = inner_.Execute(request, limits);
    if (!result.ok()) {
      return result;
    }
    if (mode_ == Mode::kMalformedRow) {
      // A row that does not match its relation's schema. The canonicalizer
      // must refuse it rather than publish a malformed fact.
      SemanticRow malformed;
      malformed.relation = facts::RelationId::kReachableCall;
      malformed.cells = {Id(core::IdKind::kFunctionVariant, "only-one-cell")};
      result->evaluation.results.push_back(std::move(malformed));
      return result;
    }
    // A result the engine never derived and did not witness. The canonicalizer
    // must refuse to ground it.
    SemanticRow fabricated;
    fabricated.relation = facts::RelationId::kReachableCall;
    fabricated.cells = {Id(core::IdKind::kFunctionVariant, "b"),
                        Id(core::IdKind::kFunctionVariant, "a"),
                        analysis::semantic::EpistemicState::kMust};
    result->evaluation.results.push_back(std::move(fabricated));
    return result;
  }

  mutable std::size_t calls = 0;

private:
  QueryExecutor &inner_;
  Mode mode_;
};

TEST(QueryExecutionTest, ScopedAnswersMatchClosedBaseline) {
  auto a = Summary("a");
  AddCall(&a, "a", "b");
  auto b = Summary("b");
  AddCall(&b, "b", "c");
  const DependencySnapshot snapshot = Snapshot({a, b, Summary("c")});
  const auto descriptor =
      Descriptor(QueryPredicate::kReachableCall, {FunctionRef("a")});

  const Baseline baseline =
      RunBaseline(snapshot, QueryPredicate::kReachableCall);
  const auto expected =
      FilterBySourceRelation(baseline.facts, facts::RelationId::kReachableCall,
                             Id(core::IdKind::kFunctionVariant, "a"));
  ASSERT_FALSE(expected.empty()) << "the closed fixture must derive facts";

  auto run = RunQuery(snapshot, descriptor);
  ASSERT_TRUE(run.has_value());
  EXPECT_TRUE(IsCompleteAnswer(run->artifact));
  EXPECT_EQ(run->artifact.coverage.outcome, QueryOutcome::kComplete);
  EXPECT_TRUE(run->artifact.coverage.frontier.empty());

  // Exact fact/epistemic agreement, not merely equal counts.
  EXPECT_EQ(run->artifact.results, expected);

  // Exact proof-root agreement for the selected results.
  std::set<WitnessEdge> baseline_edges(baseline.witnesses.begin(),
                                       baseline.witnesses.end());
  std::set<WitnessEdge> query_edges(run->artifact.proof.begin(),
                                    run->artifact.proof.end());
  for (const auto &edge : query_edges) {
    EXPECT_TRUE(baseline_edges.contains(edge))
        << "query proof cites a derivation the baseline did not select";
  }
  for (const auto &fact : run->artifact.results) {
    bool saw_derivation = false;
    for (const auto &edge : baseline.witnesses) {
      if (edge.result.row == fact.row) {
        saw_derivation = true;
        EXPECT_TRUE(query_edges.contains(edge))
            << "query proof dropped a required premise";
      }
    }
    EXPECT_TRUE(saw_derivation);
  }
}

TEST(QueryExecutionTest, UnrelatedCodeDoesNotExpandFixedQuery) {
  auto a = Summary("a");
  AddCall(&a, "a", "b");
  auto b = Summary("b");
  AddCall(&b, "b", "c");
  std::vector<summary::SummaryArtifact> fixed = {a, b, Summary("c")};

  std::vector<summary::SummaryArtifact> expanded = fixed;
  for (int i = 0; i < 30; ++i) {
    expanded.push_back(Summary("unrelated" + std::to_string(i)));
  }

  const auto descriptor =
      Descriptor(QueryPredicate::kReachableCall, {FunctionRef("a")});
  auto small = RunQuery(Snapshot(fixed), descriptor);
  auto large = RunQuery(Snapshot(expanded), descriptor);
  ASSERT_TRUE(small.has_value());
  ASSERT_TRUE(large.has_value());

  EXPECT_EQ(small->plan.sccs.size(), large->plan.sccs.size());
  EXPECT_EQ(small->artifact.cost.kernels_executed,
            large->artifact.cost.kernels_executed);
  EXPECT_EQ(small->artifact.cost.worker_setup_count,
            large->artifact.cost.worker_setup_count);
  EXPECT_EQ(small->artifact.cost.materialized_rows,
            large->artifact.cost.materialized_rows);
  EXPECT_EQ(small->artifact.cost.successor_support_rows,
            large->artifact.cost.successor_support_rows);
  EXPECT_EQ(small->artifact.cost.boundary_rows,
            large->artifact.cost.boundary_rows);
  EXPECT_EQ(small->artifact.results, large->artifact.results);
  EXPECT_EQ(small->artifact.proof, large->artifact.proof);
  EXPECT_TRUE(small->artifact.coverage.predicate_supported);
  EXPECT_FALSE(small->artifact.cost.global_svf_bootstrap);
  EXPECT_FALSE(large->artifact.cost.global_svf_bootstrap);
}

TEST(QueryExecutionTest, RecursiveTimeoutIsIncomplete) {
  auto f = Summary("f");
  AddCall(&f, "f", "g");
  auto g = Summary("g");
  AddCall(&g, "g", "f");
  const DependencySnapshot snapshot = Snapshot({f, g});
  const auto descriptor =
      Descriptor(QueryPredicate::kReachableCall, {FunctionRef("f")});

  auto plan = PlanQuery(descriptor, snapshot);
  ASSERT_TRUE(plan.ok()) << plan.status().message();
  auto inner = MakeExecutor(snapshot);
  ASSERT_TRUE(inner.has_value());
  ScriptedQueryExecutor scripted(*inner->executor,
                                 ScriptedQueryExecutor::Mode::kTimeout);
  auto artifact = ExecuteQuery(*plan, scripted);
  ASSERT_TRUE(artifact.ok()) << artifact.status().message();

  EXPECT_EQ(scripted.calls, 1u);
  EXPECT_FALSE(IsCompleteAnswer(*artifact));
  EXPECT_EQ(artifact->coverage.outcome, QueryOutcome::kIncomplete);
  ASSERT_FALSE(artifact->coverage.frontier.empty());
  EXPECT_EQ(artifact->coverage.frontier.front().kind,
            QueryFrontierKind::kTimeout);
}

TEST(QueryExecutionTest, WorkerDeathDoesNotPublish) {
  auto f = Summary("f");
  AddCall(&f, "f", "g");
  const DependencySnapshot snapshot = Snapshot({f, Summary("g")});
  const auto descriptor =
      Descriptor(QueryPredicate::kReachableCall, {FunctionRef("f")});

  auto plan = PlanQuery(descriptor, snapshot);
  ASSERT_TRUE(plan.ok()) << plan.status().message();
  auto inner = MakeExecutor(snapshot);
  ASSERT_TRUE(inner.has_value());
  ScriptedQueryExecutor scripted(*inner->executor,
                                 ScriptedQueryExecutor::Mode::kWorkerDeath);
  auto artifact = ExecuteQuery(*plan, scripted);

  EXPECT_FALSE(artifact.ok());
  EXPECT_EQ(artifact.status().code(), StatusCode::kInternal);
  EXPECT_EQ(scripted.calls, 1u);
}

TEST(QueryExecutionTest, MissingPremisePreventsSeal) {
  auto f = Summary("f");
  AddCall(&f, "f", "g");
  const DependencySnapshot snapshot = Snapshot({f, Summary("g")});
  const auto descriptor =
      Descriptor(QueryPredicate::kReachableCall, {FunctionRef("f")});

  auto plan = PlanQuery(descriptor, snapshot);
  ASSERT_TRUE(plan.ok()) << plan.status().message();
  auto inner = MakeExecutor(snapshot);
  ASSERT_TRUE(inner.has_value());
  ScriptedQueryExecutor scripted(*inner->executor,
                                 ScriptedQueryExecutor::Mode::kUnprovenResult);
  auto artifact = ExecuteQuery(*plan, scripted);
  ASSERT_TRUE(artifact.ok()) << artifact.status().message();

  EXPECT_FALSE(IsCompleteAnswer(*artifact));
  EXPECT_EQ(artifact->coverage.outcome, QueryOutcome::kIncomplete);
  bool saw_missing_premise = false;
  for (const auto &entry : artifact->coverage.frontier) {
    if (entry.kind == QueryFrontierKind::kMissingPremise) {
      saw_missing_premise = true;
    }
  }
  EXPECT_TRUE(saw_missing_premise);
  // The fabricated result is never returned as an answer.
  for (const auto &fact : artifact->results) {
    if (fact.row.relation != facts::RelationId::kReachableCall ||
        fact.row.cells.size() != 3) {
      continue;
    }
    const auto *source = std::get_if<StableId>(&fact.row.cells[0]);
    const auto *target = std::get_if<StableId>(&fact.row.cells[1]);
    EXPECT_FALSE(source != nullptr && target != nullptr &&
                 *source == Id(core::IdKind::kFunctionVariant, "b") &&
                 *target == Id(core::IdKind::kFunctionVariant, "a"));
  }
}

std::optional<AnalysisFact>
FindReachableFact(const std::vector<AnalysisFact> &facts,
                  const StableId &source, const StableId &target) {
  for (const auto &fact : facts) {
    if (fact.row.relation != facts::RelationId::kReachableCall ||
        fact.row.cells.size() < 2) {
      continue;
    }
    const auto *s = std::get_if<StableId>(&fact.row.cells[0]);
    const auto *t = std::get_if<StableId>(&fact.row.cells[1]);
    if (s != nullptr && t != nullptr && *s == source && *t == target) {
      return fact;
    }
  }
  return std::nullopt;
}

TEST(QueryExecutionTest, ClosedGlobalFlowMatchesBaseline) {
  auto f = Summary("f");
  AddValueFlow(&f, "v1", "v2");
  const DependencySnapshot snapshot = Snapshot({f});
  const auto descriptor =
      Descriptor(QueryPredicate::kGlobalFlow, {ValueRef("v1", "f")},
                 {ValueRef("v2", "f")});

  const Baseline baseline = RunBaseline(snapshot, QueryPredicate::kGlobalFlow);
  const auto expected =
      FilterBySourceRelation(baseline.facts, facts::RelationId::kGlobalFlow,
                             Id(core::IdKind::kValueRef, "v1"));
  ASSERT_FALSE(expected.empty()) << "the closed fixture must derive a flow";

  auto run = RunQuery(snapshot, descriptor);
  ASSERT_TRUE(run.has_value());
  EXPECT_TRUE(IsCompleteAnswer(run->artifact));
  EXPECT_EQ(run->artifact.results, expected);
}

TEST(QueryExecutionTest, ClosedMayReadMatchesBaseline) {
  auto f = Summary("f");
  AddMemoryEffect(&f, "m", false);
  const DependencySnapshot snapshot = Snapshot({f});
  const auto descriptor = Descriptor(QueryPredicate::kMayRead,
                                     {FunctionRef("f")}, {MemoryRef("m", "f")});

  const Baseline baseline = RunBaseline(snapshot, QueryPredicate::kMayRead);
  const auto expected =
      FilterBySourceRelation(baseline.facts, facts::RelationId::kMayRead,
                             Id(core::IdKind::kFunctionVariant, "f"));
  ASSERT_FALSE(expected.empty()) << "the closed fixture must derive a read";

  auto run = RunQuery(snapshot, descriptor);
  ASSERT_TRUE(run.has_value());
  EXPECT_TRUE(IsCompleteAnswer(run->artifact));
  EXPECT_EQ(run->artifact.results, expected);
}

TEST(QueryExecutionTest, ClosedMayWriteMatchesBaseline) {
  auto f = Summary("f");
  AddMemoryEffect(&f, "m", true);
  const DependencySnapshot snapshot = Snapshot({f});
  const auto descriptor = Descriptor(QueryPredicate::kMayWrite,
                                     {FunctionRef("f")}, {MemoryRef("m", "f")});

  const Baseline baseline = RunBaseline(snapshot, QueryPredicate::kMayWrite);
  const auto expected =
      FilterBySourceRelation(baseline.facts, facts::RelationId::kMayWrite,
                             Id(core::IdKind::kFunctionVariant, "f"));
  ASSERT_FALSE(expected.empty()) << "the closed fixture must derive a write";

  auto run = RunQuery(snapshot, descriptor);
  ASSERT_TRUE(run.has_value());
  EXPECT_TRUE(IsCompleteAnswer(run->artifact));
  EXPECT_EQ(run->artifact.results, expected);
}

TEST(QueryExecutionTest, SharedMemoryReadMatchesBaselineButStaysOpen) {
  auto f = Summary("f");
  AddMemoryEffect(&f, "m1", false);
  AddAlias(&f, "m1", "m2", v2::ALIAS_KIND_MAY_ALIAS);
  const DependencySnapshot snapshot = Snapshot({f});
  const auto descriptor = Descriptor(
      QueryPredicate::kMayRead, {FunctionRef("f")}, {MemoryRef("m1", "f")});

  const Baseline baseline = RunBaseline(snapshot, QueryPredicate::kMayRead);
  const auto expected =
      FilterBySourceRelation(baseline.facts, facts::RelationId::kMayRead,
                             Id(core::IdKind::kFunctionVariant, "f"));
  ASSERT_FALSE(expected.empty());

  auto run = RunQuery(snapshot, descriptor);
  ASSERT_TRUE(run.has_value());
  EXPECT_EQ(run->artifact.results, expected);
  // A possible alias keeps the memory scope open even though the direct read
  // is derived: the answer is inspectable, never a closed-world claim.
  EXPECT_FALSE(IsCompleteAnswer(run->artifact));
  bool saw_shared = false;
  for (const auto &entry : run->artifact.coverage.frontier) {
    if (entry.kind == QueryFrontierKind::kSharedMemory) {
      saw_shared = true;
    }
  }
  EXPECT_TRUE(saw_shared);
}

TEST(QueryExecutionTest, UnknownEffectMatchesBaselineButStaysOpen) {
  auto f = Summary("f");
  f.add_unknowns()->set_kind("indirect-call");
  const DependencySnapshot snapshot = Snapshot({f});
  const auto descriptor =
      Descriptor(QueryPredicate::kUnknownEffect, {FunctionRef("f")});

  const Baseline baseline =
      RunBaseline(snapshot, QueryPredicate::kUnknownEffect);
  const auto expected =
      FilterBySourceRelation(baseline.facts, facts::RelationId::kUnknownEffect,
                             Id(core::IdKind::kFunctionVariant, "f"));
  ASSERT_FALSE(expected.empty()) << "the unknown must surface as a typed fact";

  auto run = RunQuery(snapshot, descriptor);
  ASSERT_TRUE(run.has_value());
  EXPECT_EQ(run->artifact.results, expected);
  // An explicit unknown keeps the scope open even though the typed effect is
  // derived: the answer is inspectable, never a closed-world claim.
  EXPECT_FALSE(IsCompleteAnswer(run->artifact));
  bool saw_unknown = false;
  for (const auto &entry : run->artifact.coverage.frontier) {
    if (entry.kind == QueryFrontierKind::kUnknownEffect) {
      saw_unknown = true;
    }
  }
  EXPECT_TRUE(saw_unknown);
}

TEST(QueryExecutionTest, BoundaryFactAnswersACoveredExternalCall) {
  auto f = Summary("f");
  const auto site = Id(core::IdKind::kCallSite, "f-external");
  AddOpenExternalCall(&f, "f-external");

  auto declared = MakeReachableFact("f", "external-target");
  ASSERT_TRUE(declared.has_value());
  const BoundarySummary boundary = MakeBoundary("f", {*declared}, {site});
  auto boundary_id = ComputeBoundarySummaryId(boundary);
  ASSERT_TRUE(boundary_id.ok()) << boundary_id.status().message();

  DependencySnapshot snapshot = Snapshot({f});
  snapshot.boundary_summaries.push_back(boundary);
  auto descriptor =
      Descriptor(QueryPredicate::kReachableCall, {FunctionRef("f")});
  descriptor.boundary_summaries.push_back(*boundary_id);

  auto run = RunQuery(snapshot, descriptor);
  ASSERT_TRUE(run.has_value());
  EXPECT_TRUE(run->plan.scope_closed);
  EXPECT_TRUE(IsCompleteAnswer(run->artifact));
  ASSERT_EQ(run->artifact.results.size(), 1u);
  EXPECT_EQ(run->artifact.results.front(), *declared);
  EXPECT_EQ(run->artifact.cost.boundary_rows, 1u);

  // The artifact names the exact boundary contract that warrants the fact.
  const std::string link = "boundary:" + boundary_id->value() +
                           ":fact:" + core::ToString(declared->fact_id);
  EXPECT_NE(std::find(run->artifact.coverage.assumptions.begin(),
                      run->artifact.coverage.assumptions.end(), link),
            run->artifact.coverage.assumptions.end());
}

TEST(QueryExecutionTest, BoundarySupportForExcludedSuccessorMatchesBaseline) {
  auto a = Summary("a");
  AddCall(&a, "a", "b1");
  auto b1 = Summary("b1");
  AddCall(&b1, "b1", "b2");
  auto b2 = Summary("b2");
  AddCall(&b2, "b2", "b1");
  AddCall(&b2, "b2", "d");
  const DependencySnapshot snapshot = Snapshot({a, b1, b2, Summary("d")});

  auto b1_to_d = MakeReachableFact("b1", "d");
  auto b2_to_d = MakeReachableFact("b2", "d");
  ASSERT_TRUE(b1_to_d.has_value());
  ASSERT_TRUE(b2_to_d.has_value());
  const BoundarySummary b1_boundary = MakeBoundary("b1", {*b1_to_d});
  const BoundarySummary b2_boundary = MakeBoundary("b2", {*b2_to_d});
  auto b1_id = ComputeBoundarySummaryId(b1_boundary);
  auto b2_id = ComputeBoundarySummaryId(b2_boundary);
  ASSERT_TRUE(b1_id.ok());
  ASSERT_TRUE(b2_id.ok());

  const auto a_id = Id(core::IdKind::kFunctionVariant, "a");
  const auto d_id = Id(core::IdKind::kFunctionVariant, "d");

  DependencySnapshot covered = snapshot;
  covered.boundary_summaries = {b1_boundary, b2_boundary};
  auto descriptor =
      Descriptor(QueryPredicate::kReachableCall, {FunctionRef("a")});
  // The recursive {b1, b2} SCC is oversized, so it is excluded whole. The
  // boundary summaries stand in for it and must reach the admitted caller.
  descriptor.limits.max_functions = 1;
  descriptor.boundary_summaries = {*b1_id, *b2_id};

  auto run = RunQuery(covered, descriptor);
  ASSERT_TRUE(run.has_value());
  EXPECT_FALSE(IsCompleteAnswer(run->artifact));
  bool saw_oversized = false;
  for (const auto &entry : run->artifact.coverage.frontier) {
    if (entry.kind == QueryFrontierKind::kOversizedScc) {
      saw_oversized = true;
    }
  }
  EXPECT_TRUE(saw_oversized);

  const auto query_a_d = FindReachableFact(run->artifact.results, a_id, d_id);
  ASSERT_TRUE(query_a_d.has_value())
      << "boundary support must reach the admitted caller";

  // The boundary-supported fact is exactly the baseline fact.
  const Baseline baseline =
      RunBaseline(snapshot, QueryPredicate::kReachableCall);
  const auto baseline_a_d = FindReachableFact(baseline.facts, a_id, d_id);
  ASSERT_TRUE(baseline_a_d.has_value());
  EXPECT_EQ(*query_a_d, *baseline_a_d);

  // Without the boundary summaries the same query cannot derive it: the
  // boundary, not the engine, supplies the excluded successor's facts.
  auto bare_descriptor = descriptor;
  bare_descriptor.boundary_summaries.clear();
  auto bare = RunQuery(snapshot, bare_descriptor);
  ASSERT_TRUE(bare.has_value());
  EXPECT_FALSE(
      FindReachableFact(bare->artifact.results, a_id, d_id).has_value());
}

TEST(QueryExecutionTest, ProofBudgetDropsWholeResults) {
  auto a = Summary("a");
  AddCall(&a, "a", "b");
  auto b = Summary("b");
  AddCall(&b, "b", "c");
  auto c = Summary("c");
  AddCall(&c, "c", "d");
  const DependencySnapshot snapshot = Snapshot({a, b, c, Summary("d")});

  auto full_descriptor =
      Descriptor(QueryPredicate::kReachableCall, {FunctionRef("a")});
  auto full = RunQuery(snapshot, full_descriptor);
  ASSERT_TRUE(full.has_value());
  ASSERT_GE(full->artifact.results.size(), 2u);

  auto limited_descriptor = full_descriptor;
  limited_descriptor.limits.max_proof_edges = 1;
  auto limited = RunQuery(snapshot, limited_descriptor);
  ASSERT_TRUE(limited.has_value());

  EXPECT_FALSE(IsCompleteAnswer(limited->artifact));
  EXPECT_TRUE(limited->artifact.coverage.proof_truncated);
  EXPECT_LE(limited->artifact.coverage.proof_edges, 1u);
  EXPECT_LT(limited->artifact.results.size(), full->artifact.results.size());
  bool saw_budget = false;
  for (const auto &entry : limited->artifact.coverage.frontier) {
    if (entry.kind == QueryFrontierKind::kProofBudget) {
      saw_budget = true;
    }
  }
  EXPECT_TRUE(saw_budget);

  // Omitted, never truncated: every returned result is a complete answer the
  // unbudgeted run also returns.
  for (const auto &fact : limited->artifact.results) {
    EXPECT_NE(std::find(full->artifact.results.begin(),
                        full->artifact.results.end(), fact),
              full->artifact.results.end());
  }
}

TEST(QueryExecutionTest, RecordsWorkerAndGlobalSvfSetup) {
  auto a = Summary("a");
  AddCall(&a, "a", "b");
  const DependencySnapshot snapshot = Snapshot({a, Summary("b")});
  const auto descriptor =
      Descriptor(QueryPredicate::kReachableCall, {FunctionRef("a")});

  auto run = RunQuery(snapshot, descriptor);
  ASSERT_TRUE(run.has_value());
  const auto &cost = run->artifact.cost;
  EXPECT_GT(cost.kernels_executed, 0u);
  EXPECT_EQ(cost.worker_setup_count, cost.kernels_executed);
  EXPECT_EQ(cost.materialized_rows, run->artifact.coverage.examined_rows);
  EXPECT_EQ(run->artifact.coverage.proof_edges, run->artifact.proof.size());
  EXPECT_FALSE(cost.global_svf_bootstrap);
  EXPECT_EQ(cost.global_svf_bootstrap_identity, "not-run:local-summary-input");
}

TEST(QueryExecutionTest, SummaryOrderDoesNotChangeTheArtifact) {
  auto a = Summary("a");
  AddCall(&a, "a", "b");
  auto b = Summary("b");
  AddCall(&b, "b", "c");
  const std::vector<summary::SummaryArtifact> ordered = {a, b, Summary("c")};
  const std::vector<summary::SummaryArtifact> reversed(ordered.rbegin(),
                                                       ordered.rend());
  const auto descriptor =
      Descriptor(QueryPredicate::kReachableCall, {FunctionRef("a")});

  auto first = RunQuery(Snapshot(ordered), descriptor);
  auto second = RunQuery(Snapshot(reversed), descriptor);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(first->artifact.results, second->artifact.results);
  EXPECT_EQ(first->artifact.proof, second->artifact.proof);
  EXPECT_EQ(first->artifact.artifact_id, second->artifact.artifact_id);
}

TEST(QueryExecutionTest, MalformedKernelOutputPublishesNothing) {
  auto f = Summary("f");
  AddCall(&f, "f", "g");
  const DependencySnapshot snapshot = Snapshot({f, Summary("g")});
  const auto descriptor =
      Descriptor(QueryPredicate::kReachableCall, {FunctionRef("f")});

  auto plan = PlanQuery(descriptor, snapshot);
  ASSERT_TRUE(plan.ok()) << plan.status().message();
  auto inner = MakeExecutor(snapshot);
  ASSERT_TRUE(inner.has_value());
  ScriptedQueryExecutor scripted(*inner->executor,
                                 ScriptedQueryExecutor::Mode::kMalformedRow);
  auto artifact = ExecuteQuery(*plan, scripted);

  EXPECT_FALSE(artifact.ok());
  EXPECT_EQ(artifact.status().code(), StatusCode::kInvalidArgument);
}

} // namespace
} // namespace veritas::query
