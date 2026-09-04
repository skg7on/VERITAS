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

// WpaDeterminismQualificationTest.cpp — summary discovery order must not
// change the materialized logical input, the derived facts, the selected
// witnesses, or the fixpoint hash. Determinism is a property of the pipeline
// (materializer + canonicalizer), so this exercises the in-process C++
// conformance engine over many permutations; the differential test already
// pins Souffle to the same canonical output.

#include <chrono>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "WpaQualificationSupport.h"

namespace veritas::wpa::qualification {
namespace {

std::vector<summary::SummaryArtifact> DeterminismProgram() {
  auto a = V2Summary("a");
  AddDirectCall(&a, "a", "b");
  auto b = V2Summary("b");
  AddDirectCall(&b, "b", "c");
  auto c = V2Summary("c");
  AddDirectCall(&c, "c", "d");
  auto d = V2Summary("d");
  AddDirectCall(&d, "d", "e");
  return {a, b, c, d, V2Summary("e")};
}

StatusOr<facts::CanonicalizedResult> RunPermutation(std::uint32_t seed) {
  auto shuffled = DeterminismProgram();
  std::mt19937 rng(seed);
  std::ranges::shuffle(shuffled, rng);

  auto logical =
      InputFor(shuffled, WpaComponentKind::kReachability, "a");
  if (!logical.ok())
    return logical.status();

  const auto manifest = MakeManifest(facts::EngineIdentity::kCppConformance,
                                     "cpp-toolchain");
  WpaExecutionEnvelope envelope{manifest, *logical};
  auto cpp = CppConformanceExecutor::Create(
      facts::EngineIdentity::kCppConformance, "cpp-toolchain");
  if (!cpp.ok())
    return cpp.status();
  const WpaExecutionLimits limits{std::chrono::seconds(30), 0, 1};
  auto raw = cpp->Execute(envelope, limits);
  if (!raw.ok())
    return raw.status();
  return Canonicalize(*logical, *raw);
}

TEST(WpaDeterminismQualificationTest, AllInputPermutationsHaveOneResult) {
  auto canonical = RunPermutation(0);
  ASSERT_TRUE(canonical.ok()) << canonical.status().message();

  for (std::uint32_t seed = 1; seed <= 64; ++seed) {
    auto permuted = RunPermutation(seed);
    ASSERT_TRUE(permuted.ok()) << permuted.status().message();
    EXPECT_EQ(permuted->facts, canonical->facts);
    EXPECT_EQ(permuted->witnesses, canonical->witnesses);
    EXPECT_EQ(permuted->fixpoint_hash, canonical->fixpoint_hash);
  }
}

}  // namespace
}  // namespace veritas::wpa::qualification
