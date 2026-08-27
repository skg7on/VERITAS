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

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

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

// relations.v2.manifest is the Datalog-side mirror of the compiled registry:
// logic/schema/relations.v2.dl declares these relations by hand, so the two
// must agree. Checking it here is what keeps the manifest from becoming an
// unenforced copy that silently drifts, which is what its JSON predecessor
// had become.
//
// The domain and epistemic spellings below are deliberately a test-local
// table rather than a production helper: adding a ColumnDomain or an
// EpistemicState without teaching this test its manifest spelling fails here
// instead of silently writing an unreadable manifest.
namespace {

std::string_view DomainText(ColumnDomain domain) {
  switch (domain) {
    case ColumnDomain::kFunctionId: return "function_id";
    case ColumnDomain::kValueId: return "value_id";
    case ColumnDomain::kMemoryId: return "memory_id";
    case ColumnDomain::kCallSiteId: return "call_site_id";
    case ColumnDomain::kFactId: return "fact_id";
    case ColumnDomain::kModelId: return "model_id";
    case ColumnDomain::kInt64: return "int64";
    case ColumnDomain::kUint64: return "uint64";
    case ColumnDomain::kString: return "string";
    case ColumnDomain::kDispatchKind: return "dispatch_kind";
    case ColumnDomain::kAliasKind: return "alias_kind";
    case ColumnDomain::kByteRangeKind: return "byte_range_kind";
    case ColumnDomain::kEpistemic: return "epistemic";
  }
  return {};
}

std::string_view EpistemicText(EpistemicState state) {
  switch (state) {
    case EpistemicState::kMust: return "MUST";
    case EpistemicState::kMay: return "MAY";
    case EpistemicState::kMustNot: return "MUST_NOT";
    case EpistemicState::kInferred: return "INFERRED";
    case EpistemicState::kAssumed: return "ASSUMED";
    case EpistemicState::kUnknown: return "UNKNOWN";
  }
  return {};
}

std::string_view OwnershipText(RelationOwnership ownership) {
  return ownership == RelationOwnership::kEdb ? "edb" : "idb";
}

// name <TAB> ownership <TAB> column:domain,... <TAB> epistemic,... ("-" when
// the relation has no epistemic column).
std::string ExpectedManifestRow(RelationId id) {
  const auto& schema = RelationsV2().Get(id);
  std::string row = schema.name;
  row += '\t';
  row += OwnershipText(schema.ownership);
  row += '\t';
  for (std::size_t i = 0; i < schema.columns.size(); ++i) {
    if (i != 0) row += ',';
    row += schema.columns[i].name;
    row += ':';
    row += DomainText(schema.columns[i].domain);
  }
  row += '\t';
  if (schema.allowed_epistemic.empty()) {
    row += '-';
  } else {
    for (std::size_t i = 0; i < schema.allowed_epistemic.size(); ++i) {
      if (i != 0) row += ',';
      row += EpistemicText(schema.allowed_epistemic[i]);
    }
  }
  return row;
}

}  // namespace

TEST(RelationSchemaManifestTest, ManifestMatchesCompiledRegistry) {
  const std::filesystem::path manifest_path =
      std::filesystem::path(VERITAS_LOGIC_DIR) / "schema" /
      "relations.v2.manifest";
  std::ifstream stream(manifest_path);
  ASSERT_TRUE(stream.is_open()) << "missing " << manifest_path;

  std::string version;
  std::vector<std::string> rows;
  std::string line;
  while (std::getline(stream, line)) {
    if (line.empty() || line[0] == '#') continue;
    if (line.starts_with("relation_schema_version=")) {
      version = line.substr(std::string("relation_schema_version=").size());
      continue;
    }
    rows.push_back(line);
  }

  EXPECT_EQ(version, "relations.v2");

  // Both directions: every registry relation appears, in registry order, and
  // the manifest carries nothing the registry does not declare.
  ASSERT_EQ(rows.size(), kRelationCountV2);
  for (std::size_t i = 0; i < kRelationCountV2; ++i) {
    const auto id = static_cast<RelationId>(i);
    EXPECT_EQ(rows[i], ExpectedManifestRow(id))
        << "manifest line " << (i + 1) << " disagrees with the registry";
  }
}
