# M8 SCC-Aware WPA and Souffle Fact Engine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Compute deterministic transitive-call and may-write facts through SCC-aware C++ fixpoint evaluation, persist convergence state, propagate external changes through the M7 scheduler, and provide an optional Souffle execution boundary with provenance-preserving import.

**Architecture:** Function summaries carry stable resolved callee IDs. A deterministic call graph is collapsed into a stable-ID SCC condensation graph and evaluated callee-first by finite monotone C++ domains. SQLite stores SCC topology and three state hashes; the existing M7 scheduler receives predecessor work only when externally visible output changes. Stable fact tuples are exported to Souffle-compatible files and imported with canonical C++ provenance reconstruction; Souffle execution remains optional.

**Tech Stack:** C++20, CMake 3.23+, Protobuf, SQLite, optional Souffle executable, LLVM Support process utilities, GoogleTest.

**Spec:** `docs/specs/milestones/m08-scc-wpa-souffle-fact-engine-design-spec.md`

## Global Constraints

- `UNKNOWN_CALL` and any call without a resolved stable callee never fan out to all functions.
- `MUST` and `MAY` are the only positive call-graph edge states; derivation can weaken but never strengthen epistemic state.
- SCC IDs, tuple IDs, processing order, and state hashes are independent of insertion order, native addresses, timestamps, and wall-clock time.
- SCC state persists input, fixpoint, and externally visible SHA-256 hashes plus iteration count and convergence status.
- The required executable domains are `TransitiveCalls` and `MayWrite`; unsupported domains return `kUnsupported` explicitly.
- Derived tuples carry a stable tuple ID, versioned rule ID, and immediate input tuple IDs before they cross the M9 boundary.
- `VERITAS_ENABLE_SOUFFLE=OFF` and Souffle-absent builds compile and run every C++ WPA and fact-boundary test.
- VERITAS code uses C++20 with RTTI and exceptions disabled; fallible APIs return `Status` or `StatusOr<T>`.
- Every new C++, header, CMake, Datalog, and fixture source file begins with the repository's full Apache-2.0 header.

---

### Task 1: Stable SCC and Resolved Callee Identities

**Files:**
- Modify: `include/veritas/core/Ids.h`
- Modify: `src/core/Ids.cpp`
- Modify: `proto/veritas/summary/v1/summary.proto`
- Modify: `src/analysis/llvm/OriginMap.h`
- Modify: `src/analysis/llvm/OriginMap.cpp`
- Modify: `src/analysis/llvm/LocalFactExtractor.cpp`
- Modify: `src/analysis/svf/SvfMerge.h`
- Modify: `src/analysis/svf/SvfMerge.cpp`
- Modify: `src/analysis/ProjectAnalyzer.cpp`
- Test: `tests/unit/core/IdsTest.cpp`
- Test: `tests/integration/analysis/LocalAnalysisStageTest.cpp`
- Test: `tests/integration/analysis/svf/SvfFactMapperTest.cpp`

**Interfaces:**
- Produces: `core::IdKind::kScc`, serialized as `scc:sha256:<digest>`.
- Produces: `Call.resolved_callee_function_variant_id` at Protobuf field 5.
- Produces: `OriginMap::GetSymbolIdByLlvmName(std::string_view) const`.
- Changes: `MergeSvfFacts(std::vector<summary::v1::FunctionSummary> drafts,
  const SvfFacts& facts, const llvm::OriginMap& origin_map)` resolves exact SVF
  target names without heuristic name matching.

- [x] **Step 1: Add the failing SCC identity test**

Append this behavior test to `tests/unit/core/IdsTest.cpp`:

```cpp
TEST(IdsTest, SccIdRoundTripsWithDedicatedPrefix) {
  const std::vector<std::byte> data = {std::byte{0x53}, std::byte{0x43},
                                       std::byte{0x43}};
  const auto id = MakeStableId(IdKind::kScc, data);
  const std::string text = ToString(id);

  EXPECT_EQ(text.rfind("scc:sha256:", 0), 0u);
  auto parsed = ParseStableId(text);
  ASSERT_TRUE(parsed.ok()) << parsed.status().message();
  EXPECT_EQ(*parsed, id);
}
```

- [x] **Step 2: Run the test and verify the missing identity fails**

Run:

```bash
cmake --build --preset default --target IdsTest
```

Expected: compilation fails because `IdKind::kScc` does not exist.

- [x] **Step 3: Add the SCC identity kind**

Add `kScc` immediately after `kFact` in `Ids.h`. Add both mappings in `Ids.cpp`:

```cpp
case IdKind::kScc:
  return "scc";
```

```cpp
{"scc", IdKind::kScc},
```

- [x] **Step 4: Run the SCC identity test and verify green**

Run:

```bash
cmake --build --preset default --target IdsTest
./build/bin/IdsTest --gtest_filter=IdsTest.SccIdRoundTripsWithDedicatedPrefix
```

Expected: one test passes.

- [x] **Step 5: Add failing direct-call identity coverage**

Append this test to `LocalAnalysisStageTest.cpp`; it uses the existing `smoke`
fixture where `main` calls `add`:

```cpp
TEST(LocalAnalysisStageTest, DirectCallCarriesResolvedFunctionVariantId) {
  auto manifest = LoadFixtureManifest("smoke");
  ASSERT_TRUE(manifest.ok()) << manifest.status().message();
  auto result = RunLocalAnalysis(*manifest);
  ASSERT_TRUE(result.ok()) << result.status().message();

  const summary::v1::Call* add_call = nullptr;
  for (const auto& draft : result->summary_drafts) {
    for (const auto& call : draft.calls()) {
      if (call.callee_symbol() == "add") add_call = &call;
    }
  }
  ASSERT_NE(add_call, nullptr);
  auto parsed = core::ParseStableId(
      add_call->resolved_callee_function_variant_id());
  ASSERT_TRUE(parsed.ok()) << parsed.status().message();
  EXPECT_EQ(parsed->kind, core::IdKind::kFunctionVariant);
}
```

Add a focused merge-boundary test to `SvfFactMapperTest.cpp`. Build a
`ProgramIr` containing LLVM function `identity`, record its exact origin as
`funcvar:sha256:identity`, create a caller draft plus one synthetic refined
call, and invoke `MergeSvfFacts`:

```cpp
summary::v1::FunctionSummary draft;
draft.mutable_identity()->set_function_variant_id(
    "funcvar:sha256:caller");
SvfFacts facts;
facts.refined_calls.push_back(summary::CallFact{
    .callsite = {.name = "funcvar:sha256:caller:site"},
    .target = {.name = "identity"},
    .call_kind = "MAY_CALL",
    .provenance = "svf:test",
});
auto merged = MergeSvfFacts({draft}, facts, program_ir->origin_map());
ASSERT_EQ(merged.size(), 1u);
ASSERT_EQ(merged[0].calls_size(), 1);
EXPECT_EQ(merged[0].calls(0).resolved_callee_function_variant_id(),
          "funcvar:sha256:identity");
```

- [x] **Step 6: Run the extraction tests and verify red**

Run:

```bash
cmake --build --preset default --target local_analysis_stage_integration_test svf_fact_mapper_integration_test
```

Expected: compilation fails because the generated `Call` API has no
`resolved_callee_function_variant_id` accessor.

- [x] **Step 7: Add the Protobuf field and exact-name origin lookup**

Add this field to `Call` without renumbering existing fields:

```proto
string resolved_callee_function_variant_id = 5;
```

Maintain a second deterministic lookup in `OriginMap`:

```cpp
std::optional<std::string> GetSymbolIdByLlvmName(
    std::string_view llvm_name) const;

std::unordered_map<std::string, std::string> name_to_symbol_;
```

`RecordOrigin` inserts `func->getName().str() -> symbol_id`; `RemoveFunction`
erases that exact name; `Clear` clears all three maps. Duplicate LLVM names with
different stable IDs must not silently overwrite: erase the name entry so the
lookup returns `nullopt` and WPA later emits a scoped unknown.

- [x] **Step 8: Populate resolved IDs in local and SVF call facts**

Pass `const OriginMap&` into the private `ExtractCalls` helper and set the field
only on an exact stable lookup:

