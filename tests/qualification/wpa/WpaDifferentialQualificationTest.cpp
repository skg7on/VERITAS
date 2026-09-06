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
  // A read closure: a direct read and a read reached through a call, so the
  // transitive MayRead rule is exercised alongside the direct one.
  if (c.name == "memory_read") {
    auto reader = V2Summary("reader");
    AddMemoryRead(&reader, "mem:buffer", /*known_range=*/true);
    AddDirectCall(&reader, "reader", "callee");
    auto callee = V2Summary("callee");
    AddMemoryRead(&callee, "mem:shared", /*known_range=*/true);
    return {{reader, callee}, "reader"};
  }
  // A value-flow closure: two local flows compose transitively, and a
  // parameter flow seeds a third base edge, so the transitive and parameter
  // GlobalFlow rules are exercised alongside the local base rule.
  if (c.name == "flow") {
    auto f = V2Summary("f");
    AddLocalFlow(&f, "v:0", "v:1");
    AddLocalFlow(&f, "v:1", "v:2");
    AddParameterFlow(&f, "cs:1", "v:2", "v:3");
    return {{f}, "f"};
  }
  // An unresolved call yields an unknown effect, and the coverage certificate
  // marks the function incomplete.
  if (c.name == "effects") {
    auto f = V2Summary("f");
    AddUnknownCall(&f, "f", "unresolved");
    return {{f}, "f"};
  }
  // A direct call to a function with no summary (external, unmodeled) yields
  // an unmodeled_external unknown effect.
  if (c.name == "effects_external") {
    auto f = V2Summary("f");
    AddDirectCall(&f, "f", "ext");
    return {{f}, "f"};
  }
  // An unsupported construct yields an unsupported_feature unknown effect.
  if (c.name == "effects_feature") {
    auto f = V2Summary("f");
    AddUnknown(&f, "unsupported_construct");
    return {{f}, "f"};
  }
  // The mixed C/C++ semantic_zoo corpus's recursion shapes: a self-recursive
  // function and a mutually recursive pair, sharing a leaf. The entry point
  // lives in the mutual/self-recursive SCC, so the derived reachability facts
  // exercise both cycle shapes at once.
  if (c.name == "semantic_zoo_recursive") {
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

    return {{entry, self, even, odd, V2Summary("writing_leaf")},
            "zoo_recursive_entry"};
  }
  // A function-pointer formal parameter invokes one of several admissible
  // callback targets: callback dispatch with MAY epistemic, matching the
  // semantic_zoo callback-parameter classification.
  if (c.name == "semantic_zoo_callback") {
    auto parameter = V2Summary("zoo_callback_parameter");
    AddCallbackCall(&parameter, "zoo_callback_parameter", "zoo_callback_left");
    AddCallbackCall(&parameter, "zoo_callback_parameter", "zoo_callback_right");
    return {{parameter, V2Summary("zoo_callback_left"),
             V2Summary("zoo_callback_right")},
            "zoo_callback_parameter"};
  }
  // Virtual dispatch through a base pointer resolves to two admissible
  // overrides (single and multiple inheritance): virtual dispatch with MAY
  // epistemic, matching the semantic_zoo virtual-dispatch shapes.
  if (c.name == "semantic_zoo_virtual") {
    auto select = V2Summary("zoo_virtual_select");
    AddVirtualCall(&select, "zoo_virtual_select", "zoo_single_override");
    AddVirtualCall(&select, "zoo_virtual_select", "zoo_multiple_override");
    return {{select, V2Summary("zoo_single_override"),
             V2Summary("zoo_multiple_override")},
            "zoo_virtual_select"};
  }
  // The semantic_zoo memory shapes: one function writes several distinct
  // memory locations, mixing known and unknown byte ranges (globals, stack
  // slots, and an aliased/overlapping access).
  if (c.name == "semantic_zoo_memory") {
    auto shapes = V2Summary("zoo_memory_shapes");
    AddMemoryWrite(&shapes, "mem:zoo_global", /*known_range=*/true);
    AddMemoryWrite(&shapes, "mem:zoo_static", /*known_range=*/true);
    AddMemoryWrite(&shapes, "mem:zoo_stack", /*known_range=*/true);
    AddMemoryWrite(&shapes, "mem:zoo_union_overlap", /*known_range=*/false);
    AddMemoryWrite(&shapes, "mem:zoo_alias", /*known_range=*/false);
    return {{shapes}, "zoo_memory_shapes"};
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
  EXPECT_EQ(pair->souffle.witnesses, pair->cpp.witnesses);
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
                          "writer"},
        QualificationCase{"memory_read", WpaComponentKind::kMemoryEffects,
                          "reader"},
        QualificationCase{"flow", WpaComponentKind::kFlow, "f"},
        QualificationCase{"effects", WpaComponentKind::kEffects, "f"},
        QualificationCase{"effects_external", WpaComponentKind::kEffects, "f"},
        QualificationCase{"effects_feature", WpaComponentKind::kEffects, "f"},
        QualificationCase{"semantic_zoo_recursive",
                          WpaComponentKind::kReachability,
                          "zoo_recursive_entry"},
        QualificationCase{"semantic_zoo_callback",
                          WpaComponentKind::kReachability,
                          "zoo_callback_parameter"},
        QualificationCase{"semantic_zoo_virtual", WpaComponentKind::kReachability,
                          "zoo_virtual_select"},
        QualificationCase{"semantic_zoo_memory", WpaComponentKind::kMemoryEffects,
                          "zoo_memory_shapes"}));

}  // namespace
}  // namespace veritas::wpa::qualification
