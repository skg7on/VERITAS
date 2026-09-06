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

// The mixed C/C++ semantic_zoo corpus at the WPA boundary: mutual and self
// recursion, callback and virtual dispatch, and structured memory writes with
// mixed known/unknown ranges. One artifact set exercises both the reachability
// and memory-effect components.
std::vector<summary::SummaryArtifact> SemanticZooProgram() {
  auto entry = V2Summary("zoo_recursive_entry");
  AddDirectCall(&entry, "zoo_recursive_entry", "self_recursive");
  AddDirectCall(&entry, "zoo_recursive_entry", "mutual_even");

  auto self = V2Summary("self_recursive");
  AddDirectCall(&self, "self_recursive", "self_recursive");
  AddDirectCall(&self, "self_recursive", "writing_leaf");

  auto even = V2Summary("mutual_even");
  AddDirectCall(&even, "mutual_even", "mutual_odd");
  AddDirectCall(&even, "mutual_even", "writing_leaf");

  auto odd = V2Summary("mutual_odd");
  AddDirectCall(&odd, "mutual_odd", "mutual_even");
  AddDirectCall(&odd, "mutual_odd", "writing_leaf");

  auto leaf = V2Summary("writing_leaf");
  AddCallbackCall(&leaf, "writing_leaf", "zoo_callback_left");
  AddCallbackCall(&leaf, "writing_leaf", "zoo_callback_right");

  auto select = V2Summary("zoo_virtual_select");
  AddVirtualCall(&select, "zoo_virtual_select", "zoo_single_override");
  AddVirtualCall(&select, "zoo_virtual_select", "zoo_multiple_override");

  auto shapes = V2Summary("zoo_memory_shapes");
  AddMemoryWrite(&shapes, "mem:zoo_global", /*known_range=*/true);
  AddMemoryWrite(&shapes, "mem:zoo_static", /*known_range=*/true);
  AddMemoryWrite(&shapes, "mem:zoo_stack", /*known_range=*/true);
  AddMemoryWrite(&shapes, "mem:zoo_union_overlap", /*known_range=*/false);
  AddMemoryWrite(&shapes, "mem:zoo_alias", /*known_range=*/false);

  return {entry, self, even, odd, leaf, V2Summary("zoo_callback_left"),
          V2Summary("zoo_callback_right"), select,
          V2Summary("zoo_single_override"), V2Summary("zoo_multiple_override"),
          shapes};
}

StatusOr<facts::CanonicalizedResult> RunPermutation(
    const std::vector<summary::SummaryArtifact>& program,
    WpaComponentKind component, std::string_view root, std::uint32_t seed) {
  auto shuffled = program;
  std::mt19937 rng(seed);
  std::ranges::shuffle(shuffled, rng);

  auto logical = InputFor(shuffled, component, root);
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

// Every discovery-order permutation of one program must yield the same derived
// facts, witnesses, and hashes. Determinism is asserted from the outputs, not
// from a run identity.
void ExpectOneResultAcrossPermutations(
    const std::vector<summary::SummaryArtifact>& program,
    WpaComponentKind component, std::string_view root) {
  auto canonical = RunPermutation(program, component, root, 0);
  ASSERT_TRUE(canonical.ok()) << canonical.status().message();
  EXPECT_FALSE(canonical->facts.empty());

  for (std::uint32_t seed = 1; seed <= 64; ++seed) {
    auto permuted = RunPermutation(program, component, root, seed);
    ASSERT_TRUE(permuted.ok()) << permuted.status().message();
    EXPECT_EQ(permuted->facts, canonical->facts);
    EXPECT_EQ(permuted->witnesses, canonical->witnesses);
    EXPECT_EQ(permuted->fixpoint_hash, canonical->fixpoint_hash);
    EXPECT_EQ(permuted->external_hash, canonical->external_hash);
  }
}

TEST(WpaDeterminismQualificationTest, AllInputPermutationsHaveOneResult) {
  ExpectOneResultAcrossPermutations(DeterminismProgram(),
                                    WpaComponentKind::kReachability, "a");
}

TEST(WpaDeterminismQualificationTest, SemanticZooPermutationsHaveOneResult) {
  const auto program = SemanticZooProgram();
  ExpectOneResultAcrossPermutations(program, WpaComponentKind::kReachability,
                                    "zoo_recursive_entry");
  ExpectOneResultAcrossPermutations(program, WpaComponentKind::kReachability,
                                    "zoo_virtual_select");
  ExpectOneResultAcrossPermutations(program, WpaComponentKind::kMemoryEffects,
                                    "zoo_memory_shapes");
}

}  // namespace
}  // namespace veritas::wpa::qualification