```cpp
if (const auto* callee = call->getCalledFunction()) {
  fact.set_callee_symbol(callee->getName().str());
  fact.set_epistemic(v1::EPISTEMIC_STATE_MUST);
  if (auto id = origin_map.GetSymbolId(callee)) {
    fact.set_resolved_callee_function_variant_id(*id);
  }
}
```

Change `MergeSvfFacts` to receive the live origin map and resolve only exact
target names:

```cpp
if (auto id = origin_map.GetSymbolIdByLlvmName(call.target.name)) {
  out->set_resolved_callee_function_variant_id(*id);
}
```

Update its `ProjectAnalyzer.cpp` call site to pass
`local_result.program_ir.origin_map()` before the `ProgramIr` is destroyed.

- [x] **Step 9: Run all identity and extraction tests**

Run:

```bash
cmake --build --preset default --target IdsTest local_analysis_stage_integration_test svf_fact_mapper_integration_test
ctest --test-dir build -R "IdsTest|LocalAnalysisStageTest|SvfFactMapperTest" --output-on-failure
```

Expected: all selected tests pass.

- [x] **Step 10: Commit Task 1**

```bash
git add include/veritas/core/Ids.h src/core/Ids.cpp \
  proto/veritas/summary/v1/summary.proto \
  src/analysis/llvm/OriginMap.h src/analysis/llvm/OriginMap.cpp \
  src/analysis/llvm/LocalFactExtractor.cpp \
  src/analysis/svf/SvfMerge.h src/analysis/svf/SvfMerge.cpp \
  src/analysis/ProjectAnalyzer.cpp tests/unit/core/IdsTest.cpp \
  tests/integration/analysis/LocalAnalysisStageTest.cpp \
  tests/integration/analysis/svf/SvfFactMapperTest.cpp
git commit -m "feat(summary): persist resolved call target identities"
```

---

### Task 2: Deterministic Current-Summary Enumeration

**Files:**
- Modify: `include/veritas/summarydb/SummaryRepository.h`
- Modify: `src/summarydb/SummaryRepository.cpp`
- Test: `tests/unit/summarydb/SummaryRepositoryTest.cpp`

**Interfaces:**
- Produces: `StatusOr<std::vector<FunctionSummary>> SummaryRepository::ListCurrentSummaries(std::string_view revision_id, std::string_view build_variant_id) const`.
- Consumes: SQLite current bindings and immutable object-store summary bodies.
- Guarantees: result order is ascending `function_variant_id`; every returned summary matches the requested revision/build context.

- [x] **Step 1: Add the failing ordered-enumeration test**

Add to `SummaryRepositoryTest.cpp`:

```cpp
TEST_F(SummaryRepositoryTest, ListsCurrentSummariesInFunctionVariantOrder) {
  auto repo_result = SummaryRepository::Open(test_dir_.string());
  ASSERT_TRUE(repo_result.ok());
  auto repo = std::move(*repo_result);

  PublicationContext context{
      .revision_id = "rev:sha256:def",
      .build_variant_id = "variant:sha256:ghi",
      .function_variant_id = "funcvar:sha256:z",
  };
  SetupParentRows(context);

  auto z = MakeSyntheticSummary();
  z.mutable_identity()->set_function_variant_id("funcvar:sha256:z");
  auto a = MakeSyntheticSummary();
  a.mutable_identity()->set_function_variant_id("funcvar:sha256:a");
  ASSERT_TRUE(repo->PublishProjectSummaries(
      context.revision_id, context.build_variant_id, {z, a}).ok());

  auto listed = repo->ListCurrentSummaries(
      context.revision_id, context.build_variant_id);
  ASSERT_TRUE(listed.ok()) << listed.status().message();
  ASSERT_EQ(listed->size(), 2u);
  EXPECT_EQ((*listed)[0].identity().function_variant_id(),
            "funcvar:sha256:a");
  EXPECT_EQ((*listed)[1].identity().function_variant_id(),
            "funcvar:sha256:z");
}
```

- [x] **Step 2: Run the test and verify red**

Run:

```bash
cmake --build --preset default --target SummaryRepositoryTest
```

Expected: compilation fails because `ListCurrentSummaries` does not exist.

- [x] **Step 3: Implement the bulk read**

Declare the exact interface from the approved spec. Query only the requested
context and impose SQL ordering:

```sql
SELECT summary_id
FROM summary_bindings
WHERE revision_id = ? AND build_variant_id = ? AND is_current = 1
ORDER BY function_variant_id ASC
```

For every row, parse the summary ID, call `GetSummary`, and validate:

```cpp
if (summary.identity().revision_id() != revision_id ||
    summary.identity().build_variant_id() != build_variant_id) {
  return Status::FailedPrecondition(
      "current summary binding does not match requested context");
}
```

Return an empty vector for a valid context with no bindings. Propagate malformed
IDs, object-store misses, and parse failures without partial results.

- [x] **Step 4: Add a context-isolation test**

Publish one summary for the fixture context, call `ListCurrentSummaries` with a
different revision string, and assert the result is successful and empty. This
test catches a query that forgets the revision/build predicates.

- [x] **Step 5: Run the repository tests**

```bash
cmake --build --preset default --target SummaryRepositoryTest
ctest --test-dir build -R SummaryRepositoryTest --output-on-failure
```

Expected: all `SummaryRepositoryTest` cases pass.

- [x] **Step 6: Commit Task 2**

```bash
git add include/veritas/summarydb/SummaryRepository.h \
  src/summarydb/SummaryRepository.cpp \
  tests/unit/summarydb/SummaryRepositoryTest.cpp
git commit -m "feat(summarydb): list current summaries by analysis context"
```

---

### Task 3: Stable Fact Tuple and Epistemic Model

**Files:**
- Create: `include/veritas/facts/FactSchema.h`
- Create: `src/facts/FactSchema.cpp`
- Create: `src/facts/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Create: `tests/unit/facts/FactSchemaTest.cpp`
- Create: `tests/unit/facts/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`

**Interfaces:**
- Produces: `facts::FactRelation`, `facts::FactTuple`, and `facts::BaseFactOrigin`.
- Produces: `FactRelationName`, `ParseFactRelation`, and `FactRelationArity`.
- Produces: `WeakenPositiveEpistemic`, `MakeBaseFact`, `MakeDerivedFact`, and `ValidateFactTuple`.
- Consumes: `core::IdKind::kFact`, summary epistemic enums, canonical length-prefixed SHA-256 inputs.

- [x] **Step 1: Create the failing fact-schema test target**

Register a `FactSchemaTest` executable linked to `veritas::facts` and
`GTest::gtest_main`. Add the new source/test subdirectories to their parent
`CMakeLists.txt` files. Start `FactSchemaTest.cpp` with these tests:

```cpp
TEST(FactSchemaTest, MustAndMayWeakenToMay) {
  auto joined = WeakenPositiveEpistemic(v1::EPISTEMIC_STATE_MUST,
                                        v1::EPISTEMIC_STATE_MAY);
  ASSERT_TRUE(joined.ok());
  EXPECT_EQ(*joined, v1::EPISTEMIC_STATE_MAY);
}

TEST(FactSchemaTest, DerivedTupleCarriesCanonicalImmediateInputs) {
  const auto summary_id = Id(core::IdKind::kFunctionSummary, "summary");
  const BaseFactOrigin origin{summary_id, "callsite:1", "prov:1"};
  auto call = MakeBaseFact(FactRelation::kDirectCall, {"A", "B"},
                           v1::EPISTEMIC_STATE_MUST, origin);
  auto write = MakeBaseFact(FactRelation::kDirectWrite, {"B", "X"},
                            v1::EPISTEMIC_STATE_MUST, origin);
  ASSERT_TRUE(call.ok());
  ASSERT_TRUE(write.ok());

  auto derived = MakeDerivedFact(
      FactRelation::kMayWrite, {"A", "X"},
      v1::EPISTEMIC_STATE_MUST, "m8.may_write.transitive.v1",
      {write->tuple_id, call->tuple_id});
  ASSERT_TRUE(derived.ok()) << derived.status().message();
  EXPECT_EQ(derived->tuple_id.kind, core::IdKind::kFact);
  EXPECT_EQ(derived->rule_id, "m8.may_write.transitive.v1");
  EXPECT_TRUE(std::is_sorted(derived->input_tuple_ids.begin(),
                             derived->input_tuple_ids.end()));
}
```

The test-local `Id` helper converts a literal into bytes and calls
`MakeStableId`; it must not reproduce `FactSchema` canonicalization.

- [x] **Step 2: Configure/build and verify red**

Run:

```bash
cmake --preset default -DLLVM_PROJECT_BUILD_DIR=/Users/skg7on/Workspace/Projects/llvm-project/build
cmake --build --preset default --target FactSchemaTest
```

Expected: configuration or compilation fails because the facts library and API
do not exist.

- [x] **Step 3: Define the public fact schema**

Use the exact approved relation set:

```cpp
enum class FactRelation {
  kDirectCall,
  kDirectRead,
  kDirectWrite,
  kLocalFlow,
  kMayAlias,
  kReachableCall,
  kMayWrite,
  kGlobalFlow,
};

