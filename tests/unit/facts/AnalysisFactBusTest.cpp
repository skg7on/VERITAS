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

#include "veritas/facts/AnalysisFactBus.h"

#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>

#include "veritas/facts/AnalysisFact.h"
#include "veritas/facts/AnalysisRun.h"
#include "veritas/facts/Witness.h"
#include "veritas/wpa/WpaOrchestrator.h"
#include "veritas/wpa/WpaRunRepository.h"

namespace veritas::facts {
namespace {

namespace sem = analysis::semantic;

static_assert(std::is_same_v<
              decltype(&AnalysisFactBus::Publish),
              Status (AnalysisFactBus::*)(const AnalysisFactBatch &) const>);

constexpr std::string_view kDirect = "wpa.reachability.direct.v2";
constexpr std::string_view kFlowParameter = "wpa.flow.global.parameter.v2";

core::StableId FunctionId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kFunctionVariant,
                            std::as_bytes(std::span(name.data(), name.size())));
}

core::StableId CallSiteId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kCallSite,
                            std::as_bytes(std::span(name.data(), name.size())));
}

core::StableId ValueId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kValueRef,
                            std::as_bytes(std::span(name.data(), name.size())));
}

SemanticRow Reachable(std::string_view from, std::string_view to) {
  return SemanticRow{
      RelationId::kReachableCall,
      {FunctionId(from), FunctionId(to), sem::EpistemicState::kMay}};
}

SemanticRow DirectCall(std::string_view from, std::string_view to) {
  return SemanticRow{RelationId::kDirectCall,
                     {CallSiteId(std::string(from) + "->" + std::string(to)),
                      FunctionId(from), FunctionId(to),
                      sem::DispatchKind::kDirect, sem::EpistemicState::kMay}};
}

WitnessEdge Edge(const SemanticRow &result, std::string_view rule,
                 const SemanticRow &input, std::uint32_t ordinal) {
  return WitnessEdge{.result = SemanticKey{result},
                     .rule_id = std::string(rule),
                     .input = SemanticKey{input},
                     .input_ordinal = ordinal};
}

// A parameter flow binds `actual` to `formal` at one call site.
SemanticRow ParameterFlow(std::string_view site, std::string_view actual,
                          std::string_view formal) {
  return SemanticRow{RelationId::kParameterFlow,
                     {CallSiteId(site), ValueId(actual), ValueId(formal),
                      sem::EpistemicState::kMust}};
}

// The global flow a parameter flow is promoted to. The call site is projected
// away, so two call sites sharing an actual and a formal promote to one row.
SemanticRow GlobalFlow(std::string_view from, std::string_view to) {
  return SemanticRow{RelationId::kGlobalFlow,
                     {ValueId(from), ValueId(to), sem::EpistemicState::kMust}};
}

AnalysisRunManifest TestRun() {
  AnalysisRunDescriptor descriptor;
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
  descriptor.engine = EngineIdentity::kSouffle;
  descriptor.engine_toolchain_identity = "test-toolchain";
  return std::move(MakeAnalysisRun(descriptor)).value();
}

std::filesystem::path TempDbPath() {
  std::string tmpl =
      (std::filesystem::temp_directory_path() / "veritas-factbus-XXXXXX")
          .string();
  char *made = ::mkdtemp(tmpl.data());
  return std::filesystem::path(made);
}

// A valid batch: one component, one rooted input (the direct call), one derived
// fact (the reachable call) proved by one witness edge.
AnalysisFactBatch SuccessfulBatch() {
  AnalysisFactBatch batch;
  batch.run = TestRun();

  wpa::WpaComponentKey key{FunctionId("scc"),
                           wpa::WpaComponentKind::kReachability};
  batch.expected_components = {key};

  wpa::WpaComponentCompletion completion;
  completion.key = key;
  completion.result_object_key = "result-object";
  completion.result.scc_id = key.scc_id;
  completion.result.component = key.component;
  completion.result.logical_input_hash = "logical";
  completion.result.fixpoint_hash = "fixpoint";
  completion.result.external_hash = "external";
  batch.completed_components = {completion};

  const auto root = MakeFact(DirectCall("f", "g")).value();
  const auto derived = MakeFact(Reachable("f", "g")).value();
  batch.rooted_input_fact_ids = {root.fact_id};
  batch.facts = {derived};
  batch.witnesses = {
      Edge(Reachable("f", "g"), kDirect, DirectCall("f", "g"), 0)};
  batch.batch_id = DeriveBatchId(batch);
  return batch;
}

