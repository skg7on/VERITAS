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

#include "veritas/facts/SouffleExporter.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>

namespace veritas::facts {
namespace {

core::StableId SummaryId(std::string_view text) {
  return core::MakeStableId(core::IdKind::kFunctionSummary,
                            std::as_bytes(std::span(text.data(), text.size())));
}

FactTuple BaseFact(FactRelation relation, std::vector<std::string> columns,
                   std::string_view anchor) {
  auto tuple = MakeBaseFact(relation, std::move(columns),
                            summary::v1::EPISTEMIC_STATE_MUST,
                            {.function_summary_id = SummaryId("summary"),
                             .anchor = std::string(anchor),
                             .provenance_ref = "test"});
  EXPECT_TRUE(tuple.ok()) << tuple.status().message();
  return std::move(*tuple);
}

std::string ReadFile(const std::filesystem::path &path) {
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

void WriteFile(const std::filesystem::path &path, std::string_view contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(output) << path;
  output << contents;
  output.close();
  ASSERT_TRUE(output) << path;
}

const FactTuple *Find(const std::vector<FactTuple> &facts,
                      FactRelation relation,
                      const std::vector<std::string> &columns) {
  for (const auto &fact : facts) {
    if (fact.relation == relation && fact.columns == columns)
      return &fact;
  }
  return nullptr;
}

std::vector<core::StableId>
SortedIds(std::initializer_list<core::StableId> ids) {
  std::vector<core::StableId> sorted(ids);
  std::ranges::sort(sorted);
  return sorted;
}

std::size_t FieldCount(std::string_view row) {
  if (row.empty())
    return 0;
  std::size_t count = 1;
  for (const char character : row) {
    if (character == '\t')
      ++count;
  }
  return count;
}

class SouffleExporterTest : public ::testing::Test {
protected:
  void SetUp() override {
    test_dir_ = std::filesystem::temp_directory_path() /
                ("veritas_souffle_exporter_test_" +
                 std::to_string(static_cast<unsigned long long>(getpid())) +
                 "_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    std::filesystem::remove_all(test_dir_);
    std::filesystem::create_directories(test_dir_);
  }

  void TearDown() override { std::filesystem::remove_all(test_dir_); }

  std::filesystem::path test_dir_;
};

TEST_F(SouffleExporterTest, WritesBaseRelationsInTupleIdOrder) {
  const auto later = BaseFact(FactRelation::kDirectCall, {"B", "C"}, "site:2");
  const auto earlier =
      BaseFact(FactRelation::kDirectCall, {"A", "B"}, "site:1");
  const auto read =
      BaseFact(FactRelation::kDirectRead, {"A", "memory:r"}, "memory:r");
  const auto write =
      BaseFact(FactRelation::kDirectWrite, {"B", "memory:w"}, "memory:w");
  const auto flow =
      BaseFact(FactRelation::kLocalFlow, {"v1", "v2", "A"}, "flow:1");
  const auto alias =
      BaseFact(FactRelation::kMayAlias, {"memory:r", "memory:w"}, "alias:1");
  const std::array facts{later, earlier, read, write, flow, alias};

  ASSERT_TRUE(SouffleExporter::WriteBaseRelations(test_dir_, facts).ok());

  const std::string rows = ReadFile(test_dir_ / "DirectCall.facts");
  const std::string earlier_id = core::ToString(earlier.tuple_id);
  const std::string later_id = core::ToString(later.tuple_id);
  const std::string expected_first_row = earlier_id < later_id
                                             ? earlier_id + "\tA\tB\t1\n"
                                             : later_id + "\tB\tC\t1\n";
  EXPECT_EQ(rows.substr(0, expected_first_row.size()), expected_first_row);

  EXPECT_EQ(FieldCount(ReadFile(test_dir_ / "DirectRead.facts")), 4u);
  EXPECT_EQ(FieldCount(ReadFile(test_dir_ / "DirectWrite.facts")), 4u);
  EXPECT_EQ(FieldCount(ReadFile(test_dir_ / "LocalFlow.facts")), 5u);
  EXPECT_EQ(FieldCount(ReadFile(test_dir_ / "MayAlias.facts")), 4u);
}

TEST_F(SouffleExporterTest, RejectsConflictingTupleIdBeforeReplacingFiles) {
  const auto original =
      BaseFact(FactRelation::kDirectCall, {"A", "B"}, "site:original");
  const std::array initial{original};
  ASSERT_TRUE(SouffleExporter::WriteBaseRelations(test_dir_, initial).ok());
  const std::string prior_rows = ReadFile(test_dir_ / "DirectCall.facts");
  auto conflict = original;
  conflict.columns = {"X", "Y"};
  const std::array conflicting{original, conflict};

  auto status = SouffleExporter::WriteBaseRelations(test_dir_, conflicting);

  ASSERT_FALSE(status.ok());
  EXPECT_NE(status.message().find("conflicting duplicate"), std::string::npos);
  EXPECT_EQ(ReadFile(test_dir_ / "DirectCall.facts"), prior_rows);
}

TEST_F(SouffleExporterTest, ReconstructsImmediateMayWriteProvenance) {
  const std::array base_facts{
      BaseFact(FactRelation::kDirectCall, {"A", "B"}, "call:A:B"),
      BaseFact(FactRelation::kDirectCall, {"B", "C"}, "call:B:C"),
      BaseFact(FactRelation::kDirectWrite, {"C", "X"}, "write:C:X"),
  };
  WriteFile(test_dir_ / "ReachableCall.csv", "A\tB\t1\nB\tC\t1\nA\tC\t1\n");
  WriteFile(test_dir_ / "MayWrite.csv", "C\tX\t1\nB\tX\t1\nA\tX\t1\n");

  auto imported = SouffleExporter::ReadDerivedRelations(test_dir_, base_facts);
  ASSERT_TRUE(imported.ok()) << imported.status().message();
  const FactTuple *a_writes_x =
      Find(*imported, FactRelation::kMayWrite, {"A", "X"});
  ASSERT_NE(a_writes_x, nullptr);
  EXPECT_EQ(a_writes_x->rule_id, "m8.may_write.transitive.v1");
  EXPECT_EQ(a_writes_x->input_tuple_ids.size(), 2u);
  EXPECT_EQ(a_writes_x->tuple_id.kind, core::IdKind::kFact);
}

TEST_F(SouffleExporterTest, RejectsMalformedDerivedRowWithLineNumber) {
  WriteFile(test_dir_ / "ReachableCall.csv", "A\tB\n");
  WriteFile(test_dir_ / "MayWrite.csv", "");

  auto imported = SouffleExporter::ReadDerivedRelations(test_dir_, {});

  ASSERT_FALSE(imported.ok());
  EXPECT_NE(imported.status().message().find("ReachableCall line 1"),
            std::string::npos);
}

TEST_F(SouffleExporterTest, RejectsUnsupportedEpistemicValue) {
  WriteFile(test_dir_ / "ReachableCall.csv", "A\tB\t6\n");
  WriteFile(test_dir_ / "MayWrite.csv", "");

  auto imported = SouffleExporter::ReadDerivedRelations(test_dir_, {});

  ASSERT_FALSE(imported.ok());
  EXPECT_NE(imported.status().message().find("unsupported epistemic"),
            std::string::npos);
}

TEST_F(SouffleExporterTest, DuplicateSemanticRowsKeepWeakerState) {
  auto must = BaseFact(FactRelation::kDirectCall, {"A", "B"}, "must");
  auto rebuilt_may = MakeBaseFact(FactRelation::kDirectCall, {"A", "B"},
                                  summary::v1::EPISTEMIC_STATE_MAY,
                                  {.function_summary_id = SummaryId("summary"),
                                   .anchor = "may",
                                   .provenance_ref = "test"});
  ASSERT_TRUE(rebuilt_may.ok());
  auto may = std::move(*rebuilt_may);
  const std::array base_facts{must, may};
  WriteFile(test_dir_ / "ReachableCall.csv", "A\tB\t1\nA\tB\t2\n");
  WriteFile(test_dir_ / "MayWrite.csv", "");

  auto imported = SouffleExporter::ReadDerivedRelations(test_dir_, base_facts);

  ASSERT_TRUE(imported.ok()) << imported.status().message();
  ASSERT_EQ(imported->size(), 1u);
  EXPECT_EQ((*imported)[0].epistemic, summary::v1::EPISTEMIC_STATE_MAY);
  EXPECT_EQ((*imported)[0].input_tuple_ids,
            std::vector<core::StableId>{may.tuple_id});
}

TEST_F(SouffleExporterTest, SelectsCanonicalProofAcrossMultiplePaths) {
  const auto ab = BaseFact(FactRelation::kDirectCall, {"A", "B"}, "A:B");
  const auto bd = BaseFact(FactRelation::kDirectCall, {"B", "D"}, "B:D");
  const auto ac = BaseFact(FactRelation::kDirectCall, {"A", "C"}, "A:C");
  const auto cd = BaseFact(FactRelation::kDirectCall, {"C", "D"}, "C:D");
  const std::array base_facts{ab, bd, ac, cd};
  WriteFile(test_dir_ / "ReachableCall.csv",
            "A\tD\t1\nC\tD\t1\nA\tC\t1\nB\tD\t1\nA\tB\t1\n");
  WriteFile(test_dir_ / "MayWrite.csv", "");

  auto first = SouffleExporter::ReadDerivedRelations(test_dir_, base_facts);
  ASSERT_TRUE(first.ok()) << first.status().message();
  const FactTuple *first_ad =
      Find(*first, FactRelation::kReachableCall, {"A", "D"});
  ASSERT_NE(first_ad, nullptr);

  auto bd_reachable = MakeDerivedFact(FactRelation::kReachableCall, {"B", "D"},
                                      summary::v1::EPISTEMIC_STATE_MUST,
                                      "m8.reachable.direct.v1", {bd.tuple_id});
  auto cd_reachable = MakeDerivedFact(FactRelation::kReachableCall, {"C", "D"},
                                      summary::v1::EPISTEMIC_STATE_MUST,
                                      "m8.reachable.direct.v1", {cd.tuple_id});
  ASSERT_TRUE(bd_reachable.ok());
  ASSERT_TRUE(cd_reachable.ok());
  const auto through_b = SortedIds({ab.tuple_id, bd_reachable->tuple_id});
  const auto through_c = SortedIds({ac.tuple_id, cd_reachable->tuple_id});
  EXPECT_EQ(first_ad->input_tuple_ids, std::min(through_b, through_c));

  const std::array reversed_base_facts{cd, ac, bd, ab};
  WriteFile(test_dir_ / "ReachableCall.csv",
            "A\tB\t1\nB\tD\t1\nA\tC\t1\nC\tD\t1\nA\tD\t1\n");
  auto second =
      SouffleExporter::ReadDerivedRelations(test_dir_, reversed_base_facts);
  ASSERT_TRUE(second.ok()) << second.status().message();
  const FactTuple *second_ad =
      Find(*second, FactRelation::kReachableCall, {"A", "D"});
  ASSERT_NE(second_ad, nullptr);
  EXPECT_EQ(second_ad->tuple_id, first_ad->tuple_id);
  EXPECT_EQ(second_ad->input_tuple_ids, first_ad->input_tuple_ids);
}

TEST_F(SouffleExporterTest, RejectsRecursiveProofWithoutDirectSeed) {
  const std::array base_facts{
      BaseFact(FactRelation::kDirectCall, {"A", "B"}, "A:B"),
      BaseFact(FactRelation::kDirectCall, {"B", "A"}, "B:A"),
  };
  WriteFile(test_dir_ / "ReachableCall.csv", "A\tC\t1\nB\tC\t1\n");
  WriteFile(test_dir_ / "MayWrite.csv", "");

  auto imported = SouffleExporter::ReadDerivedRelations(test_dir_, base_facts);

  ASSERT_FALSE(imported.ok());
  EXPECT_NE(imported.status().message().find("no acyclic provenance proof"),
            std::string::npos);
}

} // namespace
} // namespace veritas::facts
