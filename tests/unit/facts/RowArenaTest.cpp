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

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "veritas/facts/AnalysisFact.h"
#include "veritas/facts/RowArena.h"
#include "veritas/facts/Witness.h"

namespace veritas::facts {
namespace {

// One row per cell alternative the encoding must know, so a new alternative
// added without teaching the arena fails here rather than silently colliding
// two rows onto one key.
std::vector<SemanticRow> Corpus() {
  const auto fn = MakeStableId(::veritas::core::IdKind::kFunctionVariant,
                               std::span<const std::byte>{});
  const auto mem = MakeStableId(::veritas::core::IdKind::kMemoryRef,
                                std::span<const std::byte>{});
  std::vector<SemanticRow> rows;
  rows.push_back(SemanticRow{RelationId::kReachableCall, {fn, mem}});
  rows.push_back(SemanticRow{RelationId::kReachableCall,
                             {fn, std::string("a_symbol")}});
  rows.push_back(SemanticRow{RelationId::kReachableCall,
                             {fn, static_cast<std::int64_t>(-7)}});
  rows.push_back(SemanticRow{RelationId::kReachableCall,
                             {fn, static_cast<std::uint64_t>(7)}});
  rows.push_back(SemanticRow{
      RelationId::kReachableCall,
      {fn, analysis::semantic::DispatchKind::kDirect}});
  rows.push_back(SemanticRow{
      RelationId::kReachableCall,
      {fn, analysis::semantic::AliasKind::kMayAlias}});
  rows.push_back(SemanticRow{
      RelationId::kReachableCall,
      {fn, analysis::semantic::ByteRangeKind::kKnown}});
  rows.push_back(SemanticRow{
      RelationId::kReachableCall,
      {fn, analysis::semantic::EpistemicState::kMust}});
  return rows;
}

core::StableId SeededFunctionId(std::uint8_t seed) {
  const std::array<std::byte, 1> bytes{static_cast<std::byte>(seed)};
  return MakeStableId(::veritas::core::IdKind::kFunctionVariant, bytes);
}

// A schema-valid ReachableCall row. ReachableCall is a three-column relation --
// two function ids and an epistemic state -- so the corpus rows above, which
// carry two cells each to cover the encoding, are rows for the arena and not
// rows for the schema: an arena stores a row, `MakeFact` validates one.
SemanticRow ReachableCallRow(std::uint8_t source, std::uint8_t target) {
  return SemanticRow{RelationId::kReachableCall,
                     {SeededFunctionId(source), SeededFunctionId(target),
                      analysis::semantic::EpistemicState::kMust}};
}

TEST(RowArenaTest, DecodeRoundTripsEveryCellAlternative) {
  RowArena arena;
  for (const SemanticRow& row : Corpus()) {
    auto handle = arena.Append(row);
    ASSERT_TRUE(handle.ok()) << handle.status().message();
    auto decoded = arena.Decode(*handle);
    ASSERT_TRUE(decoded.ok()) << decoded.status().message();
    EXPECT_EQ(*decoded, row);
  }
  EXPECT_EQ(arena.size(), Corpus().size());
}

TEST(RowArenaTest, AppendKeyMatchesAppendSemanticKey) {
  RowArena arena;
  for (const SemanticRow& row : Corpus()) {
    auto handle = arena.Append(row);
    ASSERT_TRUE(handle.ok()) << handle.status().message();
    std::string from_arena;
    ASSERT_TRUE(arena.AppendKey(*handle, &from_arena).ok());
    EXPECT_EQ(from_arena, EncodeSemanticKey(row));
  }
}

TEST(RowArenaTest, RowEqualsMatchesSemanticRowEquality) {
  RowArena arena;
  const auto rows = Corpus();
  std::vector<RowHandle> handles;
  for (const SemanticRow& row : rows) {
    auto handle = arena.Append(row);
    ASSERT_TRUE(handle.ok());
    handles.push_back(*handle);
  }
  for (std::size_t i = 0; i < rows.size(); ++i) {
    for (std::size_t j = 0; j < rows.size(); ++j) {
      EXPECT_EQ(arena.RowEquals(handles[i], handles[j]), rows[i] == rows[j])
          << i << " vs " << j;
    }
  }
}

TEST(RowArenaTest, RejectsAHandleOutsideTheArena) {
  RowArena arena;
  auto handle = arena.Append(Corpus().front());
  ASSERT_TRUE(handle.ok());
  const RowHandle past_end{arena.bytes() + 8, 4};
  EXPECT_FALSE(arena.Decode(past_end).ok());
  EXPECT_FALSE(arena.AppendKey(past_end, nullptr).ok());
  EXPECT_FALSE(arena.RowEquals(*handle, past_end));
}

TEST(RowArenaTest, HandlesSurviveReallocation) {
  RowArena arena;
  auto first = arena.Append(Corpus().front());
  ASSERT_TRUE(first.ok());
  for (int i = 0; i < 4096; ++i) {
    ASSERT_TRUE(arena.Append(Corpus()[std::size_t(i) % Corpus().size()]).ok());
  }
  auto decoded = arena.Decode(*first);
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(*decoded, Corpus().front());
}

TEST(RowArenaTest, PayloadFormsRoundTripAFactAndAWitness) {
  const SemanticRow fact_row = ReachableCallRow(0, 1);
  auto fact = MakeFact(fact_row);
  ASSERT_TRUE(fact.ok()) << fact.status().message();

  RowArena facts;
  auto fact_handles = facts.AppendFact(*fact);
  ASSERT_TRUE(fact_handles.ok()) << fact_handles.status().message();
  auto decoded_fact = facts.DecodeFact(fact_handles->entry);
  ASSERT_TRUE(decoded_fact.ok()) << decoded_fact.status().message();
  EXPECT_EQ(decoded_fact->fact_id, fact->fact_id);
  EXPECT_EQ(decoded_fact->row, fact->row);

  // The row sub-range must name the row, not the entry: Task 4 compares a
  // published fact's row against a witness's result row, and only a row
  // spelling decodes as a row.
  auto row_only = facts.Decode(fact_handles->row);
  ASSERT_TRUE(row_only.ok()) << row_only.status().message();
  EXPECT_EQ(*row_only, fact->row);

  // The entry spelling is not a row, so it cannot be mistaken for one.
  EXPECT_FALSE(facts.Decode(fact_handles->entry).ok());

  // The key is rendered from a row that sits past the entry's id, which is the
  // spelling `DeriveBatchId` addresses when it hashes a stored batch.
  std::string row_key;
  ASSERT_TRUE(facts.AppendKey(fact_handles->row, &row_key).ok());
  EXPECT_EQ(row_key, EncodeSemanticKey(fact->row));

  const WitnessEdge edge{.result = SemanticKey{fact_row},
                         .rule_id = "direct",
                         .derivation_key = "d",
                         .input = SemanticKey{ReachableCallRow(1, 2)},
                         .input_ordinal = 2};
  RowArena witnesses;
  auto edge_handles = witnesses.AppendWitness(edge);
  ASSERT_TRUE(edge_handles.ok()) << edge_handles.status().message();
  auto decoded_edge = witnesses.DecodeWitness(edge_handles->entry);
  ASSERT_TRUE(decoded_edge.ok()) << decoded_edge.status().message();
  EXPECT_EQ(*decoded_edge, edge);

  // Cross-arena comparison, which is the shape validation uses: the witness's
  // result row equals the fact's row when both name the same row, and differs
  // when they do not.
  EXPECT_TRUE(RowArena::RowsEqual(witnesses, edge_handles->result_row, facts,
                                  fact_handles->row));
  EXPECT_FALSE(RowArena::RowsEqual(witnesses, edge_handles->input_row, facts,
                                   fact_handles->row));
}

TEST(RowArenaTest, HandleAtAddressesAnEntryByPosition) {
  RowArena arena;
  for (const SemanticRow& row : Corpus()) {
    ASSERT_TRUE(arena.Append(row).ok());
  }
  for (std::size_t i = 0; i < Corpus().size(); ++i) {
    auto handle = arena.handle_at(i);
    ASSERT_TRUE(handle.ok()) << handle.status().message();
    auto decoded = arena.Decode(*handle);
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(*decoded, Corpus()[i]);
  }
  EXPECT_FALSE(arena.handle_at(Corpus().size()).ok());
}

TEST(RowArenaTest, RejectsANonCanonicalDigestWithoutLeavingBytesBehind) {
  RowArena arena;
  ASSERT_TRUE(arena.Append(Corpus().front()).ok());
  const std::size_t before = arena.bytes();

  // Upper-case hex is the same number and not the canonical spelling. A
  // rejected append must leave the buffer where it was: a partial row would be
  // bytes the next handle silently overlapped, and the next append is what
  // finds out.
  SemanticRow bad = Corpus().front();
  auto& digest = std::get<core::StableId>(bad.cells[0]).digest_hex;
  digest = "A" + digest.substr(1);
  EXPECT_FALSE(arena.Append(bad).ok());
  EXPECT_EQ(arena.bytes(), before);
  EXPECT_EQ(arena.size(), 1u);

  auto handle = arena.Append(Corpus()[1]);
  ASSERT_TRUE(handle.ok());
  auto decoded = arena.Decode(*handle);
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(*decoded, Corpus()[1]);
}

TEST(RowArenaTest, RejectsAFactWhoseIdIsNotHexWithoutKeepingItsEntry) {
  RowArena arena;
  AnalysisFact fact = *MakeFact(ReachableCallRow(0, 1));
  // The id is appended before the row and the digest fails only part way
  // through it, so this is the append that has to roll its own bytes back.
  fact.fact_id.digest_hex = std::string(64, 'Z');
  EXPECT_FALSE(arena.AppendFact(fact).ok());
  EXPECT_EQ(arena.bytes(), 0u);
  EXPECT_EQ(arena.size(), 0u);
  EXPECT_FALSE(arena.handle_at(0).ok());
}

// The two ranges are the readers Task 2 and Task 3 stream through, and the
// handles their iterators expose are what Task 4 compares: an entry that only
// needs to be identified is never decoded.

TEST(RowArenaTest, AnalysisFactRangeYieldsEveryStoredFact) {
  auto first = MakeFact(ReachableCallRow(0, 1));
  auto second = MakeFact(ReachableCallRow(1, 2));
  ASSERT_TRUE(first.ok()) << first.status().message();
  ASSERT_TRUE(second.ok()) << second.status().message();

  RowArena facts;
  ASSERT_TRUE(facts.AppendFact(*first).ok());
  ASSERT_TRUE(facts.AppendFact(*second).ok());

  const AnalysisFactRange range(&facts);
  EXPECT_FALSE(range.empty());
  EXPECT_EQ(range.size(), 2u);

  std::vector<core::StableId> ids;
  std::size_t index = 0;
  for (auto it = range.begin(); it != range.end(); ++it) {
    EXPECT_EQ(it.handle(), *facts.handle_at(index));
    EXPECT_EQ(it.row_handle(), *facts.fact_row_handle_at(index));
    auto row = facts.Decode(it.row_handle());
    ASSERT_TRUE(row.ok()) << row.status().message();
    EXPECT_EQ(*row, (*it).row) << index;
    ids.push_back((*it).fact_id);
    ++index;
  }
  ASSERT_EQ(ids.size(), 2u);
  EXPECT_EQ(ids[0], first->fact_id);
  EXPECT_EQ(ids[1], second->fact_id);

  const RowArena empty;
  EXPECT_TRUE(AnalysisFactRange(&empty).empty());
  EXPECT_EQ(AnalysisFactRange(&empty).size(), 0u);
}

TEST(RowArenaTest, WitnessRangeYieldsEveryStoredEdgeAndItsRowHandles) {
  const WitnessEdge first{.result = SemanticKey{ReachableCallRow(0, 1)},
                          .rule_id = "direct",
                          .derivation_key = "d",
                          .input = SemanticKey{ReachableCallRow(1, 2)},
                          .input_ordinal = 1};
  const WitnessEdge second{.result = SemanticKey{ReachableCallRow(1, 2)},
                           .rule_id = "indirect",
                           .derivation_key = "e",
                           .input = SemanticKey{ReachableCallRow(0, 1)},
                           .input_ordinal = 0};
  const std::vector<WitnessEdge> expected{first, second};

  RowArena witnesses;
  for (const WitnessEdge& edge : expected) {
    ASSERT_TRUE(witnesses.AppendWitness(edge).ok());
  }

  const WitnessRange range(&witnesses);
  EXPECT_EQ(range.size(), 2u);

  std::size_t index = 0;
  for (auto it = range.begin(); it != range.end(); ++it) {
    EXPECT_EQ(it.handle(), *witnesses.handle_at(index));
    EXPECT_EQ(it.result_row_handle(),
              *witnesses.witness_result_row_handle_at(index));
    EXPECT_EQ(it.input_row_handle(),
              *witnesses.witness_input_row_handle_at(index));
    auto result = witnesses.Decode(it.result_row_handle());
    auto input = witnesses.Decode(it.input_row_handle());
    ASSERT_TRUE(result.ok()) << result.status().message();
    ASSERT_TRUE(input.ok()) << input.status().message();
    EXPECT_EQ(*result, expected[index].result.row) << index;
    EXPECT_EQ(*input, expected[index].input.row) << index;
    EXPECT_EQ(*it, expected[index]) << index;
    ++index;
  }
  EXPECT_EQ(index, 2u);

  // A range over a facts arena is the one way to reach a witness entry that is
  // not there, and it reports it rather than reading a row as a witness.
  EXPECT_FALSE(witnesses.witness_result_row_handle_at(2).ok());
  EXPECT_FALSE(witnesses.witness_input_row_handle_at(2).ok());
}

}  // namespace
}  // namespace veritas::facts