wpa::WpaRunResult DuplicateProofRun() {
  const SemanticRow shared_flow = GlobalFlow("r", "p");
  const SemanticRow root_a = ParameterFlow("cs:a", "r", "p");
  const SemanticRow root_b = ParameterFlow("cs:b", "r", "p");
  const auto root_fact_a = MakeFact(root_a).value();
  const auto root_fact_b = MakeFact(root_b).value();

  auto component = [](std::string_view scc, const SemanticRow &root,
                      const SemanticRow &flow) {
    wpa::WpaComponentCompletion completion;
    completion.key =
        wpa::WpaComponentKey{FunctionId(scc), wpa::WpaComponentKind::kFlow};
    completion.result.scc_id = completion.key.scc_id;
    completion.result.component = completion.key.component;
    completion.result.logical_input_hash = "logical";
    completion.result.fixpoint_hash = "fixpoint";
    completion.result.external_hash = "external";
    completion.result.facts = {MakeFact(flow).value()};
    completion.result.witnesses = {Edge(flow, kFlowParameter, root, 0)};
    return completion;
  };
  const auto completion_a = component("scc:a", root_a, shared_flow);
  const auto completion_b = component("scc:b", root_b, shared_flow);

  wpa::WpaRunResult run;
  run.run = TestRun();
  run.expected_components = {completion_a.key, completion_b.key};
  run.completed_components = {completion_a, completion_b};
  run.rooted_input_fact_ids = {root_fact_a.fact_id, root_fact_b.fact_id};
  run.rooted_input_facts = {RootedInputFact{.fact = root_fact_a},
                            RootedInputFact{.fact = root_fact_b}};
  return run;
}

class RecordingSink : public AnalysisFactSink {
public:
  Status Publish(const AnalysisFactBatch &batch) override {
    batches_.push_back(batch);
    ++counts_[core::ToString(batch.batch_id)];
    return Status::Ok();
  }

  const std::vector<AnalysisFactBatch> &batches() const { return batches_; }
  std::size_t logical_publication_count(const core::StableId &batch_id) const {
    const auto it = counts_.find(core::ToString(batch_id));
    return it == counts_.end() ? 0 : it->second;
  }

private:
  std::vector<AnalysisFactBatch> batches_;
  std::map<std::string, std::size_t> counts_;
};

class FailOnceSink : public AnalysisFactSink {
public:
  Status Publish(const AnalysisFactBatch &batch) override {
    if (!failed_once_) {
      failed_once_ = true;
      return Status::Internal("injected fan-out failure");
    }
    ++counts_[core::ToString(batch.batch_id)];
    return Status::Ok();
  }

  std::size_t logical_publication_count(const core::StableId &batch_id) const {
    const auto it = counts_.find(core::ToString(batch_id));
    return it == counts_.end() ? 0 : it->second;
  }

private:
  bool failed_once_ = false;
  std::map<std::string, std::size_t> counts_;
};

TEST(AnalysisFactBusTest, DeliversOneValidatedImmutableBatch) {
  const auto db = TempDbPath();
  auto repo = wpa::WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok()) << repo.status().message();

  RecordingSink sink;
  AnalysisFactBus bus(*repo);
  bus.AddSink("recording", sink);

  const auto batch = SuccessfulBatch();
  ASSERT_TRUE(bus.Publish(batch).ok());
  ASSERT_EQ(sink.batches().size(), 1u);
  EXPECT_EQ(sink.batches()[0].run.run_id, batch.run.run_id);

  std::filesystem::remove_all(db);
}

// Two sibling callers each bind the same callee-owned value to the same formal,
// at their own call sites. `GlobalFlow` drops the call site, so both components
// derive the identical fact from their own `ParameterFlow` row and the
// flattened run carries it twice. A run's provenance records one proof per
// fact, so the assembled batch keeps one fact and one whole derivation -- not a
// mixture of the two proofs, which would bind a single-input rule at two
// ordinals.
TEST(AnalysisFactBusTest, CoalescesAFactProvenByTwoComponents) {
  const auto db = TempDbPath();
  auto repo = wpa::WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok()) << repo.status().message();
  AnalysisFactBus bus(*repo);

  wpa::WpaRunResult run = DuplicateProofRun();
  const auto &completion_a = run.completed_components[0];
  const auto &completion_b = run.completed_components[1];

  // Ownership goes to the first component in canonical key order, so the
  // surviving proof is whichever component's key sorts first -- and it must be
  // that component's whole derivation, root and all.
  const bool first_is_a = completion_a.key < completion_b.key;
  const SemanticRow &surviving_root =
      first_is_a ? completion_a.result.witnesses[0].input.row
                 : completion_b.result.witnesses[0].input.row;

  const AnalysisFactBatch batch = MakeAnalysisFactBatch(run);
  ASSERT_EQ(batch.facts.size(), 1u);
  ASSERT_EQ(batch.witnesses.size(), 1u);
  EXPECT_EQ(batch.witnesses[0].input.row, surviving_root);
  EXPECT_TRUE(bus.Publish(batch).ok());

  // Ownership follows the sorted component keys, not the order the orchestrator
  // happened to complete them in, so reversing the completion order changes
  // nothing at all -- not even the batch identity.
  wpa::WpaRunResult reversed = run;
  reversed.completed_components = {completion_b, completion_a};
  const AnalysisFactBatch reversed_batch = MakeAnalysisFactBatch(reversed);
  ASSERT_EQ(reversed_batch.witnesses.size(), 1u);
  EXPECT_EQ(reversed_batch.witnesses[0].input.row, surviving_root);
  EXPECT_EQ(reversed_batch.batch_id, batch.batch_id);
  EXPECT_TRUE(bus.Publish(reversed_batch).ok());

  std::filesystem::remove_all(db);
}

