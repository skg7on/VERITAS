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
#include <string>

#include "cpg/CpgCanonicalizer.h"
#include "veritas/summarydb/MetadataStore.h"

namespace veritas::cpg {
namespace {

core::StableId Id(core::IdKind kind, std::string hex) {
  // Pad to a valid 64-char SHA-256 hex digest so the ID round-trips through
  // ParseStableId in the repository.
  hex.resize(64, '0');
  return core::StableId{kind, std::move(hex)};
}

ThinCpg MakeGraph() {
  ThinCpg graph;
  graph.SetMetadata(ProjectionMetadata{
      .revision_id = Id(core::IdKind::kRevision, "rev"),
      .build_variant_id = Id(core::IdKind::kBuildVariant, "bv"),
      .module_hash = "modulehash",
      .summary_ids = {Id(core::IdKind::kFunctionSummary, "s1")},
  });
  (void)graph.AddNode(
      CpgNode{Id(core::IdKind::kFunctionVariant, "f"), NodeKind::kFunction, "foo"});
  (void)graph.AddNode(
      CpgNode{Id(core::IdKind::kMemoryRef, "m"), NodeKind::kMemoryObject, "mem"});
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

}  // namespace
}  // namespace veritas::cpg
