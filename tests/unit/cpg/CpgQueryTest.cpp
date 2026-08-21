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

#include "veritas/cpg/CpgQuery.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "cpg/CpgCanonicalizer.h"
#include "veritas/cpg/CpgRepository.h"
#include "veritas/summarydb/MetadataStore.h"

namespace veritas::cpg {
namespace {

core::StableId Id(core::IdKind kind, std::string_view name) {
  return core::MakeStableId(kind,
                            std::as_bytes(std::span(name.data(), name.size())));
}

CpgNode ValueNode(std::string hex) {
  return CpgNode{Id(core::IdKind::kValueRef, hex), NodeKind::kParameter, hex};
}

CpgNode FunctionNode(std::string hex) {
  return CpgNode{Id(core::IdKind::kFunctionVariant, hex), NodeKind::kFunction,
                 hex};
}

ThinCpg BuildFlowGraph() {
  ThinCpg graph;
  graph.SetMetadata(ProjectionMetadata{
      .revision_id = Id(core::IdKind::kRevision, "rev"),
      .build_variant_id = Id(core::IdKind::kBuildVariant, "bv"),
      .module_hash = "modulehash",
      .summary_ids = {Id(core::IdKind::kFunctionSummary, "s1")},
  });

  (void)graph.AddNode(ValueNode("start"));
  (void)graph.AddNode(ValueNode("mid1"));
  (void)graph.AddNode(ValueNode("mid2"));
  (void)graph.AddNode(ValueNode("end"));
  (void)graph.AddNode(FunctionNode("f1"));
  (void)graph.AddNode(FunctionNode("f2"));

  auto flow = [&](std::string id, std::string src, std::string dst) {
    CpgEdge edge;
    edge.edge_id = Id(core::IdKind::kCpgEdge, std::move(id));
    edge.kind = EdgeKind::kFlowsTo;
    edge.source_node_id = Id(core::IdKind::kValueRef, std::move(src));
    edge.target_node_id = Id(core::IdKind::kValueRef, std::move(dst));
    (void)graph.AddEdge(std::move(edge));
  };
  flow("e1", "start", "mid1");
  flow("e2", "mid1", "mid2");
  flow("e3", "mid2", "end");

  CpgEdge call;
  call.edge_id = Id(core::IdKind::kCpgEdge, "call");
  call.kind = EdgeKind::kCalls;
  call.source_node_id = Id(core::IdKind::kFunctionVariant, "f1");
  call.target_node_id = Id(core::IdKind::kFunctionVariant, "f2");
  (void)graph.AddEdge(std::move(call));

  return graph;
}

CpgQuery OpenQuery(const ThinCpg &graph) {
  const auto path =
      std::filesystem::temp_directory_path() / "veritas_cpg_query_test.db";
  std::filesystem::remove(path);
  auto md = std::move(*summarydb::MetadataStore::Open(path));
  (void)md.ApplySchema();
  CpgRepository repository(md);
  (void)md.BeginTransaction();
  (void)repository.StageProjection(graph);
  (void)md.CommitTransaction();
  auto query = CpgQuery::OpenProjection(repository,
                                        CpgCanonicalizer::ProjectionId(graph));
  std::filesystem::remove(path);
  return std::move(*query);
}

TEST(CpgQueryTest, DistinguishesNoPathFromTruncatedSearch) {
  CpgQuery query = OpenQuery(BuildFlowGraph());

  auto no_path = query.GetValueFlow(
      Id(core::IdKind::kValueRef, "unconnected_a"),
      Id(core::IdKind::kValueRef, "unconnected_b"), QueryBudget{10, 100, 10});
  ASSERT_TRUE(no_path.ok());
  EXPECT_TRUE(no_path->items.empty());
  EXPECT_TRUE(no_path->truncation_reasons.empty());

  auto truncated = query.GetValueFlow(Id(core::IdKind::kValueRef, "start"),
                                      Id(core::IdKind::kValueRef, "end"),
                                      QueryBudget{2, 3, 1});
  ASSERT_TRUE(truncated.ok());
  bool has_max_depth = false;
  for (auto reason : truncated->truncation_reasons) {
    if (reason == TruncationReason::kMaxDepth)
      has_max_depth = true;
  }
  EXPECT_TRUE(has_max_depth);
}

TEST(CpgQueryTest, ExactBudgetBoundaryIsComplete) {
  CpgQuery query = OpenQuery(BuildFlowGraph());

  auto result = query.GetValueFlow(Id(core::IdKind::kValueRef, "start"),
                                   Id(core::IdKind::kValueRef, "end"),
                                   QueryBudget{3, 4, 1});
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->items.size(), 1u);
  EXPECT_TRUE(result->truncation_reasons.empty());
  EXPECT_EQ(result->explored_nodes, 4u);
  EXPECT_EQ(result->explored_paths, 1u);
}

TEST(CpgQueryTest, ReportsCallees) {
  CpgQuery query = OpenQuery(BuildFlowGraph());
  auto callees = query.GetCallees(Id(core::IdKind::kFunctionVariant, "f1"));
  ASSERT_TRUE(callees.ok());
  ASSERT_EQ(callees->size(), 1u);
  EXPECT_EQ((*callees)[0].node_id, Id(core::IdKind::kFunctionVariant, "f2"));
}

} // namespace
} // namespace veritas::cpg