struct BaseFactOrigin {
  core::StableId function_summary_id;
  std::string anchor;
  std::string provenance_ref;
};

struct FactTuple {
  core::StableId tuple_id;
  FactRelation relation;
  std::vector<std::string> columns;
  summary::v1::EpistemicState epistemic;
  std::string rule_id;
  std::vector<core::StableId> input_tuple_ids;
};
```

Declare the six functions listed in the Interfaces block. The arities are
`2, 2, 2, 3, 2, 2, 2, 2` in enum order.

- [x] **Step 4: Implement validation and epistemic weakening**

`WeakenPositiveEpistemic` accepts only `MUST` and `MAY`:

```cpp
if (!IsPositive(left) || !IsPositive(right)) {
  return Status::InvalidArgument("epistemic join requires MUST or MAY");
}
return left == v1::EPISTEMIC_STATE_MAY ||
               right == v1::EPISTEMIC_STATE_MAY
           ? v1::EPISTEMIC_STATE_MAY
           : v1::EPISTEMIC_STATE_MUST;
```

`ValidateFactTuple` checks relation arity, `kFact` tuple kind, positive
epistemic state, empty rule/inputs for base relations, and non-empty
rule/inputs for derived relations. It rejects embedded tabs, CR, or LF in every
column, rule ID, anchor, and provenance string.

- [x] **Step 5: Implement stable base and derived tuple IDs**

Use one private length-prefix helper:

```cpp
void AppendField(std::string* out, std::string_view value) {
  out->append(std::to_string(value.size()));
  out->push_back(':');
  out->append(value);
}
```

Base canonical bytes contain `veritas.fact.base.v1`, relation name, ordered
columns, epistemic integer, summary ID, anchor, and provenance. Derived bytes
contain `veritas.fact.derived.v1`, relation name, ordered columns, epistemic
integer, rule ID, and sorted immediate input IDs. Convert the final string to a
byte span and call:

```cpp
const auto bytes = std::as_bytes(std::span(canonical.data(), canonical.size()));
return core::MakeStableId(core::IdKind::kFact, bytes);
```

- [x] **Step 6: Add arity, invalid-state, and insertion-order tests**

Add separate tests that prove:

```cpp
EXPECT_FALSE(MakeBaseFact(FactRelation::kDirectCall, {"A"},
                          v1::EPISTEMIC_STATE_MUST, origin).ok());
EXPECT_FALSE(WeakenPositiveEpistemic(v1::EPISTEMIC_STATE_UNKNOWN,
                                     v1::EPISTEMIC_STATE_MUST).ok());
auto forward = MakeDerivedFact(
    FactRelation::kMayWrite, {"A", "X"}, v1::EPISTEMIC_STATE_MAY,
    "m8.may_write.transitive.v1", {input_a, input_b});
auto reverse = MakeDerivedFact(
    FactRelation::kMayWrite, {"A", "X"}, v1::EPISTEMIC_STATE_MAY,
    "m8.may_write.transitive.v1", {input_b, input_a});
ASSERT_TRUE(forward.ok());
ASSERT_TRUE(reverse.ok());
EXPECT_EQ(forward->tuple_id, reverse->tuple_id);
```

Use literal expected relation names and arities; do not compute expected values
with production helpers.

- [x] **Step 7: Run fact-schema tests**

```bash
cmake --build --preset default --target FactSchemaTest
ctest --test-dir build -R FactSchemaTest --output-on-failure
```

Expected: all fact schema tests pass.

- [x] **Step 8: Commit Task 3**

```bash
git add CMakeLists.txt include/veritas/facts src/facts \
  tests/unit/CMakeLists.txt tests/unit/facts
git commit -m "feat(facts): add stable tuple and epistemic model"
```

---
### Task 4: Deterministic Call Graph and SCC Condensation Graph

**Files:**
- Create: `include/veritas/wpa/CallGraph.h`
- Create: `include/veritas/wpa/SccGraph.h`
- Create: `src/wpa/CallGraph.cpp`
- Create: `src/wpa/SccGraph.cpp`
- Create: `src/wpa/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Create: `tests/unit/wpa/SccGraphTest.cpp`
- Create: `tests/unit/wpa/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`

**Interfaces:**
- Produces: `wpa::CallEdge`, `wpa::UnknownCallEffect`, and `wpa::CallGraph` exactly as specified in design section 5.
- Produces: `wpa::SccGraph::Build`, `SccForFunction`, `Members`, `Predecessors`, `Successors`, and `ReverseTopologicalOrder` exactly as specified in design section 6.
- Consumes: current `FunctionSummary` values and positive `MUST`/`MAY` call facts.
- Guarantees: `UNKNOWN`, unsupported, unresolved, and known-but-unavailable targets produce scoped unknown effects rather than graph edges.

- [x] **Step 1: Add the failing SCC tests and target**

Create the CMake targets and start `SccGraphTest.cpp` with these literal graph
helpers (they deliberately do not reuse SCC production canonicalization):

```cpp
core::StableId FunctionId(std::string_view name) {
  return core::MakeStableId(
      core::IdKind::kFunctionVariant,
      std::as_bytes(std::span(name.data(), name.size())));
}

CallEdge MayCall(core::StableId caller, core::StableId callee,
                 std::string anchor) {
  return CallEdge{.caller = caller,
                  .callee = callee,
                  .call_site_anchor_id = std::move(anchor),
                  .epistemic = v1::EPISTEMIC_STATE_MAY,
                  .provenance_ref = "test:call"};
}
```

```cpp
TEST(SccGraphTest, MutualRecursionHasOneInsertionIndependentScc) {
  const auto a = FunctionId("A");
  const auto b = FunctionId("B");

  CallGraph first;
  ASSERT_TRUE(first.AddFunction(a).ok());
  ASSERT_TRUE(first.AddFunction(b).ok());
  ASSERT_TRUE(first.AddCall(MayCall(a, b, "site:a-b")).ok());
  ASSERT_TRUE(first.AddCall(MayCall(b, a, "site:b-a")).ok());

  CallGraph second;
  ASSERT_TRUE(second.AddFunction(b).ok());
  ASSERT_TRUE(second.AddFunction(a).ok());
  ASSERT_TRUE(second.AddCall(MayCall(b, a, "site:b-a")).ok());
  ASSERT_TRUE(second.AddCall(MayCall(a, b, "site:a-b")).ok());

  auto first_scc = SccGraph::Build(first);
  auto second_scc = SccGraph::Build(second);
  ASSERT_TRUE(first_scc.ok());
  ASSERT_TRUE(second_scc.ok());
  EXPECT_EQ(*first_scc->SccForFunction(a), *first_scc->SccForFunction(b));
  EXPECT_EQ(*first_scc->SccForFunction(a), *second_scc->SccForFunction(a));
}

TEST(SccGraphTest, AcyclicGraphOrdersCalleesBeforeCallers) {
  const auto a = FunctionId("A");
  const auto b = FunctionId("B");
  const auto c = FunctionId("C");
  CallGraph graph;
  ASSERT_TRUE(graph.AddFunction(a).ok());
  ASSERT_TRUE(graph.AddFunction(b).ok());
  ASSERT_TRUE(graph.AddFunction(c).ok());
  ASSERT_TRUE(graph.AddCall(MayCall(a, b, "site:a-b")).ok());
  ASSERT_TRUE(graph.AddCall(MayCall(b, c, "site:b-c")).ok());
  auto scc = SccGraph::Build(graph);
  ASSERT_TRUE(scc.ok());

  const auto order = scc->ReverseTopologicalOrder();
  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order[0], *scc->SccForFunction(c));
  EXPECT_EQ(order[1], *scc->SccForFunction(b));
  EXPECT_EQ(order[2], *scc->SccForFunction(a));
}
```

