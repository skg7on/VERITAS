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

#include "veritas/cpg/ThinCpg.h"

#include <gtest/gtest.h>

#include <string>

#include "veritas/core/Ids.h"

namespace veritas::cpg {
namespace {

core::StableId Id(core::IdKind kind, std::string hex) {
  return core::StableId{kind, std::move(hex)};
}

CpgNode FunctionNode(std::string hex, std::string label) {
  return CpgNode{Id(core::IdKind::kFunctionVariant, std::move(hex)),
                 NodeKind::kFunction, std::move(label)};
}

CpgNode MemoryNode(std::string hex) {
  return CpgNode{Id(core::IdKind::kMemoryRef, std::move(hex)),
                 NodeKind::kMemoryObject, "mem"};
}

ThinCpg GraphWithMetadata() {
  ThinCpg graph;
  graph.SetMetadata(ProjectionMetadata{
      .schema_version = "veritas.cpg.v1",
      .revision_id = Id(core::IdKind::kRevision, "rev"),
      .build_variant_id = Id(core::IdKind::kBuildVariant, "bv"),
      .module_hash = "modulehash",
  });
  return graph;
}

TEST(ThinCpgTest, PreservesAllFourAliasStatesWithoutPairFanout) {
  ThinCpg graph = GraphWithMetadata();
  ASSERT_TRUE(graph.AddNode(MemoryNode("a")).ok());
  ASSERT_TRUE(graph.AddNode(MemoryNode("b")).ok());

  const core::StableId left = Id(core::IdKind::kMemoryRef, "a");
  const core::StableId right = Id(core::IdKind::kMemoryRef, "b");
  const AliasState states[] = {AliasState::kMustAlias, AliasState::kMayAlias,
                               AliasState::kNoAlias, AliasState::kUnknownAlias};
  int ordinal = 0;
  for (AliasState state : states) {
    CpgEdge edge;
    edge.edge_id = Id(core::IdKind::kCpgEdge, "alias" + std::to_string(ordinal++));
    edge.kind = EdgeKind::kAliases;
    edge.source_node_id = left;
    edge.target_node_id = right;
    edge.alias_state = state;
    edge.support.push_back(
        SupportRef{Id(core::IdKind::kFunctionSummary, "summary:one"), "prov:one"});
    ASSERT_TRUE(graph.AddEdge(std::move(edge)).ok());
  }

  std::size_t alias_edges = 0;
  for (const auto& edge : graph.edges()) {
    if (edge.kind == EdgeKind::kAliases) ++alias_edges;
  }
  EXPECT_EQ(alias_edges, 4u);
}

TEST(ThinCpgTest, RejectsStableIdCollisionWithDifferentContent) {
  ThinCpg graph;
  ASSERT_TRUE(graph.AddNode(FunctionNode("same", "first")).ok());
  Status status = graph.AddNode(FunctionNode("same", "second"));
  EXPECT_EQ(status.code(), StatusCode::kFailedPrecondition);
}

TEST(ThinCpgTest, DuplicateNodeWithIdenticalContentIsIdempotent) {
  ThinCpg graph;
  ASSERT_TRUE(graph.AddNode(FunctionNode("same", "first")).ok());
  ASSERT_TRUE(graph.AddNode(FunctionNode("same", "first")).ok());
  EXPECT_EQ(graph.nodes().size(), 1u);
}

TEST(ThinCpgTest, RejectsAliasEdgeWithoutAliasState) {
  ThinCpg graph = GraphWithMetadata();
  ASSERT_TRUE(graph.AddNode(MemoryNode("a")).ok());
  ASSERT_TRUE(graph.AddNode(MemoryNode("b")).ok());

  CpgEdge edge;
  edge.edge_id = Id(core::IdKind::kCpgEdge, "alias");
  edge.kind = EdgeKind::kAliases;
  edge.source_node_id = Id(core::IdKind::kMemoryRef, "a");
  edge.target_node_id = Id(core::IdKind::kMemoryRef, "b");
  EXPECT_EQ(graph.AddEdge(std::move(edge)).code(), StatusCode::kInvalidArgument);
}

TEST(ThinCpgTest, ValidateRejectsMissingEndpoint) {
  ThinCpg graph = GraphWithMetadata();
  ASSERT_TRUE(graph.AddNode(MemoryNode("a")).ok());

  CpgEdge edge;
  edge.edge_id = Id(core::IdKind::kCpgEdge, "reads");
  edge.kind = EdgeKind::kReads;
  edge.source_node_id = Id(core::IdKind::kMemoryRef, "a");
  edge.target_node_id = Id(core::IdKind::kMemoryRef, "missing");
  ASSERT_TRUE(graph.AddEdge(std::move(edge)).ok());

  EXPECT_FALSE(graph.Validate().ok());
}

}  // namespace
}  // namespace veritas::cpg
