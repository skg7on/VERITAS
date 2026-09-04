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

// WpaDifferentialQualificationTest.cpp — compiled Souffle and the C++
// conformance oracle publish the same canonical facts over byte-identical
// engine-neutral logical input, across the reachability and memory-effect
// shapes the qualification matrix exercises.

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "WpaQualificationSupport.h"

namespace veritas::wpa::qualification {
namespace {

struct QualificationCase {
  std::string name;
  WpaComponentKind component;
  std::string root;
};

struct Program {
  std::vector<summary::SummaryArtifact> artifacts;
  std::string root;
};

Program ProgramFor(const QualificationCase& c) {
  if (c.name == "direct") {
    auto a = V2Summary("a");
    AddDirectCall(&a, "a", "b");
    auto b = V2Summary("b");
    AddDirectCall(&b, "b", "c");
    return {{a, b, V2Summary("c")}, "a"};
  }
  if (c.name == "recursive") {
    auto f = V2Summary("f");
    AddDirectCall(&f, "f", "g");
    auto g = V2Summary("g");
    AddDirectCall(&g, "g", "f");
    AddDirectCall(&g, "g", "h");
    return {{f, g, V2Summary("h")}, "f"};
  }
  if (c.name == "function_pointer") {
    auto invoke = V2Summary("invoke");
    AddIndirectCall(&invoke, "invoke", "target");
    return {{invoke, V2Summary("target")}, "invoke"};
  }
  if (c.name == "callback") {
    auto dispatch = V2Summary("dispatch");
    AddIndirectCall(&dispatch, "dispatch", "handler_a");
    AddIndirectCall(&dispatch, "dispatch", "handler_b");
    return {{dispatch, V2Summary("handler_a"), V2Summary("handler_b")},
            "dispatch"};
  }
  if (c.name == "memory") {
    auto writer = V2Summary("writer");
    AddMemoryWrite(&writer, "mem:buffer", /*known_range=*/true);
    return {{writer}, "writer"};
  }
  return {};
}

class WpaDifferentialQualificationTest
    : public ::testing::TestWithParam<QualificationCase> {};

TEST_P(WpaDifferentialQualificationTest, SouffleEqualsCppOracle) {
  const auto program = ProgramFor(GetParam());
  auto logical = InputFor(program.artifacts, GetParam().component, program.root);
  ASSERT_TRUE(logical.ok()) << logical.status().message();

  auto pair = RunBothEngines(*logical);
  ASSERT_TRUE(pair.ok()) << pair.status().message();

  // The two runs must be distinct (different engine identity => different
  // run ID) but agree on every published fact and on the externally visible
  // hash, over byte-identical logical input.
  EXPECT_EQ(pair->souffle.facts, pair->cpp.facts);
  EXPECT_EQ(pair->souffle.external_hash, pair->cpp.external_hash);
  EXPECT_EQ(pair->souffle.fixpoint_hash, pair->cpp.fixpoint_hash);
  // The case must actually derive something, not merely agree on an empty
  // result.
  EXPECT_FALSE(pair->souffle.facts.empty());
}

TEST(WpaDifferentialQualificationTest, UnknownRangeIsLosslessAcrossEngines) {
  auto writer = V2Summary("writer");
  AddMemoryWrite(&writer, "mem:unknown", /*known_range=*/false);

  auto logical = InputFor({writer}, WpaComponentKind::kMemoryEffects, "writer");
  ASSERT_TRUE(logical.ok()) << logical.status().message();

  EXPECT_TRUE(ContainsRangeKind(*logical, sem::ByteRangeKind::kUnknown));

  auto pair = RunBothEngines(*logical);
  ASSERT_TRUE(pair.ok()) << pair.status().message();
  EXPECT_EQ(pair->souffle.facts, pair->cpp.facts);
  EXPECT_EQ(pair->souffle.external_hash, pair->cpp.external_hash);
  EXPECT_FALSE(pair->souffle.facts.empty());
}

INSTANTIATE_TEST_SUITE_P(
    M9Entry, WpaDifferentialQualificationTest,
    ::testing::Values(
        QualificationCase{"direct", WpaComponentKind::kReachability, "a"},
        QualificationCase{"recursive", WpaComponentKind::kReachability, "f"},
        QualificationCase{"function_pointer", WpaComponentKind::kReachability,
                          "invoke"},
        QualificationCase{"callback", WpaComponentKind::kReachability,
                          "dispatch"},
        QualificationCase{"memory", WpaComponentKind::kMemoryEffects,
                          "writer"}));

}  // namespace
}  // namespace veritas::wpa::qualification