- [x] **Step 2: Configure/build and verify red**

```bash
cmake --preset default -DLLVM_PROJECT_BUILD_DIR=/Users/skg7on/Workspace/Projects/llvm-project/build
cmake --build --preset default --target SccGraphTest
```

Expected: configuration or compilation fails because the WPA graph APIs do not
exist.

- [x] **Step 3: Implement the call graph import boundary**

Store functions, edges, and unknown markers in sorted vectors behind maps keyed
by `core::ToString(id)`. `FromSummaries` follows this order:

```text
1. Parse and validate every summary function-variant ID; add all vertices.
2. Revisit summaries in function-ID order.
3. For each call, accept an edge only when:
   a. epistemic is MUST or MAY;
   b. resolved_callee_function_variant_id parses as kFunctionVariant; and
   c. the callee is one of the loaded vertices.
4. Otherwise append one caller/call-site-scoped UnknownCallEffect.
5. Sort/deduplicate identical facts; reject conflicting same-site facts.
```

Do not fall back from a missing resolved ID to `callee_symbol` matching.

- [x] **Step 4: Implement deterministic Tarjan SCC construction**

Use stable-ID text as the traversal key. Maintain explicit DFS frames containing
the current vertex, sorted callees, and next-callee position. Assign Tarjan
indices when pushing a frame; on frame completion, propagate lowlink to its
parent and emit an SCC when `lowlink == index`. This preserves Tarjan traversal
semantics without consuming one native stack frame per call-graph vertex.

```cpp
struct DfsFrame {
  core::StableId vertex;
  std::vector<core::StableId> sorted_callees;
  std::size_t next_callee;
};
```

`AddScc` hashes `veritas.scc.v1` plus length-prefixed sorted member IDs with
`IdKind::kScc`.

- [x] **Step 5: Implement deterministic reverse-topological order**

Collapse inter-SCC call edges. Start with SCCs whose successor count is zero,
using a min-priority queue ordered by SCC ID. After popping a callee SCC,
decrement each predecessor's remaining successor count and enqueue it at zero.
The final vector must contain every SCC exactly once or return `Internal`.

- [x] **Step 6: Add unknown-call, self-recursion, and missing-ID tests**

Add independent behavior tests:

```cpp
TEST(SccGraphTest, UnknownCallDoesNotFanOutOrMergeFunctions) {
  const auto a = FunctionId("A");
  const auto b = FunctionId("B");
  const auto c = FunctionId("C");
  CallGraph graph;
  ASSERT_TRUE(graph.AddFunction(a).ok());
  ASSERT_TRUE(graph.AddFunction(b).ok());
  ASSERT_TRUE(graph.AddFunction(c).ok());
  ASSERT_TRUE(graph.AddUnknownCall(
      {.caller = a, .call_site_anchor_id = "site:unknown",
       .callee_symbol = "vendor_validate",
       .provenance_ref = "test:unknown"}).ok());
  auto scc = SccGraph::Build(graph);
  ASSERT_TRUE(scc.ok());
  EXPECT_NE(*scc->SccForFunction(a), *scc->SccForFunction(b));
  EXPECT_NE(*scc->SccForFunction(a), *scc->SccForFunction(c));
  EXPECT_TRUE(graph.Outgoing(a).empty());
  EXPECT_EQ(graph.UnknownCalls(a).size(), 1u);
}

TEST(SccGraphTest, SelfRecursiveFunctionFormsOneMemberRecursiveScc) {
  const auto a = FunctionId("A");
  CallGraph graph;
  ASSERT_TRUE(graph.AddFunction(a).ok());
  ASSERT_TRUE(graph.AddCall(MayCall(a, a, "site:self")).ok());
  auto scc = SccGraph::Build(graph);
  ASSERT_TRUE(scc.ok());
  auto members = scc->Members(*scc->SccForFunction(a));
  ASSERT_TRUE(members.ok());
  ASSERT_EQ(members->size(), 1u);
  EXPECT_EQ((*members)[0], a);
}

TEST(CallGraphTest, SummaryWithoutResolvedCalleeProducesScopedUnknown) {
  v1::FunctionSummary summary;
  summary.mutable_identity()->set_function_variant_id(
      core::ToString(FunctionId("A")));
  auto* call = summary.add_calls();
  call->set_call_site_anchor_id("site:unknown");
  call->set_callee_symbol("vendor_validate");
  call->set_epistemic(v1::EPISTEMIC_STATE_UNKNOWN);
  call->set_provenance_ref("test:summary");
  const std::array summaries{summary};
  auto graph = CallGraph::FromSummaries(summaries);
  ASSERT_TRUE(graph.ok());
  EXPECT_TRUE(graph->Outgoing(FunctionId("A")).empty());
  EXPECT_EQ(graph->UnknownCalls(FunctionId("A")).size(), 1u);
}
```

Assert real graph membership and adjacency; do not assert private Tarjan state.

- [x] **Step 7: Run all graph tests**

```bash
cmake --build --preset default --target SccGraphTest
ctest --test-dir build -R SccGraphTest --output-on-failure
```

Expected: all call-graph and SCC tests pass.

- [x] **Step 8: Commit Task 4**

```bash
git add CMakeLists.txt include/veritas/wpa src/wpa \
  tests/unit/CMakeLists.txt tests/unit/wpa
git commit -m "feat(wpa): add deterministic call and SCC graphs"
```

---

### Task 5: C++ Transitive-Call and May-Write Fixpoint Engine

**Files:**
- Create: `include/veritas/wpa/FixpointDomain.h`
- Create: `include/veritas/wpa/FixpointEngine.h`
- Create: `src/wpa/FixpointDomain.cpp`
- Create: `src/wpa/FixpointEngine.cpp`
- Modify: `src/wpa/CMakeLists.txt`
- Create: `tests/unit/wpa/FixpointEngineTest.cpp`
- Modify: `tests/unit/wpa/CMakeLists.txt`

**Interfaces:**
- Produces: `wpa::SccStatus`, `wpa::FixpointBudget`, `wpa::SccResult`, and `wpa::FixpointEngine` exactly as specified in design section 8.
- Produces: `FixpointEngine::ComputeAll` results in SCC reverse-topological order.
- Consumes: `CallGraph`, `SccGraph`, summaries, and `facts::FactTuple` constructors.
- Guarantees: finite monotone joins, canonical proof selection, deterministic hashes, explicit approximation.

- [x] **Step 1: Add the failing three-function may-write test**

Create summaries `A`, `B`, and `C` with resolved `MUST` calls `A -> B`, `B -> C`
and a direct `MUST` write to memory `X` in `C`:

```cpp
TEST(FixpointEngineTest, DerivesMayWriteThroughThreeFunctionChain) {
  auto fixture = MakeChainWithWrite("A", "B", "C", "X");
  auto graph = CallGraph::FromSummaries(fixture.summaries);
  ASSERT_TRUE(graph.ok());
  auto scc = SccGraph::Build(*graph);
  ASSERT_TRUE(scc.ok());

  FixpointEngine engine(*graph, *scc, fixture.summaries);
  auto results = engine.ComputeAll(v1::COMPONENT_KIND_MEMORY_EFFECTS,
                                   FixpointBudget{.max_iterations = 32});
  ASSERT_TRUE(results.ok()) << results.status().message();
  EXPECT_TRUE(ContainsFact(*results, FactRelation::kMayWrite,
                           {fixture.a_text, "X"},
                           v1::EPISTEMIC_STATE_MUST));
}
```

- [x] **Step 2: Build and verify red**

```bash
cmake --build --preset default --target FixpointEngineTest
```

Expected: configuration or compilation fails because the fixpoint APIs do not
exist.

- [x] **Step 3: Implement the finite domain join**

Use a map from semantic key to one canonical fact:

```cpp
struct DomainFact {
  summary::v1::EpistemicState epistemic;
  facts::FactTuple tuple;
};

using FactDomain = std::map<std::vector<std::string>, DomainFact>;
```

