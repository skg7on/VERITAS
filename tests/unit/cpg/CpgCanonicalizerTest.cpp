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

#include "cpg/CpgCanonicalizer.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/cpg/ThinCpg.h"

namespace veritas::cpg {
namespace {

core::StableId Id(core::IdKind kind, std::string hex) {
  return core::StableId{kind, std::move(hex)};
}

CpgNode MemoryNode(std::string hex) {
  return CpgNode{Id(core::IdKind::kMemoryRef, std::move(hex)),
                 NodeKind::kMemoryObject, "mem"};
}

CpgEdge AliasEdge(std::string id, std::string src, std::string dst,
                  AliasState state) {
  CpgEdge edge;
  edge.edge_id = Id(core::IdKind::kCpgEdge, std::move(id));
  edge.kind = EdgeKind::kAliases;
  edge.source_node_id = Id(core::IdKind::kMemoryRef, std::move(src));
  edge.target_node_id = Id(core::IdKind::kMemoryRef, std::move(dst));
  edge.alias_state = state;
  edge.support.push_back(
      SupportRef{Id(core::IdKind::kFunctionSummary, "summary:one"), "prov"});
  return edge;
}

ThinCpg BuildFixtureGraph(bool reverse) {
  ThinCpg graph;
  graph.SetMetadata(ProjectionMetadata{
      .schema_version = "veritas.cpg.v1",
      .revision_id = Id(core::IdKind::kRevision, "rev"),
      .build_variant_id = Id(core::IdKind::kBuildVariant, "bv"),
      .module_hash = "modulehash",
      .summary_ids = {Id(core::IdKind::kFunctionSummary, "s2"),
                      Id(core::IdKind::kFunctionSummary, "s1")},
  });

  std::vector<CpgNode> nodes = {MemoryNode("a"), MemoryNode("b"),
                                MemoryNode("c")};
  std::vector<CpgEdge> edges = {
      AliasEdge("e1", "a", "b", AliasState::kMustAlias),
      AliasEdge("e2", "b", "c", AliasState::kMayAlias),
      AliasEdge("e3", "a", "c", AliasState::kNoAlias),
  };

  if (reverse) {
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
      (void)graph.AddNode(*it);
    }
    for (auto it = edges.rbegin(); it != edges.rend(); ++it) {
      (void)graph.AddEdge(*it);
    }
  } else {
    for (const auto& node : nodes) {
      (void)graph.AddNode(node);
    }
    for (const auto& edge : edges) {
      (void)graph.AddEdge(edge);
    }
  }
  return graph;
}

TEST(CpgCanonicalizerTest, IgnoresInsertionOrderAndNativeAddresses) {
  ThinCpg forward = BuildFixtureGraph(false);
  ThinCpg reverse = BuildFixtureGraph(true);

  EXPECT_EQ(CpgCanonicalizer::CanonicalBytes(forward),
            CpgCanonicalizer::CanonicalBytes(reverse));
  EXPECT_EQ(CpgCanonicalizer::ProjectionId(forward),
            CpgCanonicalizer::ProjectionId(reverse));
}

TEST(CpgCanonicalizerTest, DifferentContentProducesDifferentProjectionId) {
  ThinCpg first = BuildFixtureGraph(false);
  ThinCpg second = BuildFixtureGraph(false);
  // Change one node's label so the content differs.
  ASSERT_TRUE(second.AddNode(CpgNode{Id(core::IdKind::kMemoryRef, "d"),
                                     NodeKind::kMemoryObject, "extra"})
                  .ok());

  EXPECT_NE(CpgCanonicalizer::ProjectionId(first),
            CpgCanonicalizer::ProjectionId(second));
}

}  // namespace
}  // namespace veritas::cpg
