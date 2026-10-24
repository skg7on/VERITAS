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

#include "veritas/cpg/CpgRepository.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "cpg/CpgCanonicalizer.h"
#include "veritas/summarydb/MetadataStore.h"

namespace veritas::cpg {
namespace {

core::StableId Id(core::IdKind kind, std::string_view name) {
  return core::MakeStableId(kind,
                            std::as_bytes(std::span(name.data(), name.size())));
}

ThinCpg MakeGraph() {
  ThinCpg graph;
  graph.SetMetadata(ProjectionMetadata{
      .revision_id = Id(core::IdKind::kRevision, "rev"),
      .build_variant_id = Id(core::IdKind::kBuildVariant, "bv"),
      .module_hash = "modulehash",
      .summary_ids = {Id(core::IdKind::kFunctionSummary, "s1")},
  });
  (void)graph.AddNode(CpgNode{Id(core::IdKind::kFunctionVariant, "f"),
                              NodeKind::kFunction, "foo"});
  (void)graph.AddNode(CpgNode{Id(core::IdKind::kMemoryRef, "m"),
                              NodeKind::kMemoryObject, "mem"});
  CpgEdge edge;
  edge.edge_id = Id(core::IdKind::kCpgEdge, "e");
  edge.kind = EdgeKind::kReads;
  edge.source_node_id = Id(core::IdKind::kFunctionVariant, "f");
  edge.target_node_id = Id(core::IdKind::kMemoryRef, "m");
  edge.support.push_back({Id(core::IdKind::kFunctionSummary, "s1"), "prov"});
  (void)graph.AddEdge(std::move(edge));
  return graph;
}

TEST(CpgRepositoryTest, OpensHistoricalProjectionById) {
  const auto path =
      std::filesystem::temp_directory_path() / "veritas_cpg_repo_test.db";
  std::filesystem::remove(path);
  auto md_result = summarydb::MetadataStore::Open(path);
  ASSERT_TRUE(md_result.ok()) << md_result.status().message();
  auto md = std::move(*md_result);
  ASSERT_TRUE(md.ApplySchema().ok());

  CpgRepository repository(md);
  ThinCpg graph = MakeGraph();
  const core::StableId projection_id = CpgCanonicalizer::ProjectionId(graph);

  ASSERT_TRUE(md.BeginTransaction().ok());
  ASSERT_TRUE(repository.StageProjection(graph).ok());
  ASSERT_TRUE(md.CommitTransaction().ok());

  auto loaded = repository.LoadProjection(projection_id);
  ASSERT_TRUE(loaded.ok()) << loaded.status().message();
  EXPECT_EQ(CpgCanonicalizer::CanonicalBytes(*loaded),
            CpgCanonicalizer::CanonicalBytes(graph));
  EXPECT_EQ(CpgCanonicalizer::ProjectionId(*loaded), projection_id);

  std::filesystem::remove(path);
}

TEST(CpgRepositoryTest, PersistsCallSitesWithoutUpgradingLegacyOwnership) {
  const auto path =
      std::filesystem::temp_directory_path() / "veritas_cpg_calls.db";
  std::filesystem::remove(path);
  auto md = summarydb::MetadataStore::Open(path);
  ASSERT_TRUE(md.ok());
  ASSERT_TRUE(md->ApplySchema().ok());
  CpgRepository repository(*md);
  auto legacy = MakeGraph();
  const auto legacy_id = CpgCanonicalizer::ProjectionId(legacy);
  ASSERT_TRUE(repository.StageProjection(legacy).ok());

  auto graph = MakeGraph();
  auto meta = graph.metadata();
  meta.schema_version = kCompilerOwnedProjection;
  graph.SetMetadata(meta);
  const auto site = Id(core::IdKind::kCallSite, "call");
  const auto target = Id(core::IdKind::kUnknownNode, "memcpy");
  const auto owner = Id(core::IdKind::kFunctionVariant, "f");
  const auto memory = Id(core::IdKind::kMemoryRef, "m");
  ASSERT_TRUE(graph.AddNode({site, NodeKind::kCallSite, "memcpy"}).ok());
  ASSERT_TRUE(graph.AddNode({target, NodeKind::kUnknown, "memcpy"}).ok());
  std::vector<CallSiteArgument> args;
  for (std::uint32_t i = 0; i < 3; ++i) {
    const auto value = Id(core::IdKind::kValueRef, "arg" + std::to_string(i));
    ASSERT_TRUE(graph.AddNode({value, NodeKind::kParameter, "arg"}).ok());
    args.push_back({i, value, i < 2 ? std::optional(memory) : std::nullopt});
  }
  ASSERT_TRUE(graph
                  .AddEdge({Id(core::IdKind::kCpgEdge, "contains"),
                            EdgeKind::kContains, owner, site, std::nullopt})
                  .ok());
  ASSERT_TRUE(graph
                  .AddEdge({Id(core::IdKind::kCpgEdge, "calls"),
                            EdgeKind::kCalls, site, target, std::nullopt})
                  .ok());
  ASSERT_TRUE(graph
                  .AddCallSiteBinding(
                      {site, owner, target, "memcpy.v1", args, std::nullopt})
                  .ok());
  ASSERT_TRUE(graph.Validate().ok());
  const auto id = CpgCanonicalizer::ProjectionId(graph);
  ASSERT_TRUE(md->BeginTransaction().ok());
  ASSERT_TRUE(repository.StageProjection(graph).ok());
  ASSERT_TRUE(md->CommitTransaction().ok());
  auto loaded = repository.LoadProjection(id);
  ASSERT_TRUE(loaded.ok()) << loaded.status().message();
  ASSERT_EQ(loaded->callsites().size(), 1u);
  EXPECT_EQ(loaded->callsites()[0], graph.callsites()[0]);
  EXPECT_TRUE(loaded->HasCompilerOwnership());
  EXPECT_EQ(CpgCanonicalizer::ProjectionId(*loaded), id);
  auto historical = repository.LoadProjection(legacy_id);
  ASSERT_TRUE(historical.ok());
  EXPECT_FALSE(historical->HasCompilerOwnership());
  EXPECT_TRUE(historical->callsites().empty());
  EXPECT_EQ(CpgCanonicalizer::ProjectionId(*historical), legacy_id);

  auto next = graph;
  meta.revision_id = Id(core::IdKind::kRevision, "next");
  next.SetMetadata(meta);
  const auto next_id = CpgCanonicalizer::ProjectionId(next);
  ASSERT_TRUE(md->BeginTransaction().ok());
  ASSERT_TRUE(repository.StageProjection(next).ok());
  ASSERT_TRUE(md->RollbackTransaction().ok());
  EXPECT_FALSE(repository.LoadProjection(next_id).ok());
  EXPECT_EQ(*repository.OpenCurrent(graph.metadata().revision_id,
                                    graph.metadata().build_variant_id),
            id);

  // A valid but cross-wired operand must fail the persisted content identity,
  // rather than acquiring the old projection's authority after reload.
  ASSERT_TRUE(md->Execute("UPDATE cpg_callsite_arguments SET value_id = ? "
                          "WHERE projection_id = ? "
                          "AND callsite_id = ? AND argument_index = 1",
                          {core::ToString(args[2].value_ref),
                           core::ToString(id), core::ToString(site)})
                  .ok());
  EXPECT_FALSE(repository.LoadProjection(id).ok());
  std::filesystem::remove(path);
}

} // namespace
} // namespace veritas::cpg
