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

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace veritas;
using namespace veritas::analysis::semantic;
using namespace veritas::facts;

namespace {

core::StableId FunctionStableId(std::uint8_t seed) {
  const std::array<std::byte, 1> bytes{static_cast<std::byte>(seed)};
  return core::MakeStableId(core::IdKind::kFunctionVariant, bytes);
}

SemanticRow ReachableCallSemanticRow(core::StableId source,
                                     core::StableId target,
                                     EpistemicState epistemic) {
  SemanticRow row;
  row.relation = RelationId::kReachableCall;
  row.cells = {source, target, epistemic};
  return row;
}

core::StableId SeededId(core::IdKind kind, std::uint8_t seed) {
  const std::array<std::byte, 1> bytes{static_cast<std::byte>(seed)};
  return core::MakeStableId(kind, bytes);
}

// The identity contract is the fact ID's exact 64-hex digest, and every fact ID
// in a published store depends on it: a preimage that changes by one byte
// renames every fact downstream of it. These rows cover all eight semantic cell
// kinds, both range payloads, a negative offset, and a string carrying a NUL and
// a colon, so an encoding change is caught on every branch of the encoder rather
// than on the two cell kinds the rest of this suite happens to use.
//
// The expected strings were captured from the implementation as it stood before
// its encoding was rewritten for speed. They are a recording of the contract,
// not a second implementation of it: a test that recomputed the digest would
// agree with any encoder it was written alongside.
struct IdentityGolden {
  const char* name;
  SemanticRow row;
  const char* id;
};

std::vector<IdentityGolden> IdentityGoldens() {
  SemanticRow direct_call;
  direct_call.relation = RelationId::kDirectCall;
  direct_call.cells = {SeededId(core::IdKind::kCallSite, 3),
                       FunctionStableId(1), FunctionStableId(2),
                       DispatchKind::kIndirect, EpistemicState::kMust};

  SemanticRow known_read;
  known_read.relation = RelationId::kDirectRead;
  known_read.cells = {FunctionStableId(1), SeededId(core::IdKind::kMemoryRef, 4),
                      ByteRangeKind::kKnown, std::int64_t{-1},
                      std::uint64_t{19}, EpistemicState::kMust};

  SemanticRow unknown_read;
  unknown_read.relation = RelationId::kDirectRead;
  unknown_read.cells = {FunctionStableId(1), SeededId(core::IdKind::kMemoryRef, 4),
                        ByteRangeKind::kUnknown, std::int64_t{0},
                        std::uint64_t{0}, EpistemicState::kUnknown};

  SemanticRow alias;
  alias.relation = RelationId::kAlias;
  alias.cells = {SeededId(core::IdKind::kMemoryRef, 4),
                 SeededId(core::IdKind::kMemoryRef, 5), AliasKind::kMayAlias,
                 EpistemicState::kInferred};

  SemanticRow local_flow;
  local_flow.relation = RelationId::kLocalFlow;
  local_flow.cells = {FunctionStableId(1), SeededId(core::IdKind::kValueRef, 6),
                      SeededId(core::IdKind::kValueRef, 7),
                      std::string("12:ab\0c", 7), EpistemicState::kMay};

  SemanticRow modeled_effect;
  modeled_effect.relation = RelationId::kModeledEffect;
  modeled_effect.cells = {SeededId(core::IdKind::kModel, 8), FunctionStableId(1),
                          std::string("effect"), std::string("subject"),
                          EpistemicState::kAssumed};

  SemanticRow unsupported_feature;
  unsupported_feature.relation = RelationId::kUnsupportedFeature;
  unsupported_feature.cells = {std::string("node"), std::string("feature"),
                               std::string("policy")};

  SemanticRow fact_map;
  fact_map.relation = RelationId::kFactMap;
  fact_map.cells = {SeededId(core::IdKind::kFact, 9),
                    std::string("fact:sha256:deadbeef")};

  SemanticRow coverage;
  coverage.relation = RelationId::kSoundnessCoverage;
  coverage.cells = {std::string("scope"), std::string("complete"),
                    std::uint64_t{1}, EpistemicState::kMust};

  return {
      {"reachable-call",
       ReachableCallSemanticRow(FunctionStableId(1), FunctionStableId(2),
                                EpistemicState::kMay),
       "fact:sha256:a44d426dbc2457a57a2ecaa08d79297c6b1f01b3a39a9ef8747de182ca66f807"},
      {"direct-call", direct_call,
       "fact:sha256:ce73a23eea22eb2a06a3e00bd9dc990aa097845789ddd22db5f3d0ec4dfa638a"},
      {"direct-read-known", known_read,
       "fact:sha256:c63b59e3066165d2c3f8922b0c21dbef4f99beaef790cf9d3b996d414b0c5413"},
      {"direct-read-unknown", unknown_read,
       "fact:sha256:f4511c10b43f9929d27c639c80ea2ff42db3249b9927dcf4f20663ec6abb53ee"},
      {"alias", alias,
       "fact:sha256:9c527a3001621ec6b77db8bfa3b3f7aa85fc75ac4c629b888fb9794480e3e9e1"},
      {"local-flow", local_flow,
       "fact:sha256:25d6cba6340fe687da157fd63109e738dca7031d1a0b2e6af2173d1b3cb404c0"},
      {"modeled-effect", modeled_effect,
       "fact:sha256:3ba254e287a6cf6bc277f3f153e0cafe9b280465ee19bc577619f284c6e47aa5"},
      {"unsupported-feature", unsupported_feature,
       "fact:sha256:a78ad7b4dcf3b42147bfd2227ecf5f991473fb4a9e80a78abe82eed88f720945"},
      {"fact-map", fact_map,
       "fact:sha256:255a021cbc11afbd428aba7532b5bf01edb368cbb264f0162e743913ba182399"},
      {"soundness-coverage", coverage,
       "fact:sha256:4eccc4649aeea96de33bc53d104745ce9ce8fb4743226d693a029fac9a071fdd"},
  };
}

}  // namespace

