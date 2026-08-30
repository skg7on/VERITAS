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
#include <optional>
#include <set>
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

FactTuple BaseFactWithOrigin(FactRelation relation,
                             std::vector<std::string> columns,
                             std::string anchor) {
  auto tuple = MakeBaseFact(
      relation, std::move(columns), summary::v1::EPISTEMIC_STATE_MUST,
      {.function_summary_id =
           core::StableId{core::IdKind::kFunctionSummary, std::string(64, 'a')},
       .anchor = std::move(anchor),
       .provenance_ref = "p"});
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

TEST_F(SouffleExporterTest, CyclicAlternateProofUsesMinimumRootedRank) {
  const auto zero_b = BaseFact(FactRelation::kDirectCall, {"0", "B"}, "0:B");
  const auto b_a = BaseFact(FactRelation::kDirectCall, {"B", "A"}, "B:A");
  const auto a_b = BaseFact(FactRelation::kDirectCall, {"A", "B"}, "A:B");
  const auto c_z = BaseFact(FactRelation::kDirectCall, {"C", "Z"}, "C:Z");
  const auto d_z = BaseFact(FactRelation::kDirectCall, {"D", "Z"}, "D:Z");
  auto c_reachable = MakeDerivedFact(FactRelation::kReachableCall, {"C", "Z"},
                                     summary::v1::EPISTEMIC_STATE_MUST,
                                     "m8.reachable.direct.v1", {c_z.tuple_id});
  auto d_reachable = MakeDerivedFact(FactRelation::kReachableCall, {"D", "Z"},
                                     summary::v1::EPISTEMIC_STATE_MUST,
                                     "m8.reachable.direct.v1", {d_z.tuple_id});
  ASSERT_TRUE(c_reachable.ok());
  ASSERT_TRUE(d_reachable.ok());

  std::optional<FactTuple> b_c;
  std::optional<FactTuple> a_d;
  std::optional<FactTuple> b_reachable;
  std::vector<core::StableId> expected_a_inputs;
  for (int attempt = 0; attempt < 10000 && !a_d.has_value(); ++attempt) {
    auto candidate_b_c = BaseFact(FactRelation::kDirectCall, {"B", "C"},
                                  "B:C:" + std::to_string(attempt));
    auto candidate_a_d = BaseFact(FactRelation::kDirectCall, {"A", "D"},
                                  "A:D:" + std::to_string(attempt));
    const auto b_frontier =
        SortedIds({candidate_b_c.tuple_id, c_reachable->tuple_id});
    const auto a_frontier =
        SortedIds({candidate_a_d.tuple_id, d_reachable->tuple_id});
    if (!(b_frontier < a_frontier))
      continue;
    auto candidate_b_reachable =
        MakeDerivedFact(FactRelation::kReachableCall, {"B", "Z"},
                        summary::v1::EPISTEMIC_STATE_MUST,
                        "m8.reachable.transitive.v1", b_frontier);
    ASSERT_TRUE(candidate_b_reachable.ok());
    const auto through_b =
        SortedIds({a_b.tuple_id, candidate_b_reachable->tuple_id});
    if (!(through_b < a_frontier))
      continue;
    b_c = std::move(candidate_b_c);
    a_d = std::move(candidate_a_d);
    b_reachable = std::move(*candidate_b_reachable);
    expected_a_inputs = a_frontier;
  }
  ASSERT_TRUE(b_c.has_value());
  ASSERT_TRUE(a_d.has_value());
  ASSERT_TRUE(b_reachable.has_value());

  const std::vector base_facts{zero_b, b_a, *b_c, a_b, *a_d, c_z, d_z};
  WriteFile(test_dir_ / "ReachableCall.csv",
            "0\tZ\t1\nA\tZ\t1\nB\tZ\t1\nC\tZ\t1\nD\tZ\t1\n");
  WriteFile(test_dir_ / "MayWrite.csv", "");

  auto imported = SouffleExporter::ReadDerivedRelations(test_dir_, base_facts);
  ASSERT_TRUE(imported.ok()) << imported.status().message();
  const FactTuple *a_reaches_z =
      Find(*imported, FactRelation::kReachableCall, {"A", "Z"});
  ASSERT_NE(a_reaches_z, nullptr);
  EXPECT_EQ(a_reaches_z->input_tuple_ids, expected_a_inputs);

  std::set<core::StableId> available_ids;
  for (const auto &fact : base_facts)
    available_ids.insert(fact.tuple_id);
  for (const auto &fact : *imported)
    available_ids.insert(fact.tuple_id);
  for (const auto &fact : *imported) {
    for (const auto &input_id : fact.input_tuple_ids) {
      EXPECT_TRUE(available_ids.contains(input_id));
    }
  }
}

TEST_F(SouffleExporterTest,
       ShorterRootedProofPrecedesLexicographicallySmallerLongerProof) {
  const auto a_b = BaseFact(FactRelation::kDirectCall, {"A", "B"}, "A:B");
  const auto c_z = BaseFact(FactRelation::kDirectCall, {"C", "Z"}, "C:Z");
  const auto d_z = BaseFact(FactRelation::kDirectCall, {"D", "Z"}, "D:Z");
  auto c_reachable = MakeDerivedFact(FactRelation::kReachableCall, {"C", "Z"},
                                     summary::v1::EPISTEMIC_STATE_MUST,
                                     "m8.reachable.direct.v1", {c_z.tuple_id});
  auto d_reachable = MakeDerivedFact(FactRelation::kReachableCall, {"D", "Z"},
                                     summary::v1::EPISTEMIC_STATE_MUST,
                                     "m8.reachable.direct.v1", {d_z.tuple_id});
  ASSERT_TRUE(c_reachable.ok());
  ASSERT_TRUE(d_reachable.ok());

  std::optional<FactTuple> b_c;
  std::optional<FactTuple> a_d;
  std::vector<core::StableId> expected_a_inputs;
  for (int attempt = 0; attempt < 100000 && !a_d.has_value(); ++attempt) {
    auto candidate_b_c = BaseFact(FactRelation::kDirectCall, {"B", "C"},
                                  "B:C:" + std::to_string(attempt));
    auto candidate_a_d = BaseFact(FactRelation::kDirectCall, {"A", "D"},
                                  "A:D:" + std::to_string(attempt));
    const auto b_frontier =
        SortedIds({candidate_b_c.tuple_id, c_reachable->tuple_id});
    const auto a_frontier =
        SortedIds({candidate_a_d.tuple_id, d_reachable->tuple_id});
    if (!(a_frontier < b_frontier))
      continue;
    auto candidate_b_reachable =
        MakeDerivedFact(FactRelation::kReachableCall, {"B", "Z"},
                        summary::v1::EPISTEMIC_STATE_MUST,
                        "m8.reachable.transitive.v1", b_frontier);
    ASSERT_TRUE(candidate_b_reachable.ok());
    const auto through_b =
        SortedIds({a_b.tuple_id, candidate_b_reachable->tuple_id});
    if (!(through_b < a_frontier))
      continue;
    b_c = std::move(candidate_b_c);
    a_d = std::move(candidate_a_d);
    expected_a_inputs = a_frontier;
  }
  ASSERT_TRUE(b_c.has_value());
  ASSERT_TRUE(a_d.has_value());

  const std::vector base_facts{a_b, *b_c, *a_d, c_z, d_z};
  WriteFile(test_dir_ / "ReachableCall.csv",
            "A\tZ\t1\nB\tZ\t1\nC\tZ\t1\nD\tZ\t1\n");
  WriteFile(test_dir_ / "MayWrite.csv", "");

  auto imported = SouffleExporter::ReadDerivedRelations(test_dir_, base_facts);
  ASSERT_TRUE(imported.ok()) << imported.status().message();
  const FactTuple *a_reaches_z =
      Find(*imported, FactRelation::kReachableCall, {"A", "Z"});
  ASSERT_NE(a_reaches_z, nullptr);
  EXPECT_EQ(a_reaches_z->input_tuple_ids, expected_a_inputs);
}

TEST_F(SouffleExporterTest, CyclicAlternativesStillProduceClosedProofForest) {
  const std::vector base_facts{
      BaseFactWithOrigin(FactRelation::kDirectWrite, {"A", "X"}, "w75821-A"),
      BaseFactWithOrigin(FactRelation::kDirectCall, {"A", "C"}, "e75821-A-C"),
      BaseFactWithOrigin(FactRelation::kDirectCall, {"A", "D"}, "e75821-A-D"),
      BaseFactWithOrigin(FactRelation::kDirectCall, {"B", "A"}, "e75821-B-A"),
      BaseFactWithOrigin(FactRelation::kDirectCall, {"C", "B"}, "e75821-C-B"),
      BaseFactWithOrigin(FactRelation::kDirectCall, {"C", "E"}, "e75821-C-E"),
      BaseFactWithOrigin(FactRelation::kDirectCall, {"D", "A"}, "e75821-D-A"),
      BaseFactWithOrigin(FactRelation::kDirectCall, {"D", "C"}, "e75821-D-C"),
      BaseFactWithOrigin(FactRelation::kDirectCall, {"E", "A"}, "e75821-E-A"),
      BaseFactWithOrigin(FactRelation::kDirectCall, {"E", "D"}, "e75821-E-D"),
  };
  WriteFile(test_dir_ / "ReachableCall.csv", "");
  WriteFile(test_dir_ / "MayWrite.csv",
            "A\tX\t1\nB\tX\t1\nC\tX\t1\nD\tX\t1\nE\tX\t1\n");

  auto imported = SouffleExporter::ReadDerivedRelations(test_dir_, base_facts);
  ASSERT_TRUE(imported.ok()) << imported.status().message();
  ASSERT_EQ(imported->size(), 5u);
  std::set<core::StableId> available_ids;
  for (const auto &fact : base_facts)
    available_ids.insert(fact.tuple_id);
  for (const auto &fact : *imported)
    available_ids.insert(fact.tuple_id);
  for (const auto &fact : *imported) {
    for (const auto &input : fact.input_tuple_ids)
      EXPECT_TRUE(available_ids.contains(input));
  }
}

TEST_F(SouffleExporterTest, ReconstructsLongSparseProofChain) {
  constexpr std::size_t kFunctionCount = 512;
  std::vector<std::string> functions;
  functions.reserve(kFunctionCount);
  for (std::size_t index = 0; index < kFunctionCount; ++index)
    functions.push_back("F" + std::to_string(index));

  std::vector<FactTuple> base_facts;
  base_facts.reserve(kFunctionCount);
  for (std::size_t index = 1; index < functions.size(); ++index) {
    base_facts.push_back(BaseFact(FactRelation::kDirectCall,
                                  {functions[index - 1], functions[index]},
                                  "call:" + std::to_string(index)));
  }
  base_facts.push_back(
      BaseFact(FactRelation::kDirectWrite, {functions.back(), "X"}, "write:X"));

  std::vector<FactTuple> semantics;
  semantics.reserve(kFunctionCount);
  for (const auto &function : functions) {
    auto semantic = MakeDerivedFact(FactRelation::kMayWrite, {function, "X"},
                                    summary::v1::EPISTEMIC_STATE_MUST,
                                    "test.semantic.placeholder.v1",
                                    {base_facts.back().tuple_id});
    ASSERT_TRUE(semantic.ok()) << semantic.status().message();
    semantics.push_back(std::move(*semantic));
  }

  auto reconstructed =
      SouffleExporter::ReconstructCanonicalProofs(base_facts, semantics);
  ASSERT_TRUE(reconstructed.ok()) << reconstructed.status().message();
  EXPECT_EQ(reconstructed->size(), kFunctionCount);
}

// Canonical proof reconstruction decides which rule and which inputs prove
// each derived fact, and MakeDerivedFact hashes both into the fact's tuple ID.
// Selection is therefore identity-bearing: a different proof is a different
// fact, and a changed rule ID silently re-identifies every fact derived by it.
//
// The other tests here assert only how many facts came back, so none of that
// is pinned. This fingerprints the full selected proof forest -- relation,
// columns, epistemic, rule ID, and ordered input IDs -- so any change to
// selection or to a rule ID fails loudly instead of quietly renaming facts.
TEST_F(SouffleExporterTest, CanonicalProofSelectionIsPinned) {
  const std::vector<std::string> functions = {"F0", "F1", "F2"};

  std::vector<FactTuple> base_facts;
  base_facts.push_back(BaseFact(FactRelation::kDirectCall,
                                {functions[0], functions[1]}, "call:f-g"));
  base_facts.push_back(BaseFact(FactRelation::kDirectCall,
                                {functions[1], functions[2]}, "call:g-h"));
  // A direct edge that competes with the transitive proof of f -> h.
  base_facts.push_back(BaseFact(FactRelation::kDirectCall,
                                {functions[0], functions[2]}, "call:f-h"));
  base_facts.push_back(BaseFact(FactRelation::kDirectWrite,
                                {functions[2], "X"}, "write:X"));

  std::vector<FactTuple> semantics;
  for (const auto &function : functions) {
    // F2 reaches nothing further, so only F0 and F1 have a ReachableCall to
    // prove; asking for F2 -> F2 would demand a self-loop that no edge backs.
    if (function != functions[2]) {
      auto reachable = MakeDerivedFact(
          FactRelation::kReachableCall, {function, functions[2]},
          summary::v1::EPISTEMIC_STATE_MUST, "test.semantic.placeholder.v1",
          {base_facts.front().tuple_id});
      ASSERT_TRUE(reachable.ok()) << reachable.status().message();
      semantics.push_back(std::move(*reachable));
    }

    auto may_write = MakeDerivedFact(
        FactRelation::kMayWrite, {function, "X"},
        summary::v1::EPISTEMIC_STATE_MUST, "test.semantic.placeholder.v1",
        {base_facts.back().tuple_id});
    ASSERT_TRUE(may_write.ok()) << may_write.status().message();
    semantics.push_back(std::move(*may_write));
  }

  auto reconstructed =
      SouffleExporter::ReconstructCanonicalProofs(base_facts, semantics);
  ASSERT_TRUE(reconstructed.ok()) << reconstructed.status().message();

  std::vector<std::string> fingerprint;
  for (const auto &fact : *reconstructed) {
    auto name = FactRelationName(fact.relation);
    ASSERT_TRUE(name.ok());
    std::string line(*name);
    for (const auto &column : fact.columns)
      line += "|" + column;
    line += "|e" + std::to_string(static_cast<int>(fact.epistemic));
    line += "|" + fact.rule_id;
    for (const auto &input : fact.input_tuple_ids)
      line += "|" + core::ToString(input);
    line += "|=>" + core::ToString(fact.tuple_id);
    fingerprint.push_back(std::move(line));
  }
  std::ranges::sort(fingerprint);

  // Golden values captured from the reconstruction this test was written to
  // pin. F0 -> F2 selects the direct edge over the two-step transitive proof,
  // which is the shortest-proof rule doing its job.
  const std::vector<std::string> expected = {
      "MayWrite|F0|X|e1|m8.may_write.transitive.v1|"
      "fact:sha256:048f836fc31d4f482e56a6de18fe9eed3bd9a0fa3ed825298d675f998ce6b108|"
      "fact:sha256:ab8c9f952f43a4b700c7824e758f430be944a355d315a1029681eb820c0b8bdc|"
      "=>fact:sha256:f9d2310e46f6b1b8fc542420ba8f3f2c61df81c7e0257882fdd10572d3d9010a",
      "MayWrite|F1|X|e1|m8.may_write.transitive.v1|"
      "fact:sha256:686a5ae0333dff4d94534f42049e231baa96e6b27362290c94966da7d24dbb90|"
      "fact:sha256:ab8c9f952f43a4b700c7824e758f430be944a355d315a1029681eb820c0b8bdc|"
      "=>fact:sha256:4ae3b82446131f5844ecc3c7f80b11318cb47504849a9efc43ca85690fb97d3e",
      "MayWrite|F2|X|e1|m8.may_write.direct.v1|"
      "fact:sha256:943c0ce6de522f71bddbdf8e2732669959a7c16b8e7ec71aa1c7c82d4884b048|"
      "=>fact:sha256:ab8c9f952f43a4b700c7824e758f430be944a355d315a1029681eb820c0b8bdc",
      "ReachableCall|F0|F2|e1|m8.reachable.direct.v1|"
      "fact:sha256:048f836fc31d4f482e56a6de18fe9eed3bd9a0fa3ed825298d675f998ce6b108|"
      "=>fact:sha256:e35a67eda2d789316d4fa5786043f12855e54bc810a289fdd15ba7cdc850a88d",
      "ReachableCall|F1|F2|e1|m8.reachable.direct.v1|"
      "fact:sha256:686a5ae0333dff4d94534f42049e231baa96e6b27362290c94966da7d24dbb90|"
      "=>fact:sha256:6b91a1b3f8f8b88f4f4b02832a9c8ddfd43e8b8e0535c23068a29ff188cf9c72",
  };
  EXPECT_EQ(fingerprint, expected);
}

} // namespace
} // namespace veritas::facts