TEST(AnalysisFactBusTest, ConsumesComponentPayloadIntoCanonicalBatchVectors) {
  auto run = DuplicateProofRun();
  const auto expected = MakeAnalysisFactBatch(run);
  auto consumed = MakeAnalysisFactBatch(std::move(run));

  EXPECT_EQ(consumed.batch_id, expected.batch_id);
  EXPECT_EQ(consumed.facts, expected.facts);
  EXPECT_EQ(consumed.witnesses, expected.witnesses);
  ASSERT_FALSE(consumed.completed_components.empty());
  for (const auto &completion : consumed.completed_components) {
    EXPECT_TRUE(completion.result.facts.empty());
    EXPECT_TRUE(completion.result.witnesses.empty());
    EXPECT_TRUE(completion.result.diagnostics.empty());
    EXPECT_FALSE(completion.result.logical_input_hash.empty());
    EXPECT_FALSE(completion.result.fixpoint_hash.empty());
    EXPECT_FALSE(completion.result.external_hash.empty());
  }
}

TEST(AnalysisFactBusTest, RejectsIncompleteOrMixedRunBatch) {
  const auto db = TempDbPath();
  auto repo = wpa::WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok()) << repo.status().message();
  AnalysisFactBus bus(*repo);

  // Missing a completed component for the expected set.
  auto incomplete = SuccessfulBatch();
  incomplete.completed_components.clear();
  EXPECT_FALSE(bus.Publish(incomplete).ok());

  // A fact whose identity does not match its semantic row (mixed identity).
  auto mixed = SuccessfulBatch();
  mixed.facts[0].fact_id = FunctionId("not-the-fact");
  EXPECT_FALSE(bus.Publish(mixed).ok());

  std::filesystem::remove_all(db);
}

TEST(AnalysisFactBusTest, RejectsFactWithoutClosedWitness) {
  const auto db = TempDbPath();
  auto repo = wpa::WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok()) << repo.status().message();
  AnalysisFactBus bus(*repo);

  auto orphan = SuccessfulBatch();
  orphan.witnesses.clear();
  EXPECT_EQ(bus.Publish(std::move(orphan)).code(),
            StatusCode::kFailedPrecondition);

  std::filesystem::remove_all(db);
}

TEST(AnalysisFactBusTest, RejectsClosedSubsetWithMissingComponent) {
  const auto db = TempDbPath();
  auto repo = wpa::WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok()) << repo.status().message();
  AnalysisFactBus bus(*repo);

  auto batch = SuccessfulBatch();
  batch.completed_components.pop_back();
  EXPECT_EQ(bus.Publish(std::move(batch)).code(),
            StatusCode::kFailedPrecondition);

  std::filesystem::remove_all(db);
}

TEST(AnalysisFactBusTest, RejectsWitnessLeafOutsideRootSet) {
  const auto db = TempDbPath();
  auto repo = wpa::WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok()) << repo.status().message();
  AnalysisFactBus bus(*repo);

  auto undeclared = SuccessfulBatch();
  undeclared.rooted_input_fact_ids.clear();
  // The witness still cites the direct call, which is now neither a published
  // fact nor a declared rooted input.
  EXPECT_EQ(bus.Publish(std::move(undeclared)).code(),
            StatusCode::kFailedPrecondition);

  std::filesystem::remove_all(db);
}

TEST(AnalysisFactBusTest, RejectsCyclicWitnessDag) {
  const auto db = TempDbPath();
  auto repo = wpa::WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok()) << repo.status().message();
  AnalysisFactBus bus(*repo);

  auto cycle = SuccessfulBatch();
  const SemanticRow forward = Reachable("f", "g");
  const SemanticRow reverse = Reachable("g", "f");
  cycle.facts = {MakeFact(forward).value(), MakeFact(reverse).value()};
  cycle.rooted_input_fact_ids.clear();
  cycle.witnesses = {Edge(forward, kDirect, reverse, 0),
                     Edge(reverse, kDirect, forward, 0)};
  cycle.batch_id = DeriveBatchId(cycle);

  const Status status = bus.Publish(cycle);
  EXPECT_EQ(status.code(), StatusCode::kFailedPrecondition);
  EXPECT_EQ(status.message(), "witness DAG contains a cycle");

  std::filesystem::remove_all(db);
}

TEST(AnalysisFactBusTest, RetryAfterPartialFanoutIsIdempotent) {
  const auto db = TempDbPath();
  auto repo = wpa::WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok()) << repo.status().message();

  RecordingSink first;
  FailOnceSink second;
  AnalysisFactBus bus(*repo);
  bus.AddSink("first", first);
  bus.AddSink("second", second);

  const auto batch = SuccessfulBatch();
  EXPECT_FALSE(bus.Publish(batch).ok());
  EXPECT_TRUE(bus.Publish(batch).ok());
  EXPECT_EQ(first.logical_publication_count(batch.batch_id), 1u);
  EXPECT_EQ(second.logical_publication_count(batch.batch_id), 1u);

  std::filesystem::remove_all(db);
}

} // namespace
} // namespace veritas::facts
