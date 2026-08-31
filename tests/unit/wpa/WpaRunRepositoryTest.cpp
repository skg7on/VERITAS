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

#include "veritas/wpa/WpaRunRepository.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>

#include "veritas/facts/AnalysisRun.h"
#include "veritas/facts/Witness.h"

namespace veritas::wpa {
namespace {

namespace sem = analysis::semantic;

core::StableId FunctionId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kFunctionVariant,
                            std::as_bytes(std::span(name.data(), name.size())));
}

facts::AnalysisRunManifest MakeManifest(facts::EngineIdentity engine) {
  facts::AnalysisRunDescriptor d;
  d.revision_id = core::MakeStableId(core::IdKind::kRevision,
                                     std::as_bytes(std::span("rev", 3)));
  d.build_variant_id = core::MakeStableId(core::IdKind::kBuildVariant,
                                          std::as_bytes(std::span("bv", 2)));
  d.summary_schema_version = "summary.v2";
  d.relation_schema_version = "relations.v2";
  d.rule_bundle_version = "rules.v2";
  d.model_bundle_version = "models.v1";
  d.svf_configuration_hash = std::string(64, 'a');
  d.wpa_configuration_hash = std::string(64, 'b');
  d.engine = engine;
  d.engine_toolchain_identity = "test-toolchain";
  return std::move(facts::MakeAnalysisRun(d)).value();
}

facts::AnalysisFact ReachableFact(std::string_view from, std::string_view to) {
  facts::SemanticRow row;
  row.relation = facts::RelationId::kReachableCall;
  row.cells = {FunctionId(from), FunctionId(to), sem::EpistemicState::kMust};
  return std::move(facts::MakeFact(row)).value();
}

WpaComponentResult ResultFor(std::string_view scc_name) {
  WpaComponentResult result;
  result.scc_id = FunctionId(scc_name);
  result.component = WpaComponentKind::kReachability;
  result.logical_input_hash = "logical";
  result.fixpoint_hash = "fixpoint";
  result.external_hash = "external";
  result.facts = {ReachableFact("f", "g")};
  return result;
}

std::filesystem::path TempDbPath() {
  std::string tmpl =
      (std::filesystem::temp_directory_path() / "veritas-wpa-XXXXXX").string();
  char* made = ::mkdtemp(tmpl.data());
  return std::filesystem::path(made);
}

TEST(WpaRunRepositoryTest, StoresAndLoadsAComponentResult) {
  const auto db = TempDbPath();
  auto repo = WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok());

  const auto run = MakeManifest(facts::EngineIdentity::kSouffle);
  ASSERT_TRUE(repo->BeginRun(run).ok());

  const WpaComponentKey key{FunctionId("f"), WpaComponentKind::kReachability};
  const WpaComponentResult result = ResultFor("f");
  auto stored = repo->StoreSuccessfulComponent(run, key, result);
  ASSERT_TRUE(stored.ok());
  EXPECT_EQ(stored->key, key);

  auto loaded =
      repo->LoadReusableComponent(DeriveResultCacheKey(run, key, "logical"));
  ASSERT_TRUE(loaded.ok());
  ASSERT_TRUE(loaded->has_value());
  EXPECT_EQ(loaded->value().facts, result.facts);
  EXPECT_EQ(loaded->value().external_hash, "external");
  EXPECT_EQ(loaded->value().scc_id, key.scc_id);

  std::filesystem::remove_all(db);
}

TEST(WpaRunRepositoryTest, FailureMarksRunIncomplete) {
  const auto db = TempDbPath();
  auto repo = WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok());

  const auto run = MakeManifest(facts::EngineIdentity::kSouffle);
  ASSERT_TRUE(repo->BeginRun(run).ok());
  ASSERT_TRUE(repo->MarkIncomplete(run).ok());

  auto status = repo->RunStatus(run.run_id);
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(*status, WpaRunStatus::kIncomplete);

  std::filesystem::remove_all(db);
}

TEST(WpaRunRepositoryTest, ReusesUnchangedResultAcrossRevisions) {
  const auto db = TempDbPath();
  auto repo = WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok());

  const WpaComponentKey key{FunctionId("f"), WpaComponentKind::kReachability};
  const WpaComponentResult result = ResultFor("f");

  // Two runs with different revisions but the same logical input and toolchain.
  auto run1 = MakeManifest(facts::EngineIdentity::kSouffle);
  auto run2 = MakeManifest(facts::EngineIdentity::kSouffle);
  run2.revision_id = core::MakeStableId(
      core::IdKind::kRevision, std::as_bytes(std::span("rev2", 4)));

  ASSERT_TRUE(repo->BeginRun(run1).ok());
  auto stored1 = repo->StoreSuccessfulComponent(run1, key, result);
  ASSERT_TRUE(stored1.ok());

  ASSERT_TRUE(repo->BeginRun(run2).ok());
  auto stored2 = repo->StoreSuccessfulComponent(run2, key, result);
  ASSERT_TRUE(stored2.ok());

  // The cache key ignores the revision, so both point at the same object.
  EXPECT_EQ(stored1->result_object_key, stored2->result_object_key);

  std::filesystem::remove_all(db);
}

}  // namespace
}  // namespace veritas::wpa
