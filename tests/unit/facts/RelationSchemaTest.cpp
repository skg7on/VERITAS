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

#include "veritas/facts/AnalysisFact.h"
#include "veritas/facts/RelationSchema.h"

#include <cstdint>
#include <variant>

#include <gtest/gtest.h>

using namespace veritas;
using namespace veritas::analysis::semantic;
using namespace veritas::facts;

namespace {

ExecutionRow DirectReadExecutionRow(FunctionId function, MemoryId memory,
                                    const ByteRange& range,
                                    EpistemicState epistemic) {
  ExecutionRow row;
  row.relation = RelationId::kDirectRead;
  row.cells = {function, memory, RelationRangeKind(range),
               range.offset.value_or(0), range.size.value_or(0), epistemic};
  return row;
}

}  // namespace

TEST(RelationSchemaTest, DirectCallHasTypedV2Columns) {
  const auto& schema = RelationsV2().Get(RelationId::kDirectCall);
  EXPECT_EQ(schema.name, "DirectCall");
  EXPECT_EQ(schema.columns[0].domain, ColumnDomain::kCallSiteId);
  EXPECT_EQ(schema.columns[1].domain, ColumnDomain::kFunctionId);
  EXPECT_EQ(schema.columns[2].domain, ColumnDomain::kFunctionId);
  EXPECT_EQ(schema.columns[3].domain, ColumnDomain::kDispatchKind);
  EXPECT_EQ(schema.columns[4].domain, ColumnDomain::kEpistemic);
}

// Successor SCC results enter a component as explicit support relations, so
// each derived domain needs an EDB relation with the same column shape as the
// IDB relation it carries results for.
TEST(RelationSchemaTest, SupportRelationsMirrorTheirIdbShapeAsEdb) {
  const auto& reachable = RelationsV2().Get(RelationId::kSupportReachableCall);
  EXPECT_EQ(reachable.name, "SupportReachableCall");
  EXPECT_EQ(reachable.ownership, RelationOwnership::kEdb);
  EXPECT_EQ(reachable.columns,
            RelationsV2().Get(RelationId::kReachableCall).columns);

  const auto& may_write = RelationsV2().Get(RelationId::kSupportMayWrite);
  EXPECT_EQ(may_write.name, "SupportMayWrite");
  EXPECT_EQ(may_write.ownership, RelationOwnership::kEdb);
  EXPECT_EQ(may_write.columns,
            RelationsV2().Get(RelationId::kMayWrite).columns);
}

TEST(RelationSchemaTest, ValidatesSupportReachableCallExecutionRow) {
  ExecutionRow row{RelationId::kSupportReachableCall,
                   {FunctionId{1}, FunctionId{2}, EpistemicState::kMay}};
  EXPECT_TRUE(ValidateExecutionRow(row).ok());
}

TEST(RelationSchemaTest, RejectsCrossDomainDenseId) {
  ExecutionRow row{RelationId::kDirectCall,
                   {MemoryId{1}, FunctionId{1}, FunctionId{2},
                    DispatchKind::kDirect, EpistemicState::kMust}};
  EXPECT_FALSE(ValidateExecutionRow(row).ok());
}

TEST(RelationSchemaTest, DirectReadPreservesUnknownRangeTag) {
  auto row = DirectReadExecutionRow(FunctionId{1}, MemoryId{2},
                                    ByteRange::Unknown(), EpistemicState::kMay);
  ASSERT_TRUE(ValidateExecutionRow(row).ok());
  EXPECT_EQ(std::get<ByteRangeKind>(row.cells[2]), ByteRangeKind::kUnknown);
  EXPECT_EQ(std::get<std::int64_t>(row.cells[3]), std::int64_t{0});
  EXPECT_EQ(std::get<std::uint64_t>(row.cells[4]), std::uint64_t{0});
}

TEST(RelationSchemaTest, RejectsNonCanonicalUnknownRangePayload) {
  ExecutionRow row;
  row.relation = RelationId::kDirectRead;
  row.cells = {FunctionId{1}, MemoryId{2}, ByteRangeKind::kUnknown,
               std::int64_t{4}, std::uint64_t{0}, EpistemicState::kMay};
  EXPECT_FALSE(ValidateExecutionRow(row).ok());
}
