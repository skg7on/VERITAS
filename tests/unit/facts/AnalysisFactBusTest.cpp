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
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>

#include "veritas/facts/AnalysisFact.h"
#include "veritas/facts/AnalysisRun.h"
#include "veritas/facts/Witness.h"
#include "veritas/wpa/WpaRunRepository.h"

namespace veritas::facts {
namespace {

namespace sem = analysis::semantic;

constexpr std::string_view kDirect = "wpa.reachability.direct.v2";

core::StableId FunctionId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kFunctionVariant,
                            std::as_bytes(std::span(name.data(), name.size())));
}

core::StableId CallSiteId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kCallSite,
                            std::as_bytes(std::span(name.data(), name.size())));
}

core::StableId BatchId(std::string_view text) {
  return core::MakeStableId(core::IdKind::kFact,
                            std::as_bytes(std::span(text.data(), text.size())));
}

SemanticRow Reachable(std::string_view from, std::string_view to) {
  return SemanticRow{RelationId::kReachableCall,
                     {FunctionId(from), FunctionId(to), sem::EpistemicState::kMay}};
}

SemanticRow DirectCall(std::string_view from, std::string_view to) {
  return SemanticRow{RelationId::kDirectCall,
                     {CallSiteId(std::string(from) + "->" + std::string(to)),
                      FunctionId(from), FunctionId(to),
                      sem::DispatchKind::kDirect, sem::EpistemicState::kMay}};
}

WitnessEdge Edge(const SemanticRow& result, std::string_view rule,
                 const SemanticRow& input, std::uint32_t ordinal) {
  return WitnessEdge{.result = SemanticKey{result},
                     .rule_id = std::string(rule),
                     .input = SemanticKey{input},
                     .input_ordinal = ordinal};
}

AnalysisRunManifest TestRun() {
  AnalysisRunDescriptor descriptor;
  descriptor.revision_id =
      core::MakeStableId(core::IdKind::kRevision, std::as_bytes(std::span("rev", 3)));
  descriptor.build_variant_id =
      core::MakeStableId(core::IdKind::kBuildVariant, std::as_bytes(std::span("bv", 2)));
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
      (std::filesystem::temp_directory_path() / "veritas-factbus-XXXXXX").string();
  char* made = ::mkdtemp(tmpl.data());
  return std::filesystem::path(made);
}

// A valid batch: one component, one rooted input (the direct call), one derived
// fact (the reachable call) proved by one witness edge.
AnalysisFactBatch SuccessfulBatch() {
  AnalysisFactBatch batch;
  batch.run = TestRun();
  batch.batch_id = BatchId("test-batch");

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
  batch.witnesses = {Edge(Reachable("f", "g"), kDirect, DirectCall("f", "g"), 0)};
  return batch;
}

class RecordingSink : public AnalysisFactSink {
 public:
  Status Publish(const AnalysisFactBatch& batch) override {
    batches_.push_back(batch);
    ++counts_[core::ToString(batch.batch_id)];
    return Status::Ok();
  }

  const std::vector<AnalysisFactBatch>& batches() const { return batches_; }
  std::size_t logical_publication_count(const core::StableId& batch_id) const {
    const auto it = counts_.find(core::ToString(batch_id));
    return it == counts_.end() ? 0 : it->second;
  }

 private:
  std::vector<AnalysisFactBatch> batches_;
  std::map<std::string, std::size_t> counts_;
};

class FailOnceSink : public AnalysisFactSink {
 public:
  Status Publish(const AnalysisFactBatch& batch) override {
    if (!failed_once_) {
      failed_once_ = true;
      return Status::Internal("injected fan-out failure");
    }
    ++counts_[core::ToString(batch.batch_id)];
    return Status::Ok();
  }

  std::size_t logical_publication_count(const core::StableId& batch_id) const {
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

}  // namespace
}  // namespace veritas::facts