For a candidate with a new key, insert it. For an existing key, compute the
weaker positive epistemic state. Return `true` only when the semantic state
changes; proof selection happens after semantic convergence so provenance does
not drive the finite lattice iteration.

- [x] **Step 4: Implement direct seeds and transfer rules**

Create base `DirectCall` tuples from positive call edges and base `DirectWrite`
tuples from `EFFECT_KIND_WRITE` summary effects. Ignore reads for the M8
executable domain. Apply exactly four rules:

```text
Reachable(A,B) <- DirectCall(A,B)
Reachable(A,C) <- DirectCall(A,B) + Reachable(B,C)
MayWrite(A,X)  <- DirectWrite(A,X)
MayWrite(A,X)  <- DirectCall(A,B) + MayWrite(B,X)
```

Each transfer calls `WeakenPositiveEpistemic` and `MakeDerivedFact` with the
versioned rule ID from the spec.

- [x] **Step 5: Implement SCC evaluation and caching**

`ComputeAll` walks `ReverseTopologicalOrder`. For every SCC:

```text
1. Seed direct member facts.
2. Import already-computed successor SCC facts.
3. Evaluate members in function-ID order.
4. Repeat internal transfers until no domain changes.
5. Stop at max_iterations and mark kApproximated.
6. Cache the result by (scc_id, component_kind).
```

`Compute(core::StableId scc_id,
summary::v1::ComponentKind component_kind, FixpointBudget budget)` recursively
ensures successor cache entries exist, then uses the same evaluator.
Unsupported component kinds return `kUnsupported` and an empty fact vector
without mutating the cache for supported domains.

Cache reuse compares successor fixpoint hashes as a proof-dependency snapshot,
while the persisted `input_hash` continues to use successor external hashes.
This refreshes exact tuple-ID references without turning internal proof changes
into persistent predecessor scheduling.

- [x] **Step 6: Implement deterministic hashes**

Compute:

```text
input_hash = SHA256(version + sorted local component digests
                    + sorted successor external hashes)
fixpoint_hash = SHA256(version + sorted complete member facts
                       + epistemic + tuple support IDs)
externally_visible_hash = SHA256(version + sorted externally visible
                                 relation/columns/epistemic)
```

Use length-prefixed fields and `DigestToHex`. Do not include iteration count,
status timestamps, or hash-table order. On `kApproximated`, weaken every
externally visible `MUST` fact to `MAY`, rebuild its derived tuple ID, then hash.

- [x] **Step 7: Add transitive-call and order tests**

Add one test asserting `ReachableCall(A,C)` for `A -> B -> C`, and one asserting
that `ComputeAll(CALLS)` returns SCC results in the exact `C, B, A` order. The
expected order must come from hand-created fixture IDs, not from calling the
engine's ordering helper twice.

- [x] **Step 8: Add recursion, weakening, and approximation tests**

Add separate tests proving:

```text
self-recursive A converges with ReachableCall(A,A)
mutually recursive A/B converge as one SCC
MAY A->B plus MUST write(B,X) yields MAY MayWrite(A,X)
max_iterations=1 on a multi-step recursive fixture yields APPROXIMATED
reordered equivalent summaries yield identical facts and all three hashes
```

For every test, name the mutation it catches: missing recursion loop, wrong
epistemic join, ignored budget, or insertion-order dependence.

- [x] **Step 9: Run the fixpoint suite**

```bash
cmake --build --preset default --target FixpointEngineTest
ctest --test-dir build -R "SccGraphTest|FixpointEngineTest" --output-on-failure
```

Expected: all graph and fixpoint tests pass.

- [x] **Step 10: Commit Task 5**

```bash
git add include/veritas/wpa/FixpointDomain.h \
  include/veritas/wpa/FixpointEngine.h src/wpa/FixpointDomain.cpp \
  src/wpa/FixpointEngine.cpp src/wpa/CMakeLists.txt \
  tests/unit/wpa/FixpointEngineTest.cpp tests/unit/wpa/CMakeLists.txt
git commit -m "feat(wpa): derive transitive calls and may-write facts"
```

---

### Task 6: Persistent SCC State and M7 Propagation

**Files:**
- Modify: `src/summarydb/schema/v1.sql`
- Create: `include/veritas/wpa/SccStateRepository.h`
- Create: `include/veritas/wpa/WpaCoordinator.h`
- Create: `src/wpa/SccStateRepository.cpp`
- Create: `src/wpa/WpaCoordinator.cpp`
- Modify: `src/wpa/CMakeLists.txt`
- Create: `tests/unit/wpa/SccStateRepositoryTest.cpp`
- Create: `tests/unit/wpa/WpaCoordinatorTest.cpp`
- Modify: `tests/unit/wpa/CMakeLists.txt`

**Interfaces:**
- Produces: `wpa::SccContext`, `wpa::StoredSccState`, `wpa::ExternalChange`, and `wpa::SccStateRepository`.
- Produces: `WpaCoordinator::EnqueuePredecessorsIfChanged`.
- Consumes: `MetadataStore`, `CallGraph`, `SccGraph`, `SccResult`, M7 `WorkItem`, and `WorklistScheduler`.

- [x] **Step 1: Add the failing schema/state test**

Create a temporary `MetadataStore`, apply the schema, store one converged state,
and reload every field:

```cpp
TEST_F(SccStateRepositoryTest, PersistsAndReloadsAllConvergenceFields) {
  auto graph = MakeSingleSccGraph();
  ASSERT_TRUE(repository.PublishGraph(context, graph.call_graph,
                                      graph.scc_graph).ok());
  const SccResult result = Result("input-a", "fixpoint-a", "external-a", 3,
                                  SccStatus::kConverged);
  auto change = repository.StoreState(context, result);
  ASSERT_TRUE(change.ok());
  EXPECT_EQ(*change, ExternalChange::kChanged);

  auto loaded = repository.LoadState(
      context, result.scc_id, result.component_kind);
  ASSERT_TRUE(loaded.ok());
  ASSERT_TRUE(loaded->has_value());
  EXPECT_EQ((*loaded)->input_hash, "input-a");
  EXPECT_EQ((*loaded)->fixpoint_hash, "fixpoint-a");
  EXPECT_EQ((*loaded)->externally_visible_hash, "external-a");
  EXPECT_EQ((*loaded)->iteration_count, 3u);
  EXPECT_EQ((*loaded)->status, SccStatus::kConverged);
}
```

- [x] **Step 2: Configure/build and verify red**

```bash
cmake --preset default -DLLVM_PROJECT_BUILD_DIR=/Users/skg7on/Workspace/Projects/llvm-project/build
cmake --build --preset default --target SccStateRepositoryTest
```

Expected: compilation fails because the repository API and tables do not
exist.

- [x] **Step 3: Add the four SCC tables**

Append the exact columns from design section 10 to `schema/v1.sql`. Use integer
columns for epistemic/component/status values and text for stable IDs/hashes.
Add these indexes in addition to the composite primary keys:

```sql
CREATE INDEX IF NOT EXISTS idx_wpa_scc_members_function
  ON wpa_scc_members(function_variant_id, revision_id, build_variant_id);
CREATE INDEX IF NOT EXISTS idx_wpa_scc_edges_callee
  ON wpa_scc_edges(callee_scc_id, revision_id, build_variant_id);
```

Reference `revisions` and `build_variants` with foreign keys. Do not cascade
deletes into historical summary or M7 dependency tables.

- [x] **Step 4: Implement transactional topology publication**

`PublishGraph(context, call_graph, scc_graph)` begins one metadata transaction,
deletes only topology rows for the exact revision/build context, inserts SCCs
and sorted members, and inserts each inter-SCC caller/callee edge once. If
multiple call edges collapse to one SCC edge, persist `MAY` when any contributing
edge is `MAY`; otherwise persist `MUST`. Roll back on every early return.

- [x] **Step 5: Implement state load/store and external-change detection**

`LoadState` returns `std::optional<StoredSccState>`. `StoreState` reads the prior
external hash inside the same transaction, upserts all fields, commits, then
returns:

```cpp
return !previous.has_value() ||
               previous->externally_visible_hash !=
                   result.externally_visible_hash
           ? ExternalChange::kChanged
           : ExternalChange::kUnchanged;
```

A changed input/fixpoint hash with the same external hash is stored and returns
`kUnchanged`.

