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

// WpaMigrationQualificationTest.cpp — the tagged V1 projection never
// fabricates precision it does not carry, and a native V2 reanalysis
// supersedes the current binding without mutating the immutable V1 bytes.

#include <filesystem>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>

#include "WpaQualificationSupport.h"
#include "veritas/summarydb/SummaryRepository.h"

namespace veritas::wpa::qualification {
namespace {

namespace v1 = summary::v1;

v1::FunctionSummary V1Summary(std::string_view name) {
  v1::FunctionSummary summary;
  summary.mutable_header()->set_schema_version("summary.v1");
  summary.mutable_identity()->set_function_variant_id(
      core::ToString(FunctionId(name)));
  return summary;
}

void AddV1ResolvedCall(v1::FunctionSummary* caller, std::string_view callee) {
  auto* call = caller->add_calls();
  call->set_callee_symbol(std::string(callee));
  call->set_resolved_callee_function_variant_id(core::ToString(FunctionId(callee)));
  call->set_call_site_anchor_id(core::ToString(CallSiteId(std::string(callee))));
  call->set_epistemic(v1::EPISTEMIC_STATE_MUST);
  call->set_provenance_ref("test:call");
}

void AddV1MemoryWrite(v1::FunctionSummary* function, std::string memory) {
  auto* effect = function->add_memory_effects();
  effect->set_kind(v1::EFFECT_KIND_WRITE);
  effect->set_location(std::move(memory));
  effect->set_epistemic(v1::EPISTEMIC_STATE_MUST);
  effect->set_provenance_ref("test:write");
}

bool ContainsDispatchKind(const WpaLogicalComponentInput& logical,
                          sem::DispatchKind kind) {
  for (const auto& row : logical.edb) {
    for (const auto& cell : row.cells) {
      if (const auto* dispatch = std::get_if<sem::DispatchKind>(&cell)) {
        if (*dispatch == kind)
          return true;
      }
    }
  }
  return false;
}

bool ContainsMemoryRows(const WpaLogicalComponentInput& logical) {
  for (const auto& row : logical.edb) {
    if (row.relation == facts::RelationId::kDirectWrite ||
        row.relation == facts::RelationId::kDirectRead)
      return true;
  }
  return false;
}

std::filesystem::path TempDbPath() {
  std::string tmpl =
      (std::filesystem::temp_directory_path() / "veritas-wpa-XXXXXX").string();
  char* made = ::mkdtemp(tmpl.data());
  return std::filesystem::path(made);
}

TEST(WpaMigrationQualificationTest, V1ProjectionIsTaggedAndNeverFabricates) {
  auto caller = V1Summary("caller");
  AddV1ResolvedCall(&caller, "callee");
  AddV1MemoryWrite(&caller, "opaque:legacy");

  // V1 carries no dispatch kind and no abstract-memory identity, so the
  // projection must emit an unknown dispatch and no fabricated memory rows.
  const std::vector<summary::SummaryArtifact> artifacts = {caller, V1Summary("callee")};

  auto reach =
      InputFor(artifacts, WpaComponentKind::kReachability, "caller");
  ASSERT_TRUE(reach.ok()) << reach.status().message();
  EXPECT_TRUE(ContainsDispatchKind(*reach, sem::DispatchKind::kUnknown));

  auto memory =
      InputFor(artifacts, WpaComponentKind::kMemoryEffects, "caller");
  ASSERT_TRUE(memory.ok()) << memory.status().message();
  EXPECT_FALSE(ContainsMemoryRows(*memory));
}

TEST(WpaMigrationQualificationTest, ReanalysisSupersedesWithoutMutation) {
  const auto db = TempDbPath();
  auto opened = summarydb::SummaryRepository::Open(db.string());
  ASSERT_TRUE(opened.ok()) << opened.status().message();
  auto repo = std::move(*opened);

  const std::string repository_id =
      core::ToString(core::MakeStableId(core::IdKind::kRepository,
                                        std::as_bytes(std::span("repo", 4))));
  const std::string revision_id =
      core::ToString(core::MakeStableId(core::IdKind::kRevision,
                                        std::as_bytes(std::span("rev", 3))));
  const std::string build_variant_id =
      core::ToString(core::MakeStableId(core::IdKind::kBuildVariant,
                                        std::as_bytes(std::span("bv", 2))));

  ASSERT_TRUE(repo->metadata_store()
                  .Execute("INSERT INTO repositories(repository_id, vcs_kind, "
                           "vcs_revision, source_tree_hash) VALUES(?, ?, ?, ?)",
                           {repository_id, "git", "r", "tree"})
                  .ok());
  ASSERT_TRUE(repo->metadata_store()
                  .Execute("INSERT INTO revisions(revision_id, repository_id, "
                           "vcs_revision) VALUES(?, ?, ?)",
                           {revision_id, repository_id, "r"})
                  .ok());
  ASSERT_TRUE(repo->metadata_store()
                  .Execute(
                      "INSERT INTO build_variants(build_variant_id, "
                      "target_triple, compiler_id, compiler_version, "
                      "compile_options_hash, macro_set_hash, "
                      "include_closure_hash, type_layout_hash) "
                      "VALUES(?, ?, ?, ?, ?, ?, ?, ?)",
                      {build_variant_id, "arm64", "clang", "24", "a", "b", "c",
                       "d"})
                  .ok());

  const std::string function_variant_id = core::ToString(FunctionId("f"));

  auto v1_summary = V1Summary("f");
  auto published_v1 =
      repo->PublishProjectSummaries(revision_id, build_variant_id, {v1_summary});
  ASSERT_TRUE(published_v1.ok()) << published_v1.status().message();
  const auto v1_bytes = v1_summary.SerializeAsString();

  auto v2_summary = V2Summary("f");
  auto published_v2 =
      repo->PublishProjectSummaries(revision_id, build_variant_id, {v2_summary});
  ASSERT_TRUE(published_v2.ok()) << published_v2.status().message();

  // The current binding is now native V2.
  auto current = repo->GetCurrentSummaryArtifact(function_variant_id);
  ASSERT_TRUE(current.ok()) << current.status().message();
  EXPECT_NE(std::get_if<v2::FunctionSummary>(&*current), nullptr);

  // The immutable V1 object is byte-identical, not mutated by the reanalysis.
  auto historical = repo->GetSummary((*published_v1)[0]);
  ASSERT_TRUE(historical.ok()) << historical.status().message();
  EXPECT_EQ(historical->SerializeAsString(), v1_bytes);

  std::filesystem::remove_all(db);
}

}  // namespace
}  // namespace veritas::wpa::qualification