TEST(AnalysisFactTest, WitnessDoesNotChangeFactIdentity) {
  SemanticRow row = ReachableCallSemanticRow(FunctionStableId(1),
                                             FunctionStableId(2),
                                             EpistemicState::kMay);
  auto first = MakeFact(row);
  auto second = MakeFact(row);
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  EXPECT_EQ(first->fact_id, second->fact_id);
}

TEST(AnalysisFactTest, DifferentCellsProduceDifferentFactId) {
  auto a = MakeFact(ReachableCallSemanticRow(FunctionStableId(1),
                                             FunctionStableId(2),
                                             EpistemicState::kMay));
  auto b = MakeFact(ReachableCallSemanticRow(FunctionStableId(1),
                                             FunctionStableId(3),
                                             EpistemicState::kMay));
  ASSERT_TRUE(a.ok());
  ASSERT_TRUE(b.ok());
  EXPECT_NE(a->fact_id, b->fact_id);
}

TEST(AnalysisFactTest, RejectsInvalidSemanticRow) {
  SemanticRow row = ReachableCallSemanticRow(FunctionStableId(1),
                                             FunctionStableId(2),
                                             EpistemicState::kMay);
  row.cells[0] = core::MakeStableId(
      core::IdKind::kMemoryRef, std::array<std::byte, 1>{std::byte{0x01}});
  EXPECT_EQ(MakeFact(row).status().code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(DeriveFactId(row).status().code(), StatusCode::kInvalidArgument);
}

// Validation and persistence only need the identity, and paying for a row copy
// per fact is the difference between a bounded and an unbounded peak on a
// million-fact batch. The identity must nevertheless be the same one MakeFact
// assigns, or a batch would validate against IDs it did not publish.
TEST(AnalysisFactTest, DeriveFactIdMatchesMakeFactWithoutCopyingTheRow) {
  const SemanticRow row = ReachableCallSemanticRow(
      FunctionStableId(1), FunctionStableId(2), EpistemicState::kMay);
  auto derived = DeriveFactId(row);
  auto made = MakeFact(row);
  ASSERT_TRUE(derived.ok());
  ASSERT_TRUE(made.ok());
  EXPECT_EQ(*derived, made->fact_id);

  // The row is untouched by identity derivation and is copied verbatim by
  // MakeFact, so the two agree on every field.
  EXPECT_EQ(made->row, row);
}

// The encoding rewrite must not move a single fact ID. This is the guard for
// that: every golden below was produced by the pre-rewrite implementation.
TEST(AnalysisFactTest, DerivedIdentityCoversEveryCellKind) {
  for (const IdentityGolden& golden : IdentityGoldens()) {
    SCOPED_TRACE(golden.name);
    auto derived = DeriveFactId(golden.row);
    ASSERT_TRUE(derived.ok()) << derived.status().message();
    EXPECT_EQ(core::ToString(*derived), golden.id);
  }
}