- [x] **Step 6: Add the internal-only-change test**

Store `("input-a", "fixpoint-a", "external")`, then store
`("input-b", "fixpoint-b", "external")`. Assert the second result is
`kUnchanged` and the reloaded row contains the `b` hashes. This catches both
lost updates and accidental propagation on fixpoint-only changes.

- [x] **Step 7: Add the failing scheduler handoff tests**

Create `WpaCoordinatorTest.cpp` with a two-SCC caller/callee graph:

```cpp
TEST(WpaCoordinatorTest, ExternalChangeSchedulesEachPredecessorOnce) {
  runtime::WorklistScheduler scheduler;
  ASSERT_TRUE(WpaCoordinator::EnqueuePredecessorsIfChanged(
      ExternalChange::kChanged, callee_scc, v1::COMPONENT_KIND_MEMORY_EFFECTS,
      context, {delta_id}, scc_graph, &scheduler).ok());
  ASSERT_TRUE(WpaCoordinator::EnqueuePredecessorsIfChanged(
      ExternalChange::kChanged, callee_scc, v1::COMPONENT_KIND_MEMORY_EFFECTS,
      context, {delta_id}, scc_graph, &scheduler).ok());
  EXPECT_EQ(scheduler.PendingCount(), 1u);
  const auto item = scheduler.PopNext();
  ASSERT_TRUE(item.has_value());
  EXPECT_EQ(item->kind, runtime::WorkItemKind::kWpaComponent);
  EXPECT_EQ(item->target_id, caller_scc);
}

TEST(WpaCoordinatorTest, UnchangedExternalHashSchedulesNothing) {
  runtime::WorklistScheduler scheduler;
  ASSERT_TRUE(WpaCoordinator::EnqueuePredecessorsIfChanged(
      ExternalChange::kUnchanged, callee_scc, v1::COMPONENT_KIND_MEMORY_EFFECTS,
      context, {}, scc_graph, &scheduler).ok());
  EXPECT_TRUE(scheduler.Empty());
}
```

- [x] **Step 8: Implement the M7 handoff**

For every sorted predecessor, construct the exact `WorkItem` from design
section 11. Preserve incoming delta IDs and let `WorklistScheduler::Enqueue`
perform semantic-key deduplication. Return `InvalidArgument` for a null
scheduler and propagate `NotFound` from `SccGraph::Predecessors`.

- [x] **Step 9: Run persistence and scheduler tests**

```bash
cmake --build --preset default --target SccStateRepositoryTest WpaCoordinatorTest
ctest --test-dir build -R "SccStateRepositoryTest|WpaCoordinatorTest|WorklistSchedulerTest" --output-on-failure
```

Expected: all selected tests pass.

- [x] **Step 10: Commit Task 6**

```bash
git add src/summarydb/schema/v1.sql \
  include/veritas/wpa/SccStateRepository.h include/veritas/wpa/WpaCoordinator.h \
  src/wpa/SccStateRepository.cpp src/wpa/WpaCoordinator.cpp \
  src/wpa/CMakeLists.txt tests/unit/wpa/SccStateRepositoryTest.cpp \
  tests/unit/wpa/WpaCoordinatorTest.cpp tests/unit/wpa/CMakeLists.txt
git commit -m "feat(wpa): persist SCC state and schedule external changes"
```

---

### Task 7: Deterministic Souffle Relation Export and Provenance Import

**Files:**
- Create: `include/veritas/facts/SouffleExporter.h`
- Create: `src/facts/SouffleExporter.cpp`
- Modify: `src/facts/CMakeLists.txt`
- Create: `tests/unit/facts/SouffleExporterTest.cpp`
- Modify: `tests/unit/facts/CMakeLists.txt`

**Interfaces:**
- Produces: `SouffleExporter::WriteBaseRelations(const std::filesystem::path&, std::span<const FactTuple>)`.
- Produces: `SouffleExporter::ReadDerivedRelations(const std::filesystem::path&, std::span<const FactTuple>)`.
- Consumes: validated base tuples plus semantic `ReachableCall.csv` and `MayWrite.csv` rows.
- Guarantees: deterministic TSV order/escaping, all-or-nothing import, canonical immediate derivation reconstruction.

- [x] **Step 1: Add the failing deterministic-export test**

Use a unique temporary directory and real `FactTuple` values:

```cpp
TEST_F(SouffleExporterTest, WritesBaseRelationsInTupleIdOrder) {
  const auto later = DirectCall("B", "C", "site:2");
  const auto earlier = DirectCall("A", "B", "site:1");
  ASSERT_TRUE(SouffleExporter::WriteBaseRelations(
      test_dir_, {later, earlier}).ok());

  const std::string rows = ReadFile(test_dir_ / "DirectCall.facts");
  const std::string earlier_id = core::ToString(earlier.tuple_id);
  const std::string later_id = core::ToString(later.tuple_id);
  const std::string expected_first_row =
      earlier_id < later_id
          ? earlier_id + "\tA\tB\t1\n"
          : later_id + "\tB\tC\t1\n";
  EXPECT_EQ(rows.substr(0, expected_first_row.size()), expected_first_row);
}
```

Do not derive the expected full row through `SouffleExporter`; construct it
from literals and `core::ToString` only. Add one tuple for every documented
base relation and assert the corresponding file exists with the correct field
count.

- [x] **Step 2: Build and verify red**

```bash
cmake --build --preset default --target SouffleExporterTest
```

Expected: compilation fails because `SouffleExporter` does not exist.

- [x] **Step 3: Implement base relation export**

Validate every tuple before opening output files. Group only base relations,
sort each group by tuple ID, and write these exact filenames:

```text
DirectCall.facts
DirectRead.facts
DirectWrite.facts
LocalFlow.facts
MayAlias.facts
```

Each row is `tuple_id`, semantic columns in schema order, then the numeric
epistemic enum, separated by one tab and terminated by `\n`. Write to sibling
temporary files first; rename all files into place only after every write and
close succeeds. On any error, remove only the temporary files created by this
call and leave prior complete relation files unchanged.

- [x] **Step 4: Add the failing derived-provenance test**

Write semantic result rows for a chain `A -> B -> C` and direct write `C -> X`:

```text
ReachableCall.csv:
A\tB\t1
B\tC\t1
A\tC\t1

MayWrite.csv:
C\tX\t1
B\tX\t1
A\tX\t1
```

Then assert:

```cpp
auto imported = SouffleExporter::ReadDerivedRelations(test_dir_, base_facts);
ASSERT_TRUE(imported.ok()) << imported.status().message();
const FactTuple& a_writes_x = Find(*imported, FactRelation::kMayWrite,
                                  {"A", "X"});
EXPECT_EQ(a_writes_x.rule_id, "m8.may_write.transitive.v1");
EXPECT_EQ(a_writes_x.input_tuple_ids.size(), 2u);
EXPECT_EQ(a_writes_x.tuple_id.kind, core::IdKind::kFact);
```

- [x] **Step 5: Run the provenance test and verify red**

```bash
cmake --build --preset default --target SouffleExporterTest
./build/bin/SouffleExporterTest \
  --gtest_filter=SouffleExporterTest.ReconstructsImmediateMayWriteProvenance
```

Expected: the new test fails because derived import is not implemented.

- [x] **Step 6: Implement semantic-row parsing and weakening**

Parse exactly three tab-separated fields per derived row. Report relation name
and one-based line number for malformed arity, unsupported epistemic integers,
embedded CR, or empty semantic columns. Group duplicate semantic keys and keep
the weaker positive epistemic state, so Souffle may emit both `MUST` and `MAY`
proofs without changing VERITAS semantics.

Read every file and validate every row into temporary in-memory structures
before producing the first returned `FactTuple`.

- [x] **Step 7: Implement canonical proof reconstruction**

Memoize reconstruction by `(relation, columns, epistemic)` and assign finite
rooted proof ranks from direct facts and validated derived-support boundaries.
Enumerate candidates:

```text
ReachableCall(A,B):
  direct DirectCall(A,B)
  or DirectCall(A,M) + reconstructed ReachableCall(M,B)

MayWrite(A,X):
  direct DirectWrite(A,X)
  or DirectCall(A,B) + reconstructed MayWrite(B,X)
```

Apply `WeakenPositiveEpistemic` and retain candidates matching the final grouped
epistemic state. Choose direct before transitive, then minimum finite rooted
rank, then the lexicographically smallest sorted input tuple-ID vector. Call
`MakeDerivedFact` with the selected rule and inputs. Return
`FailedPrecondition` only if no finite rooted proof exists.

Index calls by caller, writes by semantic columns, and derived support by
relation/columns. Register reverse dependency edges once, seed direct/support
proofs in a priority queue, and propagate rooted ranks through that queue. The
queue order is `(rank, semantic key, ordered inputs, rule)` so all lower-rank
dependencies are fixed before canonical tie-breaking finalizes a key.

Consider both `MUST` and `MAY` proofs for each supplied semantic key. Return the
transitive closure of the selected final proofs, including any stronger
auxiliary tuple needed to root a weaker recursive proof. Hash auxiliary support
in `fixpoint_hash`, but join by semantic key before computing the external hash.

- [x] **Step 8: Add malformed-row and canonical-multiple-proof tests**

Add independent tests that:

- a two-field `ReachableCall.csv` row reports line 1 and returns no tuples;
- a row with epistemic `6` is rejected;
- two valid paths from `A` to `D` select the lexicographically smaller input-ID
  proof regardless of base/result row order;
- a recursive semantic row with no direct seed fails provenance reconstruction.

- [x] **Step 9: Run fact-boundary tests**

```bash
cmake --build --preset default --target FactSchemaTest SouffleExporterTest
ctest --test-dir build -R "FactSchemaTest|SouffleExporterTest" --output-on-failure
```

Expected: all fact model/export/import tests pass without a Souffle installation.

- [x] **Step 10: Commit Task 7**

```bash
git add include/veritas/facts/SouffleExporter.h \
  src/facts/SouffleExporter.cpp src/facts/CMakeLists.txt \
  tests/unit/facts/SouffleExporterTest.cpp tests/unit/facts/CMakeLists.txt
git commit -m "feat(facts): add deterministic Souffle relation boundary"
```

---

### Task 8: Optional Souffle Runner and Recursive Rules

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `cmake/Dependencies.cmake`
- Create: `include/veritas/facts/SouffleRunner.h`
- Create: `src/facts/SouffleRunner.cpp`
- Create: `src/facts/rules/reachability.dl`
- Create: `src/facts/rules/memory_effects.dl`
- Modify: `src/facts/CMakeLists.txt`
- Create: `tests/integration/facts/SouffleRunnerTest.cpp`
- Create: `tests/integration/facts/CMakeLists.txt`
- Modify: `tests/integration/CMakeLists.txt`

**Interfaces:**
- Produces: `VERITAS_ENABLE_SOUFFLE` option, `VERITAS_HAS_SOUFFLE` CMake boolean, and `VERITAS_SOUFFLE_EXECUTABLE` path when discovered.
- Produces: `SouffleRunner::Run(executable, rule_file, input_dir, output_dir)`.
- Consumes: relation files from Task 7 and `LLVMSupport` process execution.
- Guarantees: missing/disabled Souffle is a supported configuration; process failure cannot update SQLite state.

- [x] **Step 1: Add explicit optional dependency discovery**

Add this option beside the existing build options:

```cmake
option(VERITAS_ENABLE_SOUFFLE
  "Enable optional Souffle WPA execution when the executable is available" ON)
```

Replace `find_package(Souffle QUIET)` with executable discovery:

```cmake
set(VERITAS_HAS_SOUFFLE OFF)
if(VERITAS_ENABLE_SOUFFLE)
  find_program(VERITAS_SOUFFLE_EXECUTABLE NAMES souffle)
  if(VERITAS_SOUFFLE_EXECUTABLE)
    set(VERITAS_HAS_SOUFFLE ON)
    message(STATUS "VERITAS: Found Souffle ${VERITAS_SOUFFLE_EXECUTABLE}")
  else()
    message(STATUS "VERITAS: Souffle not found; C++ WPA remains enabled")
  endif()
else()
  message(STATUS "VERITAS: Souffle execution disabled")
endif()
```

Print the option and availability in the top-level configuration summary.

- [x] **Step 2: Add the failing runner test target**

Register `SouffleRunnerTest` unconditionally. Pass numeric availability and the
discovered path only to the test target:

```cmake
target_compile_definitions(SouffleRunnerTest PRIVATE
  VERITAS_HAS_SOUFFLE=$<BOOL:${VERITAS_HAS_SOUFFLE}>
  VERITAS_SOUFFLE_EXECUTABLE="${VERITAS_SOUFFLE_EXECUTABLE}"
  VERITAS_FACT_RULE_DIR="${PROJECT_SOURCE_DIR}/src/facts/rules"
)
```

The test begins with:

```cpp
#if !VERITAS_HAS_SOUFFLE
  GTEST_SKIP() << "Souffle executable not available";
#endif
```

When available, export the `A -> B -> C`, `C writes X` base fixture, run both
rules, import results, and compare semantic `(relation, columns, epistemic)`
sets against `FixpointEngine` output.

- [x] **Step 3: Build and verify red or explicit skip precondition**

```bash
cmake --preset default -DLLVM_PROJECT_BUILD_DIR=/Users/skg7on/Workspace/Projects/llvm-project/build
cmake --build --preset default --target SouffleRunnerTest
```

Expected: compilation fails because `SouffleRunner` and rule files do not exist.
On this development machine, the eventual runtime test will skip because
Souffle is absent; the build itself must still succeed.

- [x] **Step 4: Implement the process runner**

Use `llvm::sys::ExecuteAndWait` with an argument vector, never a shell string:

```cpp
const std::vector<llvm::StringRef> args = {
    executable.string(), "-F", input_dir.string(), "-D",
    output_dir.string(), rule_file.string()};
std::string error;
const int exit_code = llvm::sys::ExecuteAndWait(
    executable.string(), args, std::nullopt, {}, 0, 0, &error);
```

Validate that the executable and rule file exist and both directories are
directories. Create the output directory when absent. Return `Internal` with
the exit code/error when execution fails. Link `veritas_facts` privately to
`LLVMSupport`; no LLVM type appears in the public header.

- [x] **Step 5: Add the reachability rule**

After the full license header, define:

```souffle
.decl Weaken(left:number, right:number, result:number)
Weaken(1, 1, 1).
Weaken(1, 2, 2).
Weaken(2, 1, 2).
Weaken(2, 2, 2).

.decl DirectCall(tuple_id:symbol, caller:symbol, callee:symbol,
                 epistemic:number)
.input DirectCall
.decl ReachableCall(source:symbol, destination:symbol, epistemic:number)
.output ReachableCall

ReachableCall(a, b, e) :- DirectCall(_, a, b, e).
ReachableCall(a, c, out) :-
  DirectCall(_, a, b, edge), ReachableCall(b, c, fact),
  Weaken(edge, fact, out).
```

- [x] **Step 6: Add the memory-effects rule**

Repeat the same `Weaken` relation and `DirectCall` declaration, then define:

```souffle
.decl DirectWrite(tuple_id:symbol, function:symbol, memory:symbol,
                  epistemic:number)
.input DirectWrite
.decl MayWrite(function:symbol, memory:symbol, epistemic:number)
.output MayWrite

MayWrite(f, x, e) :- DirectWrite(_, f, x, e).
MayWrite(f, x, out) :-
  DirectCall(_, f, g, edge), MayWrite(g, x, fact),
  Weaken(edge, fact, out).
```

- [x] **Step 7: Run the optional-runner and mandatory C++ tests**

```bash
cmake --build --preset default --target SouffleRunnerTest FixpointEngineTest SouffleExporterTest
ctest --test-dir build -R "SouffleRunnerTest|FixpointEngineTest|SouffleExporterTest" --output-on-failure
```

Expected locally: mandatory C++ tests pass and `SouffleRunnerTest` reports one
explicit skip. If the executable is installed, the comparison test passes.

- [x] **Step 8: Verify an explicitly disabled build**

Configure a separate generated tree so the primary task build remains intact:

```bash
cmake -S . -B build-souffle-off -G Ninja \
  -DLLVM_PROJECT_BUILD_DIR=/Users/skg7on/Workspace/Projects/llvm-project/build \
  -DVERITAS_ENABLE_SOUFFLE=OFF
cmake --build build-souffle-off --target FixpointEngineTest SouffleExporterTest
ctest --test-dir build-souffle-off \
  -R "FixpointEngineTest|SouffleExporterTest" --output-on-failure
```

Expected: configure/build/tests pass without Souffle headers, libraries, or
executable.

- [x] **Step 9: Commit Task 8**

```bash
git add CMakeLists.txt cmake/Dependencies.cmake \
  include/veritas/facts/SouffleRunner.h src/facts/SouffleRunner.cpp \
  src/facts/rules src/facts/CMakeLists.txt \
  tests/integration/CMakeLists.txt tests/integration/facts
git commit -m "feat(facts): add optional Souffle rule execution"
```

---

### Task 9: End-to-End M8 Acceptance Fixture

**Files:**
- Create: `tests/integration/wpa/WpaEndToEndTest.cpp`
- Create: `tests/integration/wpa/CMakeLists.txt`
- Modify: `tests/integration/CMakeLists.txt`
- Modify: `CLAUDE.md`
- Modify: `docs/specs/milestones/m08-scc-wpa-souffle-fact-engine-design-spec.md`

**Interfaces:**
- Consumes: `SummaryRepository`, `ListCurrentSummaries`, `CallGraph`, `SccGraph`, `FixpointEngine`, `SccStateRepository`, `WpaCoordinator`, and M7 `WorklistScheduler`.
- Produces: one integration test proving the complete C++ M8 path without Souffle.
- Produces: updated project/milestone status only after the acceptance test passes.

- [x] **Step 1: Add the failing end-to-end test target**

Build three synthetic summaries in reverse input order:

```text
A MUST-calls B
B MUST-calls C
C MUST-writes X
A also has one unresolved UNKNOWN call to vendor_validate
```

Publish them through `SummaryRepository`, then exercise only public APIs:

```cpp
auto summaries = repository->ListCurrentSummaries(revision, build_variant);
auto call_graph = wpa::CallGraph::FromSummaries(*summaries);
auto scc_graph = wpa::SccGraph::Build(*call_graph);
wpa::FixpointEngine engine(*call_graph, *scc_graph, *summaries);
auto results = engine.ComputeAll(v1::COMPONENT_KIND_MEMORY_EFFECTS,
                                 {.max_iterations = 32});
```

Assert `MayWrite(A,X)` is `MUST`, its tuple has rule/input provenance, and A's
unknown marker is scoped only to A.

- [x] **Step 2: Extend the test through persistence and M7 propagation**

Use `SccStateRepository(repository->metadata_store())` to publish topology and
store each result. For the first publication, pass every `kChanged` result to
`WpaCoordinator`; assert predecessor work items target SCC IDs and are
deduplicated. Store a second result with a changed fixpoint hash and identical
external hash; assert no new predecessor item is added.

- [x] **Step 3: Build/run and fix only integration defects**

```bash
cmake --preset default -DLLVM_PROJECT_BUILD_DIR=/Users/skg7on/Workspace/Projects/llvm-project/build
cmake --build --preset default --target WpaEndToEndTest
ctest --test-dir build -R WpaEndToEndTest --output-on-failure
```

Expected: pass. If it fails, fix the owning production unit and rerun both its
unit target and this integration target; do not duplicate production logic in
the test.

- [x] **Step 4: Update milestone status after green**

Change the M8 spec status from `Approved for implementation` to `Implemented`.
Update `CLAUDE.md` Current State to state that M0-M8 are implemented and that
M9-M12 remain. Mention deterministic SCC/fixpoint, persisted SCC hashes, M7
external-change propagation, and optional Souffle relation execution; do not
claim Souffle was executed on a machine where it is absent.

- [x] **Step 5: Run M8 acceptance tests**

```bash
cmake --build --preset default --target \
  SccGraphTest FixpointEngineTest SccStateRepositoryTest WpaCoordinatorTest \
  FactSchemaTest SouffleExporterTest SouffleRunnerTest WpaEndToEndTest
ctest --test-dir build \
  -R "SccGraphTest|FixpointEngineTest|SccStateRepositoryTest|WpaCoordinatorTest|FactSchemaTest|SouffleExporterTest|SouffleRunnerTest|WpaEndToEndTest" \
  --output-on-failure
```

Expected: every mandatory test passes; only the executable Souffle comparison
may be explicitly skipped when unavailable.

- [x] **Step 6: Commit Task 9**

```bash
git add tests/integration/CMakeLists.txt tests/integration/wpa CLAUDE.md \
  docs/specs/milestones/m08-scc-wpa-souffle-fact-engine-design-spec.md
git commit -m "test(wpa): verify M8 end-to-end acceptance"
```

---

### Task 10: Milestone and Pre-Push Verification

**Files:**
- Verify all M8-modified files against `main`.
- Do not add generated build trees or Souffle execution artifacts.

- [x] **Step 1: Verify the task branch diff**

```bash
git status --short
git diff main...HEAD --stat
git diff main...HEAD --check
```

Expected: only M8 source, tests, build files, and approved documentation appear;
no whitespace errors or generated artifacts appear.

- [x] **Step 2: Verify license headers**

Run the repository policy check:

```bash
missing=$(git ls-files \
  'CMakeLists.txt' 'cmake' 'include' 'src' 'tests' \
  | grep -v -E '^(third_party|build|build-souffle-off)/' \
  | grep -v -E '\.(json|md|rst)$' \
  | while IFS= read -r file; do
      head -20 "$file" | grep -q \
        'Licensed under the Apache License, Version 2.0' || echo "$file"
    done)
test -z "$missing" || { printf 'missing license header:\n%s\n' "$missing" >&2; exit 1; }
```

Expected: no missing headers.

Result: every M8-touched source file has the required header. The repository-wide
check still reports five unchanged `frontend_features` fixture files from `main`.

- [x] **Step 3: Run a clean canonical build**

From the task worktree only:

```bash
rm -rf build
cmake --preset default \
  -DLLVM_PROJECT_BUILD_DIR=/Users/skg7on/Workspace/Projects/llvm-project/build
cmake --build --preset default
```

Expected: zero build errors. Record warnings accurately; duplicate Homebrew
GoogleTest linker warnings are baseline warnings, not M8 regressions.

- [x] **Step 4: Run the complete test suite**

```bash
ctest --test-dir build --output-on-failure
```

Expected: 100% of registered tests pass. An unavailable Souffle executable may
produce an explicit GTest skip, not a failed or absent C++ WPA test.

- [x] **Step 5: Re-run the explicitly disabled Souffle build**

```bash
cmake -S . -B build-souffle-off -G Ninja \
  -DLLVM_PROJECT_BUILD_DIR=/Users/skg7on/Workspace/Projects/llvm-project/build \
  -DVERITAS_ENABLE_SOUFFLE=OFF
cmake --build build-souffle-off --target FixpointEngineTest SouffleExporterTest
ctest --test-dir build-souffle-off \
  -R "FixpointEngineTest|SouffleExporterTest" --output-on-failure
```

Expected: all selected mandatory tests pass.

- [x] **Step 6: Request code review and resolve findings**

Review the full branch diff for spec compliance, correctness, error handling,
test quality, and scope. For every accepted finding, write or identify the
failing regression test, verify red, apply the minimum fix, and rerun the owning
suite. Then repeat Steps 1-5.

- [x] **Step 7: Verify clean committed states**

```bash
git status --porcelain
git -C /Users/skg7on/Workspace/Projects/VERITAS branch --show-current
git -C /Users/skg7on/Workspace/Projects/VERITAS status --porcelain
git branch --show-current
```

Expected: primary checkout is clean on `main`; task worktree is clean on
`claude/m8-scc-wpa-souffle`.

- [ ] **Step 8: Push and open the milestone pull request**

```bash
git push -u origin claude/m8-scc-wpa-souffle
gh pr create \
  --base main \
  --head claude/m8-scc-wpa-souffle \
  --title "feat: add SCC-aware WPA and Souffle fact engine" \
  --body-file /tmp/veritas-m8-pr-body.md
```

The PR body must summarize the C++ SCC/fixpoint engine, persisted hash/state
behavior, M7 propagation, fact provenance boundary, optional Souffle status,
and exact clean-build/test results. It must link issue `#11` and state that
Souffle execution was skipped locally if the executable remains unavailable.

---
