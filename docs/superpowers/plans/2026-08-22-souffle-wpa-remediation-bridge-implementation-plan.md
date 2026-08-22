# Soufflé WPA Remediation Bridge Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver M8R.1 through M8R.5 so VERITAS uses compiled Soufflé as the normal recursive WPA engine, retains C++ only as a conformance oracle or explicitly selected emergency engine, and satisfies every M9 entry criterion.

**Architecture:** Keep Function Summary IR as the durable WPA contract, add `summary.v2` semantic precision, and materialize typed run-local `relations.v2` inputs per SCC. Execute those inputs through a private compiled-Soufflé adapter, canonicalize semantic results and generic witness edges into VERITAS-owned facts, and publish only validated complete component results. The existing C++ fixed-point logic is moved behind the same executor interface so it can compare results without becoming a silent production fallback.

**Tech Stack:** C++20, LLVM/Clang 22+, pinned SVF, Protobuf, Soufflé compiled C++ interface, CMake/Ninja, SQLite, RocksDB, GoogleTest, Python 3 schema-generation checks.

**Spec:** `docs/superpowers/specs/2026-08-22-souffle-wpa-architecture-refinement-design.md`

## Global Constraints

- `summary.v1` remains immutable and readable; native reanalysis emits `summary.v2`.
- Function Summary IR is the durable WPA contract; detailed relation rows are run-local projections.
- Pinned SVF remains authoritative for V1 Andersen points-to results, aliases, SVFG flow, and indirect-call candidates.
- Compiled Soufflé is the default and required production recursive-WPA engine.
- The supported production toolchain pins Soufflé release `2.5` at full source revision `5682a9f12e2668ecdd26348fe63cc508bc0fcf47`, verifies an install provenance manifest and executable digest, records that provenance in engine/toolchain identity, and runs generated programs with one evaluation thread until the upstream ARM concurrency issue is retired by a separately qualified upgrade.
- C++ is a differential oracle and explicit `cpp-emergency` engine; automatic fallback is forbidden.
- Conformance materializes one canonical engine-neutral logical component input, then derives distinct Soufflé and C++ execution envelopes and `RunId` values from it.
- Stable identities and typed dense IDs are separate; dense IDs never escape one `AnalysisRun`.
- Semantic values and epistemic state remain independent, including `NO_ALIAS + MUST`.
- Unknown and negative information is represented explicitly and is never omitted merely because it is not positive.
- Every published derived fact has a finite rooted witness selected independently of engine tuple order.
- A failed component publishes no new result; a previous success is retained only as stale history.
- `RunId` includes revision, build variant, summary schema, relation schema, rule bundle, model bundle, SVF configuration, WPA configuration, engine identity, and exact engine/toolchain identity.
- Cross-revision component reuse is content-addressed by engine-neutral logical input plus exact executor/toolchain provenance; each run records its own result reference and run histories are never merged.
- Fact Bus batches carry expected/completed component metadata and rooted input IDs; sink delivery is idempotent at least once by canonical batch identity.
- M9 work starts only after the qualification task at the end of this plan passes.

## Scope Decomposition

This plan covers the sequential remediation bridge only:

| Gate | Tasks | Independently reviewable result |
|---|---:|---|
| M8R.1 Semantic Fact Contract | 1–4 | Typed identities, semantic/epistemic enums, run manifests, relation registry, dense maps, and V1 fact compatibility |
| M8R.2 SVF and Memory Refinement | 5–9 | Native `summary.v2`, version-aware storage, collision-free memory/value identity, indirect calls, aliases, models |
| M8R.3 Relational WPA Projection | 10–12 | Per-SCC materialization, generic witnesses, matched Soufflé/C++ logical inputs and V2 rule bundles |
| M8R.4 Production Soufflé WPA | 13–15 | Compiled rules, production executor/orchestrator, cache/failure state, standard pipeline integration |
| M8R.5 Qualification and M9 Handoff | 16–18 | Differential and failure corpus, generic Fact Bus, synchronized documentation and executable M9 gate |

M9 persistence, M10A domain expansion, M10B Evidence APIs, M11, M12, and M13 each require a separate implementation plan after this bridge is qualified.

---

## M8R.1 — Semantic Fact Contract

### Task 1: Add typed stable IDs and semantic value types

**Files:**

- Modify: `include/veritas/core/Ids.h`
- Modify: `src/core/Ids.cpp`
- Modify: `tests/unit/core/IdsTest.cpp`
- Create: `include/veritas/analysis/semantic/SemanticTypes.h`
- Create: `src/analysis/semantic/SemanticTypes.cpp`
- Create: `tests/unit/analysis/SemanticTypesTest.cpp`
- Modify: `src/analysis/CMakeLists.txt`
- Modify: `tests/unit/analysis/CMakeLists.txt`

**Interfaces:**

- Produces: `IdKind::kAnalysisRun`, `IdKind::kAbstractObject`, and `IdKind::kModel`.
- Produces: `semantic::EpistemicState`, `AliasKind`, `DispatchKind`, `AbstractObjectKind`, `AccessPathSegment`, `ByteRange`, `AbstractObject`, and `MemoryLocation`.
- Produces: `ByteRangeKind::kKnown` and `ByteRangeKind::kUnknown` for lossless relation projection.
- Produces: `Status Validate(const AbstractObject&)` and `Status Validate(const MemoryLocation&)`.
- Consumed by: Tasks 2–12.

- [ ] **Step 1: Write stable-ID and semantic-separation tests**

```cpp
TEST(IdsTest, AnalysisRunAndAbstractObjectIdsRoundTrip) {
  const std::array bytes{std::byte{0x01}};
  for (auto kind : {IdKind::kAnalysisRun, IdKind::kAbstractObject,
                    IdKind::kModel}) {
    const auto id = MakeStableId(kind, bytes);
    ASSERT_TRUE(ParseStableId(ToString(id)).ok());
    EXPECT_EQ(*ParseStableId(ToString(id)), id);
  }
}

TEST(SemanticTypesTest, NoAliasIsNotUnknownAlias) {
  AliasObservation proven_no_alias{AliasKind::kNoAlias,
                                   EpistemicState::kMust};
  AliasObservation unknown{AliasKind::kUnknownAlias,
                           EpistemicState::kUnknown};
  EXPECT_NE(proven_no_alias, unknown);
}

TEST(SemanticTypesTest, UnknownByteRangeIsExplicit) {
  ByteRange range = ByteRange::Unknown();
  EXPECT_FALSE(range.offset.has_value());
  EXPECT_FALSE(range.size.has_value());
  EXPECT_TRUE(Validate(range).ok());
}

TEST(SemanticTypesTest, UnknownRangeDiffersFromKnownZeroRange) {
  EXPECT_NE(ByteRange::Unknown(), ByteRange::Known(0, 0));
  EXPECT_EQ(RelationRangeKind(ByteRange::Unknown()), ByteRangeKind::kUnknown);
  EXPECT_EQ(RelationRangeKind(ByteRange::Known(0, 0)), ByteRangeKind::kKnown);
}
```

- [ ] **Step 2: Build the focused tests and confirm they fail**

Run:

```bash
cmake --preset default -DLLVM_PROJECT_BUILD_DIR=/Users/skg7on/Workspace/Projects/llvm-project/build
cmake --build --preset default --target IdsTest SemanticTypesTest
```

Expected: compilation fails because the new ID kinds and semantic types do not exist.

- [ ] **Step 3: Implement the typed semantic contract**

```cpp
namespace veritas::analysis::semantic {

enum class EpistemicState : std::uint8_t {
  kMust,
  kMay,
  kMustNot,
  kInferred,
  kAssumed,
  kUnknown,
};

enum class AliasKind : std::uint8_t {
  kMustAlias,
  kMayAlias,
  kNoAlias,
  kUnknownAlias,
};

enum class DispatchKind : std::uint8_t {
  kDirect,
  kIndirect,
  kVirtual,
  kCallback,
  kExternal,
  kUnknown,
};

enum class AbstractObjectKind : std::uint8_t {
  kGlobal,
  kStack,
  kHeap,
  kArgument,
  kFunction,
  kExternal,
  kUnknown,
  kLegacyOpaque,
};

enum class ByteRangeKind : std::uint8_t { kKnown, kUnknown };

struct AliasObservation {
  AliasKind kind;
  EpistemicState epistemic;
  auto operator<=>(const AliasObservation&) const = default;
};

struct AccessPathSegment {
  enum class Kind : std::uint8_t { kField, kArrayIndex, kArrayRange, kUnknown };
  Kind kind;
  std::int64_t first = 0;
  std::int64_t last = 0;
  auto operator<=>(const AccessPathSegment&) const = default;
};

struct ByteRange {
  std::optional<std::int64_t> offset;
  std::optional<std::uint64_t> size;
  static ByteRange Unknown() { return {}; }
  static ByteRange Known(std::int64_t offset, std::uint64_t size) {
    return {.offset = offset, .size = size};
  }
  auto operator<=>(const ByteRange&) const = default;
};

struct AbstractObject {
  core::StableId id;
  AbstractObjectKind kind;
  std::optional<core::StableId> owner_function;
  std::string semantic_anchor;
  std::string diagnostic_name;
};

struct MemoryLocation {
  core::StableId id;
  AbstractObject object;
  std::vector<AccessPathSegment> access_path;
  ByteRange byte_range;
};

}  // namespace veritas::analysis::semantic
```

Use serialized prefixes `run`, `obj`, and `model` in `Ids.cpp`. Validation must reject mismatched ID kinds, half-known ranges, invalid owners, and control characters in semantic anchors while allowing signed offsets and empty diagnostic names.

- [ ] **Step 4: Run the focused tests**

Run:

```bash
cmake --build --preset default --target IdsTest SemanticTypesTest
./build/bin/IdsTest
./build/bin/SemanticTypesTest
```

Expected: both executables report all tests passed.

- [ ] **Step 5: Commit the semantic types**

```bash
git add include/veritas/core/Ids.h src/core/Ids.cpp tests/unit/core/IdsTest.cpp include/veritas/analysis/semantic/SemanticTypes.h src/analysis/semantic/SemanticTypes.cpp tests/unit/analysis/SemanticTypesTest.cpp src/analysis/CMakeLists.txt tests/unit/analysis/CMakeLists.txt
git commit -m "feat: add typed semantic identities"
```

### Task 2: Add reproducible WPA analysis-run identity

**Files:**

- Create: `include/veritas/facts/AnalysisRun.h`
- Create: `src/facts/AnalysisRun.cpp`
- Create: `tests/unit/facts/AnalysisRunTest.cpp`
- Modify: `src/facts/CMakeLists.txt`
- Modify: `tests/unit/facts/CMakeLists.txt`

**Interfaces:**

- Consumes: `core::IdKind::kAnalysisRun` from Task 1.
- Produces: `facts::EngineIdentity`, engine-neutral `AnalysisRunSemanticDescriptor`, `AnalysisRunDescriptor`, and immutable `AnalysisRunManifest`.
- Produces: `StatusOr<AnalysisRunManifest> MakeAnalysisRun(AnalysisRunDescriptor)`.
- Consumed by: materialization, execution, persistence, and publication in Tasks 10–17.

- [ ] **Step 1: Write manifest identity tests**

```cpp
TEST(AnalysisRunTest, EveryDescriptorFieldChangesRunId) {
  auto base = ValidDescriptor();
  ASSERT_TRUE(MakeAnalysisRun(base).ok());
  const auto base_id = MakeAnalysisRun(base)->run_id;

  auto changed = base;
  changed.engine = EngineIdentity::kCppEmergency;
  EXPECT_NE(MakeAnalysisRun(changed)->run_id, base_id);

  changed = base;
  changed.rule_bundle_version = "wpa.rules.v2.1";
  EXPECT_NE(MakeAnalysisRun(changed)->run_id, base_id);

  changed = base;
  changed.model_bundle_version = "models.v2";
  EXPECT_NE(MakeAnalysisRun(changed)->run_id, base_id);

  changed = base;
  changed.engine_toolchain_identity = "souffle-2.5+other-build";
  EXPECT_NE(MakeAnalysisRun(changed)->run_id, base_id);
}

TEST(AnalysisRunTest, RejectsEmptyVersionOrConfigurationFields) {
  auto descriptor = ValidDescriptor();
  descriptor.relation_schema_version.clear();
  EXPECT_EQ(MakeAnalysisRun(descriptor).status().code(),
            StatusCode::kInvalidArgument);
}
```

- [ ] **Step 2: Build and confirm the tests fail**

Run:

```bash
cmake --build --preset default --target AnalysisRunTest
```

Expected: compilation fails because `AnalysisRun.h` is absent.

- [ ] **Step 3: Implement manifest canonicalization**

```cpp
enum class EngineIdentity : std::uint8_t {
  kSouffle,
  kCppConformance,
  kCppEmergency,
};

struct AnalysisRunSemanticDescriptor {
  core::StableId build_variant_id;
  std::string summary_schema_version;
  std::string relation_schema_version;
  std::string rule_bundle_version;
  std::string model_bundle_version;
  std::string svf_configuration_hash;
  std::string wpa_configuration_hash;
};

struct AnalysisRunDescriptor : AnalysisRunSemanticDescriptor {
  core::StableId revision_id;
  EngineIdentity engine;
  std::string engine_toolchain_identity;
};

struct AnalysisRunManifest : AnalysisRunDescriptor {
  core::StableId run_id;
};

StatusOr<AnalysisRunManifest> MakeAnalysisRun(AnalysisRunDescriptor descriptor);
```

Canonicalize with length-prefixed fields beginning with `veritas.wpa-run.v1`; validate the revision/build ID kinds, non-empty version/toolchain strings, lowercase 64-character configuration hashes, and recognized engine value before deriving `run_id`. `AnalysisRunSemanticDescriptor` is the engine-neutral subset used by Task 10; `revision_id`, `engine`, and `engine_toolchain_identity` belong only to the execution envelope and run identity.

- [ ] **Step 4: Run focused tests**

Run:

```bash
cmake --build --preset default --target AnalysisRunTest
./build/bin/AnalysisRunTest
```

Expected: all manifest validation and identity tests pass.

- [ ] **Step 5: Commit the run contract**

```bash
git add include/veritas/facts/AnalysisRun.h src/facts/AnalysisRun.cpp tests/unit/facts/AnalysisRunTest.cpp src/facts/CMakeLists.txt tests/unit/facts/CMakeLists.txt
git commit -m "feat: add reproducible WPA run manifests"
```

### Task 3: Replace string-vector authority with a typed V2 relation registry

**Files:**

- Create: `include/veritas/facts/RelationSchema.h`
- Create: `include/veritas/facts/AnalysisFact.h`
- Create: `src/facts/RelationSchema.cpp`
- Create: `src/facts/AnalysisFact.cpp`
- Create: `logic/schema/relations.v2.json`
- Create: `tests/unit/facts/RelationSchemaTest.cpp`
- Create: `tests/unit/facts/AnalysisFactTest.cpp`
- Modify: `src/facts/CMakeLists.txt`
- Modify: `tests/unit/facts/CMakeLists.txt`

**Interfaces:**

- Consumes: semantic enums from Task 1.
- Produces: `RelationId`, `ColumnDomain`, `ColumnSpec`, `RelationSchema`, `SemanticCellValue`, `SemanticRow`, `ExecutionCellValue`, `ExecutionRow`, `AnalysisFact`, `RelationsV2()`, `ValidateSemanticRow()`, `ValidateExecutionRow()`, and `MakeFact()`.
- Maintains: existing `FactTuple` as the M8 compatibility type until Task 4 adapts it.
- Consumed by: every V2 materializer, executor, witness, and Fact Bus component.

- [ ] **Step 1: Write typed-row and fact-identity tests**

```cpp
TEST(RelationSchemaTest, DirectCallHasTypedV2Columns) {
  const auto& schema = RelationsV2().Get(RelationId::kDirectCall);
  EXPECT_EQ(schema.name, "DirectCall");
  EXPECT_EQ(schema.columns[0].domain, ColumnDomain::kCallSiteId);
  EXPECT_EQ(schema.columns[1].domain, ColumnDomain::kFunctionId);
  EXPECT_EQ(schema.columns[2].domain, ColumnDomain::kFunctionId);
  EXPECT_EQ(schema.columns[3].domain, ColumnDomain::kDispatchKind);
  EXPECT_EQ(schema.columns[4].domain, ColumnDomain::kEpistemic);
}

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

TEST(RelationSchemaTest, RejectsCrossDomainDenseId) {
  ExecutionRow row{RelationId::kDirectCall,
                   {MemoryId{1}, FunctionId{1}, FunctionId{2},
                    DispatchKind::kDirect, EpistemicState::kMust}};
  EXPECT_FALSE(ValidateExecutionRow(row).ok());
}

TEST(RelationSchemaTest, DirectReadPreservesUnknownRangeTag) {
  auto row = DirectReadExecutionRow(
      FunctionId{1}, MemoryId{2}, ByteRange::Unknown(),
      EpistemicState::kMay);
  ASSERT_TRUE(ValidateExecutionRow(row).ok());
  EXPECT_EQ(row.cells[2], ByteRangeKind::kUnknown);
  EXPECT_EQ(row.cells[3], std::int64_t{0});
  EXPECT_EQ(row.cells[4], std::uint64_t{0});
}
```

- [ ] **Step 2: Build and confirm the tests fail**

Run:

```bash
cmake --build --preset default --target RelationSchemaTest AnalysisFactTest
```

Expected: compilation fails because the V2 schema types are absent.

- [ ] **Step 3: Implement the typed schema and fact identity**

```cpp
template <typename Tag>
struct DenseId {
  std::uint32_t value;
  auto operator<=>(const DenseId&) const = default;
};

using FunctionId = DenseId<struct FunctionTag>;
using ValueId = DenseId<struct ValueTag>;
using MemoryId = DenseId<struct MemoryTag>;
using CallSiteId = DenseId<struct CallSiteTag>;
using FactId = DenseId<struct FactTag>;

enum class RelationId : std::uint16_t {
  kFunctionMap,
  kValueMap,
  kMemoryMap,
  kCallSiteMap,
  kFactMap,
  kDirectCall,
  kUnknownCall,
  kDirectRead,
  kDirectWrite,
  kAlias,
  kLocalFlow,
  kParameterFlow,
  kReturnFlow,
  kModeledEffect,
  kUnsupportedFeature,
  kReachableCall,
  kMayWrite,
};

using SemanticCellValue =
    std::variant<core::StableId, std::int64_t, std::uint64_t, std::string,
                 semantic::DispatchKind, semantic::AliasKind,
                 semantic::ByteRangeKind,
                 semantic::EpistemicState>;

using ExecutionCellValue =
    std::variant<FunctionId, ValueId, MemoryId, CallSiteId, FactId,
                 std::int64_t, std::uint64_t, std::string,
                 semantic::DispatchKind, semantic::AliasKind,
                 semantic::ByteRangeKind,
                 semantic::EpistemicState>;

struct SemanticRow {
  RelationId relation;
  std::vector<SemanticCellValue> cells;
};

struct ExecutionRow {
  RelationId relation;
  std::vector<ExecutionCellValue> cells;
};

struct AnalysisFact {
  core::StableId fact_id;
  SemanticRow row;
};
```

`MakeFact` validates stable-ID kinds through `RelationsV2()`, serializes `relations.v2`, relation name, typed stable semantic cells, and epistemic value, then derives a `kFact` ID. It must not include dense IDs, engine identity, tuple order, rule ID, or witness edges. `ValidateExecutionRow` separately checks each dense-ID domain for the run-local execution projection.

Add every relation and column listed in design sections 7 and 11 to `logic/schema/relations.v2.json`; mark EDB/IDB ownership and allowed epistemic values explicitly. `DirectRead` and `DirectWrite` contain `RangeKind`, signed `Offset`, and unsigned `Size`: `KNOWN` consumes both payload cells, while `UNKNOWN` requires canonical zeros that are ignored. Reject half-known ranges and non-canonical unknown payloads so a known zero range never collapses into unknown.

- [ ] **Step 4: Run schema and fact tests**

Run:

```bash
cmake --build --preset default --target RelationSchemaTest AnalysisFactTest
./build/bin/RelationSchemaTest
./build/bin/AnalysisFactTest
```

Expected: typed-domain validation, complete registry lookup, and deterministic fact identity tests pass.

- [ ] **Step 5: Commit the V2 relation contract**

```bash
git add include/veritas/facts/RelationSchema.h include/veritas/facts/AnalysisFact.h src/facts/RelationSchema.cpp src/facts/AnalysisFact.cpp logic/schema/relations.v2.json tests/unit/facts/RelationSchemaTest.cpp tests/unit/facts/AnalysisFactTest.cpp src/facts/CMakeLists.txt tests/unit/facts/CMakeLists.txt
git commit -m "feat: add typed V2 relation schema"
```

### Task 4: Add run-local dense mappings and a lossless M8 compatibility adapter

**Files:**

- Create: `include/veritas/facts/DenseIdMap.h`
- Create: `include/veritas/facts/LegacyFactAdapter.h`
- Create: `src/facts/LegacyFactAdapter.cpp`
- Create: `tests/unit/facts/DenseIdMapTest.cpp`
- Create: `tests/unit/facts/LegacyFactAdapterTest.cpp`
- Modify: `src/facts/CMakeLists.txt`
- Modify: `tests/unit/facts/CMakeLists.txt`

**Interfaces:**

- Consumes: `FactTuple`, V1 `FunctionSummary`, `AnalysisRunManifest`, `SemanticRow`, and typed dense IDs.
- Produces: `DenseIdMap<DenseIdType, StableIdKind>` with deterministic `Build`, `ToDense`, and `ToStable` operations.
- Produces: `StatusOr<LegacyProjection> ProjectLegacyFacts(run, facts)` for validated positive M8 tuples and `StatusOr<LegacyProjection> ProjectLegacySummaries(run, summaries)` for the complete V1 semantic surface.
- Guarantees: all six platform epistemic states either round-trip to V2 or become an explicit `UnsupportedFeature` row; none is silently removed.

- [ ] **Step 1: Write deterministic mapping and uncertainty tests**

```cpp
TEST(DenseIdMapTest, SortsStableIdsBeforeAssigningDenseIds) {
  auto high = Stable(IdKind::kFunctionVariant, 0xf0);
  auto low = Stable(IdKind::kFunctionVariant, 0x10);
  auto map = FunctionDenseMap::Build({high, low});
  ASSERT_TRUE(map.ok());
  EXPECT_LT(map->ToDense(low)->value, map->ToDense(high)->value);
  EXPECT_EQ(*map->ToStable(*map->ToDense(high)), high);
}

TEST(DenseIdMapTest, RejectsStableIdsFromAnotherDomain) {
  auto memory = Stable(IdKind::kMemoryRef, 0x10);
  EXPECT_FALSE(FunctionDenseMap::Build({memory}).ok());
}

TEST(LegacyFactAdapterTest, PreservesNegativeAndUnknownEpistemicStates) {
  for (auto state : {v1::EPISTEMIC_STATE_MUST_NOT,
                     v1::EPISTEMIC_STATE_INFERRED,
                     v1::EPISTEMIC_STATE_ASSUMED,
                     v1::EPISTEMIC_STATE_UNKNOWN}) {
    auto projection =
        ProjectLegacySummaries(Run(), {LegacySummaryWithCall(state)});
    ASSERT_TRUE(projection.ok());
    EXPECT_FALSE(projection->rows.empty());
  }
}
```

- [ ] **Step 2: Build and confirm the tests fail**

Run:

```bash
cmake --build --preset default --target DenseIdMapTest LegacyFactAdapterTest
```

Expected: compilation fails because the mapping and adapter interfaces are absent.

- [ ] **Step 3: Implement deterministic domain maps and compatibility projection**

```cpp
template <typename Dense, core::IdKind Kind>
class DenseIdMap {
 public:
  static StatusOr<DenseIdMap> Build(std::vector<core::StableId> stable_ids);
  StatusOr<Dense> ToDense(const core::StableId& stable) const;
  StatusOr<core::StableId> ToStable(Dense dense) const;
  std::span<const core::StableId> StableIds() const;

 private:
  std::vector<core::StableId> dense_to_stable_;
  std::map<core::StableId, Dense> stable_to_dense_;
};

using FunctionDenseMap =
    DenseIdMap<FunctionId, core::IdKind::kFunctionVariant>;
using ValueDenseMap = DenseIdMap<ValueId, core::IdKind::kValueRef>;
using MemoryDenseMap = DenseIdMap<MemoryId, core::IdKind::kMemoryRef>;
using CallSiteDenseMap = DenseIdMap<CallSiteId, core::IdKind::kCallSite>;
```

The tuple adapter accepts only validated M8 tuples. The summary adapter reads every V1 call, memory effect, flow, alias, unknown, and assumption directly so information that M8 never projected is still available. Positive rows map to their V2 semantic relations. V1 memory strings produce stable `kMemoryRef` values with object kind `kLegacyOpaque`. Negative, assumed, inferred, and unknown rows preserve their epistemic value when the V2 relation permits it; otherwise they generate `UnsupportedFeature` with reason `legacy-epistemic-not-consumed` and retain a stable origin fact ID.

- [ ] **Step 4: Run focused and existing M8 fact tests**

Run:

```bash
cmake --build --preset default --target DenseIdMapTest LegacyFactAdapterTest FactSchemaTest SummaryFactBuilderTest
./build/bin/DenseIdMapTest
./build/bin/LegacyFactAdapterTest
./build/bin/FactSchemaTest
./build/bin/SummaryFactBuilderTest
```

Expected: all four test executables pass; existing M8 behavior remains readable and the new adapter retains every epistemic state.

- [ ] **Step 5: Commit M8R.1**

```bash
git add include/veritas/facts/DenseIdMap.h include/veritas/facts/LegacyFactAdapter.h src/facts/LegacyFactAdapter.cpp tests/unit/facts/DenseIdMapTest.cpp tests/unit/facts/LegacyFactAdapterTest.cpp src/facts/CMakeLists.txt tests/unit/facts/CMakeLists.txt
git commit -m "feat: add dense mappings and legacy fact projection"
```

---
## M8R.2 — SVF and Memory Refinement

### Task 5: Introduce the native `summary.v2` protobuf and deterministic builder

**Files:**

- Create: `proto/veritas/summary/v2/summary.proto`
- Create: `include/veritas/summary/SummaryV2Builder.h`
- Create: `src/summary/SummaryV2Builder.cpp`
- Modify: `include/veritas/summary/FunctionSummary.h`
- Modify: `src/summary/FunctionSummary.cpp`
- Modify: `include/veritas/summary/ComponentHash.h`
- Modify: `src/summary/ComponentHash.cpp`
- Modify: `src/summary/CMakeLists.txt`
- Create: `tests/unit/summary/SummaryV2BuilderTest.cpp`
- Modify: `tests/unit/summary/ComponentHashTest.cpp`
- Modify: `tests/unit/summary/CMakeLists.txt`

**Interfaces:**

- Consumes: semantic types from Task 1 and existing immutable V1 messages by protobuf import only.
- Produces: `veritas::summary::v2::FunctionSummary` with typed call dispatch, stable values, structured memory, alias kind, and full epistemic/provenance fields.
- Produces: `FunctionLocalFactsV2`, `BuildLocalSummaryV2`, and V2 overloads for summary ID and component digest computation.
- Does not modify: `proto/veritas/summary/v1/summary.proto`.
- Consumed by: storage and analysis pipeline Tasks 6–12.

- [ ] **Step 1: Write V2 schema and canonicalization tests**

```cpp
TEST(SummaryV2BuilderTest, SeparatesAliasKindFromEpistemicState) {
  FunctionLocalFactsV2 facts = MinimalFacts();
  facts.aliases.push_back(AliasFactV2{
      .left = Memory("left"),
      .right = Memory("right"),
      .kind = semantic::AliasKind::kNoAlias,
      .epistemic = semantic::EpistemicState::kMust,
      .provenance_ref = "svf:alias",
  });
  auto summary = BuildLocalSummaryV2(facts, Context());
  ASSERT_TRUE(summary.ok());
  EXPECT_EQ(summary->alias_facts(0).kind(), v2::ALIAS_KIND_NO_ALIAS);
  EXPECT_EQ(summary->alias_facts(0).epistemic(),
            v1::EPISTEMIC_STATE_MUST);
}

TEST(SummaryV2BuilderTest, EquivalentInputOrderHasOneSummaryId) {
  auto left = BuildLocalSummaryV2(FactsInForwardOrder(), Context());
  auto right = BuildLocalSummaryV2(FactsInReverseOrder(), Context());
  ASSERT_TRUE(left.ok());
  ASSERT_TRUE(right.ok());
  EXPECT_EQ(*ComputeFunctionSummaryId(*left),
            *ComputeFunctionSummaryId(*right));
}
```

- [ ] **Step 2: Build and confirm the tests fail**

Run:

```bash
cmake --build --preset default --target SummaryV2BuilderTest ComponentHashTest
```

Expected: build fails because the V2 proto and builder do not exist.

- [ ] **Step 3: Add the V2 protobuf contract**

```proto
syntax = "proto3";
package veritas.summary.v2;

import "veritas/summary/v1/summary.proto";

message FunctionSummary {
  veritas.summary.v1.SummaryHeader header = 1;
  veritas.summary.v1.FunctionIdentity identity = 2;
  repeated veritas.summary.v1.ComponentDigest component_digests = 3;
  repeated Call calls = 10;
  repeated MemoryEffect memory_effects = 11;
  repeated ValueFlow value_flows = 12;
  repeated veritas.summary.v1.ControlFlowSummary control_flow = 13;
  repeated veritas.summary.v1.RangeFact range_facts = 14;
  repeated AliasFact alias_facts = 15;
  repeated veritas.summary.v1.TaintTransfer taint_transfers = 16;
  repeated veritas.summary.v1.OwnershipEffect ownership_effects = 17;
  repeated veritas.summary.v1.LockEffect lock_effects = 18;
  repeated veritas.summary.v1.StateTransition state_transitions = 19;
  repeated veritas.summary.v1.Unknown unknowns = 20;
  repeated veritas.summary.v1.Assumption assumptions = 21;
  repeated veritas.summary.v1.Dependency dependencies = 22;
  repeated veritas.summary.v1.ProvenanceRef provenance_refs = 30;
}

enum DispatchKind {
  DISPATCH_KIND_UNSPECIFIED = 0;
  DISPATCH_KIND_DIRECT = 1;
  DISPATCH_KIND_INDIRECT = 2;
  DISPATCH_KIND_VIRTUAL = 3;
  DISPATCH_KIND_CALLBACK = 4;
  DISPATCH_KIND_EXTERNAL = 5;
  DISPATCH_KIND_UNKNOWN = 6;
}

enum AliasKind {
  ALIAS_KIND_UNSPECIFIED = 0;
  ALIAS_KIND_MUST_ALIAS = 1;
  ALIAS_KIND_MAY_ALIAS = 2;
  ALIAS_KIND_NO_ALIAS = 3;
  ALIAS_KIND_UNKNOWN_ALIAS = 4;
}

enum AbstractObjectKind {
  ABSTRACT_OBJECT_KIND_UNSPECIFIED = 0;
  ABSTRACT_OBJECT_KIND_GLOBAL = 1;
  ABSTRACT_OBJECT_KIND_STACK = 2;
  ABSTRACT_OBJECT_KIND_HEAP = 3;
  ABSTRACT_OBJECT_KIND_ARGUMENT = 4;
  ABSTRACT_OBJECT_KIND_FUNCTION = 5;
  ABSTRACT_OBJECT_KIND_EXTERNAL = 6;
  ABSTRACT_OBJECT_KIND_UNKNOWN = 7;
  ABSTRACT_OBJECT_KIND_LEGACY_OPAQUE = 8;
}

message AbstractObject {
  string abstract_object_id = 1;
  AbstractObjectKind kind = 2;
  string owner_function_variant_id = 3;
  string semantic_anchor_id = 4;
  string diagnostic_name = 5;
}

message AccessPathSegment {
  enum Kind { KIND_UNSPECIFIED = 0; KIND_FIELD = 1; KIND_ARRAY_INDEX = 2;
              KIND_ARRAY_RANGE = 3; KIND_UNKNOWN = 4; }
  Kind kind = 1;
  int64 first = 2;
  int64 last = 3;
}

message ByteRange {
  bool offset_known = 1;
  int64 offset = 2;
  bool size_known = 3;
  uint64 size = 4;
}

message MemoryLocation {
  string memory_location_id = 1;
  AbstractObject object = 2;
  repeated AccessPathSegment access_path = 3;
  ByteRange byte_range = 4;
}

message Call {
  string call_site_id = 1;
  string callee_symbol = 2;
  string resolved_callee_function_variant_id = 3;
  DispatchKind dispatch = 4;
  veritas.summary.v1.EpistemicState epistemic = 5;
  string provenance_ref = 6;
}

message MemoryEffect {
  veritas.summary.v1.EffectKind kind = 1;
  MemoryLocation location = 2;
  veritas.summary.v1.EpistemicState epistemic = 3;
  string provenance_ref = 4;
}

message ValueFlow {
  string source_value_id = 1;
  string destination_value_id = 2;
  veritas.summary.v1.EpistemicState epistemic = 3;
  string provenance_ref = 4;
}

message AliasFact {
  MemoryLocation left = 1;
  MemoryLocation right = 2;
  AliasKind kind = 3;
  veritas.summary.v1.EpistemicState epistemic = 4;
  string provenance_ref = 5;
}
```

Generate V1 and V2 from the same `veritas_summary_proto` target. `BuildLocalSummaryV2` sets `schema_version` to `summary.v2`, pins the timestamp to zero, validates every stable ID, sorts repeated semantic records by deterministic serialization, and computes component digests with the existing semantic/evidence split.

- [ ] **Step 4: Run V1 and V2 summary tests**

Run:

```bash
cmake --build --preset default --target SummaryV2BuilderTest ComponentHashTest
./build/bin/SummaryV2BuilderTest
./build/bin/ComponentHashTest
```

Expected: V2 tests pass and every existing V1 component-hash test remains green.

- [ ] **Step 5: Commit the durable V2 summary schema**

```bash
git add proto/veritas/summary/v2/summary.proto include/veritas/summary/SummaryV2Builder.h src/summary/SummaryV2Builder.cpp include/veritas/summary/FunctionSummary.h src/summary/FunctionSummary.cpp include/veritas/summary/ComponentHash.h src/summary/ComponentHash.cpp src/summary/CMakeLists.txt tests/unit/summary/SummaryV2BuilderTest.cpp tests/unit/summary/ComponentHashTest.cpp tests/unit/summary/CMakeLists.txt
git commit -m "feat: add Function Summary IR v2"
```

### Task 6: Make SummaryDB version-aware without rewriting V1 objects

**Files:**

- Create: `include/veritas/summary/SummaryArtifact.h`
- Create: `src/summary/SummaryArtifact.cpp`
- Modify: `include/veritas/summarydb/SummaryRepository.h`
- Modify: `src/summarydb/SummaryRepository.cpp`
- Modify: `src/summary/CMakeLists.txt`
- Modify: `tests/unit/summarydb/SummaryRepositoryTest.cpp`
- Create: `tests/integration/summarydb/SummaryVersionCompatibilityTest.cpp`
- Modify: `tests/integration/summarydb/CMakeLists.txt`

**Interfaces:**

- Consumes: V1 and V2 summaries and their ID/hash overloads.
- Produces: `using SummaryArtifact = std::variant<v1::FunctionSummary, v2::FunctionSummary>`.
- Produces: `SchemaVersion`, `Identity`, `SerializeSummaryArtifact`, `ParseSummaryArtifact`, and version-neutral component-digest accessors.
- Adds: V2 `PublishSummary`/`PublishProjectSummaries` overloads plus `PutImmutableSummaryArtifacts`, `StageCurrentArtifactBindings`, `ListCurrentSummaryArtifacts`, and `GetSummaryArtifact`.
- Preserves: all V1 repository methods and their serialized bytes.

- [ ] **Step 1: Write mixed-history repository tests**

```cpp
TEST(SummaryVersionCompatibilityTest, ReadsHistoricalV1AfterPublishingV2) {
  auto repository = OpenRepository();
  auto v1_id = repository->PublishSummary(V1Summary(), Context());
  ASSERT_TRUE(v1_id.ok());

  auto v2_id = repository->PublishSummary(V2Summary(), Context());
  ASSERT_TRUE(v2_id.ok());
  EXPECT_NE(*v1_id, *v2_id);

  auto historical = repository->GetSummaryArtifact(*v1_id);
  ASSERT_TRUE(historical.ok());
  EXPECT_TRUE(std::holds_alternative<v1::FunctionSummary>(*historical));

  auto current = repository->GetCurrentSummaryArtifact(FunctionIdText());
  ASSERT_TRUE(current.ok());
  EXPECT_TRUE(std::holds_alternative<v2::FunctionSummary>(*current));
}
```

- [ ] **Step 2: Build and confirm the compatibility test fails**

Run:

```bash
cmake --build --preset default --target SummaryVersionCompatibilityTest SummaryRepositoryTest
```

Expected: compilation fails because version-neutral repository methods are absent.

- [ ] **Step 3: Implement the version-neutral artifact boundary**

```cpp
using SummaryArtifact =
    std::variant<summary::v1::FunctionSummary, summary::v2::FunctionSummary>;

std::string_view SchemaVersion(const SummaryArtifact& artifact);
const summary::v1::FunctionIdentity& Identity(const SummaryArtifact& artifact);
StatusOr<std::vector<std::byte>> SerializeSummaryArtifact(
    const SummaryArtifact& artifact);
StatusOr<SummaryArtifact> ParseSummaryArtifact(
    std::string_view schema_version, std::span<const std::byte> bytes);
StatusOr<core::StableId> ComputeFunctionSummaryId(
    const SummaryArtifact& artifact);
```

`GetSummaryArtifact` first queries `summary_objects.schema_version`, then parses the CAS bytes with the matching protobuf. Unknown schema versions return `FailedPrecondition`. V1 getters call the artifact API and reject a current V2 binding instead of parsing V2 bytes as V1. Existing objects and rows are not mutated.

- [ ] **Step 4: Run repository compatibility and regression tests**

Run:

```bash
cmake --build --preset default --target SummaryVersionCompatibilityTest SummaryRepositoryTest
./build/bin/SummaryVersionCompatibilityTest
./build/bin/SummaryRepositoryTest
```

Expected: V1 history remains readable, V2 becomes current, and the previous repository suite passes.

- [ ] **Step 5: Commit version-aware SummaryDB access**

```bash
git add include/veritas/summary/SummaryArtifact.h src/summary/SummaryArtifact.cpp include/veritas/summarydb/SummaryRepository.h src/summarydb/SummaryRepository.cpp src/summary/CMakeLists.txt tests/unit/summarydb/SummaryRepositoryTest.cpp tests/integration/summarydb/SummaryVersionCompatibilityTest.cpp tests/integration/summarydb/CMakeLists.txt
git commit -m "feat: support versioned summary artifacts"
```

### Task 7: Build collision-free LLVM value and abstract-memory identities

**Files:**

- Create: `src/analysis/llvm/StableValueMapper.h`
- Create: `src/analysis/llvm/StableValueMapper.cpp`
- Create: `src/analysis/llvm/AbstractMemoryBuilder.h`
- Create: `src/analysis/llvm/AbstractMemoryBuilder.cpp`
- Modify: `src/analysis/llvm/LocalFactExtractor.h`
- Modify: `src/analysis/llvm/LocalFactExtractor.cpp`
- Modify: `src/analysis/llvm/CMakeLists.txt`
- Create: `tests/unit/analysis/llvm/StableValueMapperTest.cpp`
- Create: `tests/unit/analysis/llvm/AbstractMemoryBuilderTest.cpp`
- Create: `tests/unit/analysis/llvm/CMakeLists.txt`
- Modify: `tests/unit/analysis/CMakeLists.txt`
- Modify: `tests/fixtures/projects/field_access/field_access.cpp`

**Interfaces:**

- Consumes: live `ProgramIr`, `OriginMap`, LLVM `DataLayout`, debug/source anchors, and Task 1 semantic objects.
- Produces: private `StableValueMapper::IdFor(const llvm::Value&)` and `CallSiteIdFor(const llvm::CallBase&)`.
- Produces: private `AbstractMemoryBuilder::LocationFor(const llvm::Value& pointer, std::optional<uint64_t> access_size)`.
- Guarantees: two unnamed values or allocation sites in one function never collapse.
- Consumed by: local V2 extraction and SVF mapping in Tasks 8–9.

- [ ] **Step 1: Write identity and memory-shape tests**

```cpp
TEST(StableValueMapperTest, DistinguishesUnnamedInstructions) {
  auto module = ParseIr(R"(
    define void @f() {
      %1 = alloca i32
      %2 = alloca i32
      ret void
    })");
  StableValueMapper mapper(*module, OriginMapFor(*module));
  auto values = InstructionsNamedByOrdinal(*module->getFunction("f"));
  EXPECT_NE(*mapper.IdFor(*values[0]), *mapper.IdFor(*values[1]));
}

TEST(AbstractMemoryBuilderTest, PreservesFieldAndByteRange) {
  auto access = FieldStoreFixture();
  auto location = access.builder.LocationFor(*access.pointer, 4);
  ASSERT_TRUE(location.ok());
  EXPECT_EQ(location->access_path[0].kind,
            AccessPathSegment::Kind::kField);
  EXPECT_EQ(location->byte_range.size, 4u);
}

TEST(AbstractMemoryBuilderTest, RepresentsUnknownSuffixExplicitly) {
  auto access = VariableIndexFixture();
  auto location = access.builder.LocationFor(*access.pointer, 4);
  ASSERT_TRUE(location.ok());
  EXPECT_EQ(location->access_path.back().kind,
            AccessPathSegment::Kind::kUnknown);
}
```

- [ ] **Step 2: Build and confirm the tests fail**

Run:

```bash
cmake --build --preset default --target StableValueMapperTest AbstractMemoryBuilderTest
```

Expected: compilation fails because the private LLVM semantic mappers are absent.

- [ ] **Step 3: Implement deterministic value and memory normalization**

```cpp
class StableValueMapper {
 public:
  StableValueMapper(const llvm::Module& module, const OriginMap& origin_map);
  StatusOr<core::StableId> IdFor(const llvm::Value& value) const;
  StatusOr<core::StableId> CallSiteIdFor(const llvm::CallBase& call) const;
};

class AbstractMemoryBuilder {
 public:
  AbstractMemoryBuilder(const llvm::DataLayout& layout,
                        const StableValueMapper& values,
                        const OriginMap& origins);
  StatusOr<semantic::MemoryLocation>
  LocationFor(const llvm::Value& pointer,
              std::optional<std::uint64_t> access_size) const;
};
```

Value canonical bytes include owner function variant, value kind, stable source anchor when present, normalized opcode/type/operand fingerprint, and deterministic block-local structural index. Allocation-object bytes include the owner, allocation kind, allocation instruction fingerprint, and allocation size. Access paths use GEP source element types and `DataLayout`; missing information appends `kUnknown` and produces an unknown byte range.

Refactor `LocalFactExtractor` to produce `FunctionLocalFactsV2` using these mappers. Diagnostic LLVM names may be retained in protobuf diagnostic fields but are not used as value, call-site, object, or memory-location identity.

- [ ] **Step 4: Run identity, field-access, and local-extraction tests**

Run:

```bash
cmake --build --preset default --target StableValueMapperTest AbstractMemoryBuilderTest LocalAnalysisStageTest
./build/bin/StableValueMapperTest
./build/bin/AbstractMemoryBuilderTest
./build/bin/LocalAnalysisStageTest
```

Expected: distinct unnamed entities stay distinct, field/range cases are structured, and local extraction remains deterministic.

- [ ] **Step 5: Commit stable local semantic normalization**

```bash
git add src/analysis/llvm/StableValueMapper.h src/analysis/llvm/StableValueMapper.cpp src/analysis/llvm/AbstractMemoryBuilder.h src/analysis/llvm/AbstractMemoryBuilder.cpp src/analysis/llvm/LocalFactExtractor.h src/analysis/llvm/LocalFactExtractor.cpp src/analysis/llvm/CMakeLists.txt tests/unit/analysis/llvm/StableValueMapperTest.cpp tests/unit/analysis/llvm/AbstractMemoryBuilderTest.cpp tests/unit/analysis/llvm/CMakeLists.txt tests/unit/analysis/CMakeLists.txt tests/fixtures/projects/field_access/field_access.cpp
git commit -m "feat: normalize LLVM values and memory objects"
```

### Task 8: Normalize complete SVF aliases, memory targets, and indirect calls

**Files:**

- Create: `include/veritas/analysis/semantic/NormalizedAnalysisFacts.h`
- Modify: `src/analysis/svf/SvfFactMapper.h`
- Modify: `src/analysis/svf/SvfFactMapper.cpp`
- Modify: `src/analysis/svf/SvfAnalysisStage.h`
- Modify: `src/analysis/svf/SvfAnalysisStage.cpp`
- Modify: `src/analysis/svf/SvfMerge.h`
- Modify: `src/analysis/svf/SvfMerge.cpp`
- Modify: `src/analysis/svf/CMakeLists.txt`
- Modify: `tests/integration/analysis/svf/SvfFactMapperTest.cpp`
- Create: `tests/integration/analysis/svf/SvfIndirectCallTest.cpp`
- Create: `tests/integration/analysis/svf/SvfAliasKindsTest.cpp`
- Modify: `tests/integration/analysis/svf/CMakeLists.txt`

**Interfaces:**

- Replaces: temporary string-based structs nested in `SvfFactMapper.h`.
- Produces: `NormalizedValueFlow`, `NormalizedAlias`, `NormalizedMemoryEffect`, `NormalizedCallTarget`, `NormalizedUnknown`, and `NormalizedDependency` using stable VERITAS identity.
- Reads: `view.andersen->getCallGraph()->getIndCallMap()` for indirect and virtual candidates.
- Preserves: all four `AliasKind` values and independent epistemic state.
- Consumed by: V2 merge/pipeline Task 9.

- [ ] **Step 1: Write SVF boundary tests for calls and alias kinds**

```cpp
TEST(SvfIndirectCallTest, EmitsStableMayTargetForFunctionPointer) {
  auto result = AnalyzeFixture("function_pointer");
  ASSERT_TRUE(result.ok());
  auto targets = CallsAt(result->facts, "invoke-callback");
  ASSERT_FALSE(targets.empty());
  EXPECT_TRUE(std::ranges::all_of(targets, [](const auto& target) {
    return target.callee.has_value() &&
           target.dispatch == semantic::DispatchKind::kIndirect &&
           target.epistemic == semantic::EpistemicState::kMay;
  }));
}

TEST(SvfAliasKindsTest, KeepsNoAliasAsSemanticNoAliasMust) {
  auto result = AnalyzeDisjointAllocas();
  ASSERT_TRUE(result.ok());
  auto observation = FindAlias(result->facts, "left", "right");
  ASSERT_TRUE(observation.has_value());
  EXPECT_EQ(observation->kind, semantic::AliasKind::kNoAlias);
  EXPECT_EQ(observation->epistemic, semantic::EpistemicState::kMust);
}
```

- [ ] **Step 2: Build and run to expose the current missing outputs**

Run:

```bash
cmake --build --preset default --target SvfIndirectCallTest SvfAliasKindsTest SvfFactMapperTest
./build/bin/SvfIndirectCallTest
./build/bin/SvfAliasKindsTest
```

Expected: tests fail because `refined_calls` and refined memory effects are empty and aliases encode their kind as strings/epistemic state.

- [ ] **Step 3: Implement the normalized SVF output boundary**

```cpp
struct NormalizedCallTarget {
  core::StableId call_site;
  core::StableId caller;
  std::optional<core::StableId> callee;
  semantic::DispatchKind dispatch;
  semantic::EpistemicState epistemic;
  std::string diagnostic_symbol;
  std::string provenance_ref;
};

struct NormalizedAlias {
  semantic::MemoryLocation left;
  semantic::MemoryLocation right;
  semantic::AliasKind kind;
  semantic::EpistemicState epistemic;
  std::string provenance_ref;
};

struct NormalizedAnalysisFacts {
  std::vector<NormalizedValueFlow> value_flows;
  std::vector<NormalizedAlias> aliases;
  std::vector<NormalizedMemoryEffect> memory_effects;
  std::vector<NormalizedCallTarget> calls;
  std::vector<NormalizedUnknown> unknowns;
  std::vector<NormalizedDependency> dependencies;
};
```

Iterate the Andersen indirect-call map in stable call-site/target order. Resolve `CallICFGNode` and `FunObjVar` through `SVFModule`/LLVM and Task 7 mappers. Emit one MAY target per candidate; emit an explicit unknown call when the candidate set is empty or any call site cannot be mapped. Query all admitted alias candidates, retaining `MustAlias`, `MayAlias`/`PartialAlias`, `NoAlias`, and unexpected results as distinct semantic values. Map points-to objects and SVFG memory edges into structured `MemoryLocation` values; budget truncation adds a scoped unknown rather than discarding prior facts.

- [ ] **Step 4: Run the complete SVF boundary suite**

Run:

```bash
cmake --build --preset default --target SvfIndirectCallTest SvfAliasKindsTest SvfFactMapperTest SvfSessionTest SvfTruncationTest RepeatedProjectAnalysisTest
./build/bin/SvfIndirectCallTest
./build/bin/SvfAliasKindsTest
./build/bin/SvfFactMapperTest
./build/bin/SvfSessionTest
./build/bin/SvfTruncationTest
./build/bin/RepeatedProjectAnalysisTest
```

Expected: indirect targets, all alias semantics, truncation unknowns, and repeated-process cleanup pass.

- [ ] **Step 5: Commit the normalized SVF boundary**

```bash
git add include/veritas/analysis/semantic/NormalizedAnalysisFacts.h src/analysis/svf/SvfFactMapper.h src/analysis/svf/SvfFactMapper.cpp src/analysis/svf/SvfAnalysisStage.h src/analysis/svf/SvfAnalysisStage.cpp src/analysis/svf/SvfMerge.h src/analysis/svf/SvfMerge.cpp src/analysis/svf/CMakeLists.txt tests/integration/analysis/svf/SvfFactMapperTest.cpp tests/integration/analysis/svf/SvfIndirectCallTest.cpp tests/integration/analysis/svf/SvfAliasKindsTest.cpp tests/integration/analysis/svf/CMakeLists.txt
git commit -m "feat: normalize SVF calls aliases and memory"
```

### Task 9: Add versioned external models and publish native V2 summaries

**Files:**

- Create: `include/veritas/analysis/semantic/ModelBundle.h`
- Create: `src/analysis/semantic/ModelBundle.cpp`
- Create: `logic/models/models.v1.tsv`
- Create: `logic/models/models.v1.manifest`
- Create: `tests/unit/analysis/ModelBundleTest.cpp`
- Modify: `src/analysis/svf/SvfMerge.h`
- Modify: `src/analysis/svf/SvfMerge.cpp`
- Modify: `src/analysis/pipeline/LocalAnalysisStage.h`
- Modify: `src/analysis/pipeline/LocalAnalysisStage.cpp`
- Modify: `src/analysis/cpg/CpgProjectionStage.h`
- Modify: `src/analysis/cpg/CpgProjectionStage.cpp`
- Modify: `src/analysis/ProjectPublicationCoordinator.h`
- Modify: `src/analysis/ProjectPublicationCoordinator.cpp`
- Modify: `include/veritas/analysis/ProjectAnalyzer.h`
- Modify: `src/analysis/ProjectAnalyzer.cpp`
- Modify: `src/analysis/CMakeLists.txt`
- Modify: `tests/integration/analysis/LocalAnalysisStageTest.cpp`
- Modify: `tests/integration/analysis/ProjectAnalyzerTest.cpp`
- Create: `tests/integration/analysis/SummaryV2PipelineTest.cpp`
- Modify: `tests/integration/analysis/CMakeLists.txt`

**Interfaces:**

- Produces: `ModelBundle::Load(path, manifest_path)`, `version()`, `hash()`, and `Lookup(symbol)`.
- Produces: `MergeSvfFactsV2(drafts, normalized_facts, model_bundle)`.
- Changes: `LocalAnalysisResult::summary_drafts` and `CompletedProjectAnalysis::summaries` to `std::vector<summary::v2::FunctionSummary>`.
- Adds: `revision_id` and `build_variant_id` to `ProjectAnalysisResult` so version-aware readers use the exact published context.
- Adapts: CPG projection through `SummaryArtifact`/V2 fields without reverting memory identity to strings.
- Produces: standard project analysis publishing native `summary.v2`.

- [ ] **Step 1: Write model and end-to-end V2 publication tests**

```cpp
TEST(ModelBundleTest, VersionAndContentDetermineBundleHash) {
  auto bundle = ModelBundle::Load(ModelPath(), ManifestPath());
  ASSERT_TRUE(bundle.ok());
  EXPECT_EQ(bundle->version(), "models.v1");
  EXPECT_EQ(bundle->hash().size(), 64u);
  EXPECT_FALSE(bundle->Lookup("malloc").empty());
}

TEST(SummaryV2PipelineTest, PublishesIndirectTargetsAndStructuredMemory) {
  auto result = AnalyzeProjectFixture("function_pointer");
  ASSERT_TRUE(result.ok()) << result.status().message();
  auto repository = OpenFixtureRepository("function_pointer");
  auto summaries = repository->ListCurrentSummaryArtifacts(
      result->revision_id, result->build_variant_id);
  ASSERT_TRUE(summaries.ok());
  EXPECT_TRUE(std::ranges::all_of(*summaries, [](const auto& artifact) {
    return std::holds_alternative<summary::v2::FunctionSummary>(artifact);
  }));
  EXPECT_TRUE(ContainsIndirectMayTarget(*summaries));
}
```

- [ ] **Step 2: Build and confirm the new pipeline test fails**

Run:

```bash
cmake --build --preset default --target ModelBundleTest SummaryV2PipelineTest ProjectAnalyzerTest
```

Expected: build fails because the model bundle and V2 pipeline signatures are absent.

- [ ] **Step 3: Implement versioned models and V2 pipeline merge**

```cpp
struct FunctionModel {
  core::StableId model_id;
  std::string symbol;
  ModelEffectKind effect;
  std::string subject;
  semantic::EpistemicState epistemic;
};

class ModelBundle {
 public:
  static StatusOr<ModelBundle> Load(const std::filesystem::path& rows,
                                    const std::filesystem::path& manifest);
  std::string_view version() const;
  std::string_view hash() const;
  std::span<const FunctionModel> Lookup(std::string_view symbol) const;
};
```

The TSV has exactly five tab-separated fields: model ID seed, exact symbol, effect kind, subject, epistemic. The manifest has one line `model_bundle_version=models.v1`. Reject duplicate model IDs, unknown enum strings, control characters, and unsorted rows. Compute the content hash from the version plus canonical TSV bytes and place that hash in every WPA `LogicalInputHash`.

Merge local V2 facts, normalized SVF facts, models, unknowns, and dependencies by stable owning function ID. Recompute deterministic component digests after the merge. Update CPG projection to read V2 stable IDs and semantic alias kinds. Publish summaries and CPG atomically through version-neutral SummaryDB APIs.

- [ ] **Step 4: Run M8R.2 integration and regression suites**

Run:

```bash
cmake --build --preset default --target ModelBundleTest SummaryV2PipelineTest ProjectAnalyzerTest CpgProjectionStageTest CpgEndToEndTest SummaryVersionCompatibilityTest
./build/bin/ModelBundleTest
./build/bin/SummaryV2PipelineTest
./build/bin/ProjectAnalyzerTest
./build/bin/CpgProjectionStageTest
./build/bin/CpgEndToEndTest
./build/bin/SummaryVersionCompatibilityTest
```

Expected: standard analysis publishes V2, CPG consumes it, V1 history remains readable, and function-pointer targets enter summaries as stable MAY calls.

- [ ] **Step 5: Commit M8R.2**

```bash
git add include/veritas/analysis/semantic/ModelBundle.h src/analysis/semantic/ModelBundle.cpp logic/models/models.v1.tsv logic/models/models.v1.manifest tests/unit/analysis/ModelBundleTest.cpp src/analysis/svf/SvfMerge.h src/analysis/svf/SvfMerge.cpp src/analysis/pipeline/LocalAnalysisStage.h src/analysis/pipeline/LocalAnalysisStage.cpp src/analysis/cpg/CpgProjectionStage.h src/analysis/cpg/CpgProjectionStage.cpp src/analysis/ProjectPublicationCoordinator.h src/analysis/ProjectPublicationCoordinator.cpp include/veritas/analysis/ProjectAnalyzer.h src/analysis/ProjectAnalyzer.cpp src/analysis/CMakeLists.txt tests/integration/analysis/LocalAnalysisStageTest.cpp tests/integration/analysis/ProjectAnalyzerTest.cpp tests/integration/analysis/SummaryV2PipelineTest.cpp tests/integration/analysis/CMakeLists.txt
git commit -m "feat: publish modeled Function Summary IR v2"
```

---

## M8R.3 — Relational WPA Projection

### Task 10: Materialize typed per-SCC component inputs

**Files:**

- Create: `include/veritas/wpa/WpaComponent.h`
- Create: `include/veritas/wpa/WpaInputMaterializer.h`
- Create: `src/wpa/WpaInputMaterializer.cpp`
- Modify: `include/veritas/wpa/CallGraph.h`
- Modify: `src/wpa/CallGraph.cpp`
- Modify: `include/veritas/wpa/SccGraph.h`
- Modify: `src/wpa/SccGraph.cpp`
- Modify: `include/veritas/facts/RelationSchema.h`
- Modify: `src/facts/RelationSchema.cpp`
- Modify: `logic/schema/relations.v2.json`
- Modify: `src/wpa/CMakeLists.txt`
- Create: `tests/unit/wpa/WpaInputMaterializerTest.cpp`
- Create: `tests/unit/wpa/CallGraphTest.cpp`
- Modify: `tests/unit/wpa/SccGraphTest.cpp`
- Modify: `tests/unit/wpa/CMakeLists.txt`

**Interfaces:**

- Consumes: the semantic fields of `AnalysisRunManifest`, `SummaryArtifact`, `CallGraph`, `SccGraph`, V2 relations, dense maps, model rows, and successor `AnalysisFact` support.
- Produces: `WpaComponentKind`, `StableIdMappings`, `RootedInputFact`, and immutable engine-neutral `WpaLogicalComponentInput`.
- Produces: `WpaInputMaterializer::Build(request)` with deterministic stable-to-dense maps and `LogicalInputHash`.
- Extends: `CallGraph::FromSummaries` to accept V2 indirect/MAY targets and tagged V1 compatibility artifacts.
- Consumed by: both evaluators and orchestration Tasks 12–15.

- [ ] **Step 1: Write per-SCC materialization tests**

```cpp
TEST(WpaInputMaterializerTest, EmitsIndirectCallAndExplicitUnknownCall) {
  auto input = Materialize(CallSummaryWithMayTargetAndUnknown(),
                           WpaComponentKind::kReachability);
  ASSERT_TRUE(input.ok());
  EXPECT_EQ(CountRows(input->edb, RelationId::kDirectCall), 1u);
  EXPECT_EQ(CountRows(input->edb, RelationId::kUnknownCall), 1u);
  const auto& call = FirstRow(input->edb, RelationId::kDirectCall);
  EXPECT_EQ(DispatchCell(call), semantic::DispatchKind::kIndirect);
  EXPECT_EQ(EpistemicCell(call), semantic::EpistemicState::kMay);
}

TEST(WpaInputMaterializerTest, DenseIdsRoundTripToStableIds) {
  auto input = Materialize(TwoFunctionSummary(),
                           WpaComponentKind::kReachability);
  ASSERT_TRUE(input.ok());
  for (const auto& stable : input->mappings.functions.StableIds()) {
    EXPECT_EQ(*input->mappings.functions.ToStable(
                  *input->mappings.functions.ToDense(stable)),
              stable);
  }
}

TEST(WpaInputMaterializerTest, SuccessorFactsAreExplicitSupportRows) {
  auto input = MaterializeWithSuccessorSupport(ReachableFact("g", "h"));
  ASSERT_TRUE(input.ok());
  EXPECT_EQ(CountRows(input->edb, RelationId::kSupportReachableCall), 1u);
  EXPECT_EQ(input->successor_roots.size(), 1u);
}
```

- [ ] **Step 2: Build and confirm materialization tests fail**

Run:

```bash
cmake --build --preset default --target WpaInputMaterializerTest CallGraphTest SccGraphTest
```

Expected: compilation fails because the component contract and materializer are absent.

- [ ] **Step 3: Implement the component-input boundary**

```cpp
enum class WpaComponentKind : std::uint8_t {
  kReachability,
  kMemoryEffects,
};

struct StableIdMappings {
  facts::FunctionDenseMap functions;
  facts::ValueDenseMap values;
  facts::MemoryDenseMap memories;
  facts::CallSiteDenseMap call_sites;
  facts::DenseIdMap<facts::FactId, core::IdKind::kFact> facts;
};

struct RootedInputFact {
  facts::AnalysisFact fact;
  std::string provenance_ref;
};

struct WpaLogicalComponentInput {
  core::StableId scc_id;
  WpaComponentKind component;
  StableIdMappings mappings;
  std::vector<facts::ExecutionRow> edb;
  std::vector<RootedInputFact> local_roots;
  std::vector<RootedInputFact> successor_roots;
  std::string logical_input_hash;
};

struct WpaMaterializationRequest {
  facts::AnalysisRunSemanticDescriptor semantics;
  core::StableId scc_id;
  WpaComponentKind component;
  std::span<const summary::SummaryArtifact> summaries;
  std::span<const facts::AnalysisFact> successor_support;
  const analysis::semantic::ModelBundle& models;
};
```

Add `SupportReachableCall` and `SupportMayWrite` to the registry as EDB-only relations. Emit `FunctionMap`, `ValueMap`, `MemoryMap`, `CallSiteMap`, and `FactMap` execution relations from the dual-identity maps so evaluators can reconstruct semantic keys without leaking dense IDs. Collect stable identities first, sort and build each dense map, then convert semantic rows to execution rows. Compute `LogicalInputHash` from the relation, rule, model, and semantic configuration versions; component; SCC members; canonical stable semantic EDB; canonical mappings; model hash; and successor external fact IDs. Exclude revision, `RunId`, engine identity, dense-number assignment accidents, and iteration order. Serialize this engine-neutral object once; Task 13 wraps the same bytes in executor-specific manifests.

`CallGraph::FromSummaries` accepts `SummaryArtifact` values. Native V2 calls with stable resolved targets and epistemic `MUST`, `MAY`, `INFERRED`, or `ASSUMED` become edges according to bundle policy. Empty/unknown targets remain scoped unknown-call effects and never become all-functions fanout.

- [ ] **Step 4: Run materialization, call-graph, and SCC tests**

Run:

```bash
cmake --build --preset default --target WpaInputMaterializerTest CallGraphTest SccGraphTest
./build/bin/WpaInputMaterializerTest
./build/bin/CallGraphTest
./build/bin/SccGraphTest
```

Expected: stable/dense round trips, V2 call semantics, successor support, input hashes, and deterministic SCC order pass.

- [ ] **Step 5: Commit the per-SCC relational projection**

```bash
git add include/veritas/wpa/WpaComponent.h include/veritas/wpa/WpaInputMaterializer.h src/wpa/WpaInputMaterializer.cpp include/veritas/wpa/CallGraph.h src/wpa/CallGraph.cpp include/veritas/wpa/SccGraph.h src/wpa/SccGraph.cpp include/veritas/facts/RelationSchema.h src/facts/RelationSchema.cpp logic/schema/relations.v2.json src/wpa/CMakeLists.txt tests/unit/wpa/WpaInputMaterializerTest.cpp tests/unit/wpa/CallGraphTest.cpp tests/unit/wpa/SccGraphTest.cpp tests/unit/wpa/CMakeLists.txt
git commit -m "feat: materialize typed SCC WPA inputs"
```

### Task 11: Add generic witness validation and deterministic proof selection

**Files:**

- Create: `include/veritas/facts/Witness.h`
- Create: `include/veritas/facts/RuleRegistry.h`
- Create: `include/veritas/facts/ResultCanonicalizer.h`
- Create: `src/facts/Witness.cpp`
- Create: `src/facts/RuleRegistry.cpp`
- Create: `src/facts/ResultCanonicalizer.cpp`
- Create: `logic/common/rules.v2.manifest`
- Modify: `include/veritas/wpa/WpaComponent.h`
- Modify: `src/facts/CMakeLists.txt`
- Create: `tests/unit/facts/ResultCanonicalizerTest.cpp`
- Create: `tests/unit/facts/WitnessTest.cpp`
- Modify: `tests/unit/facts/CMakeLists.txt`

**Interfaces:**

- Consumes: stable semantic rows, rooted local/support facts, and raw evaluator witness candidates.
- Produces: `SemanticKey`, `WitnessEdge`, `RawWpaEvaluation`, `SelectedWitness`, and `WpaComponentResult`.
- Produces: `ResultCanonicalizer::Canonicalize(input, raw_evaluation)`.
- Selects proofs by: fewest derived edges, lower versioned rule priority, then lexicographic stable input keys.
- Replaces: relation-specific proof reconstruction in `SouffleExporter` after Task 12 switches callers.

- [ ] **Step 1: Write witness closure and selection tests**

```cpp
TEST(ResultCanonicalizerTest, RejectsOrphanedDerivedResult) {
  RawWpaEvaluation raw{.results = {ReachableSemantic("f", "g")},
                       .witnesses = {}};
  EXPECT_EQ(Canonicalize(InputWithDirectCall(), raw).status().code(),
            StatusCode::kFailedPrecondition);
}

TEST(ResultCanonicalizerTest, RejectsCyclicUnrootedWitnesses) {
  RawWpaEvaluation raw = TwoResultCycleWithoutRoot();
  EXPECT_EQ(Canonicalize(EmptyInput(), raw).status().code(),
            StatusCode::kFailedPrecondition);
}

TEST(ResultCanonicalizerTest, SelectsShortestProofDeterministically) {
  auto forward = Canonicalize(Input(), AlternativesInForwardOrder());
  auto reverse = Canonicalize(Input(), AlternativesInReverseOrder());
  ASSERT_TRUE(forward.ok());
  ASSERT_TRUE(reverse.ok());
  EXPECT_EQ(forward->facts, reverse->facts);
  EXPECT_EQ(forward->witnesses, reverse->witnesses);
  EXPECT_EQ(forward->fixpoint_hash, reverse->fixpoint_hash);
}
```

- [ ] **Step 2: Build and confirm witness tests fail**

Run:

```bash
cmake --build --preset default --target ResultCanonicalizerTest WitnessTest
```

Expected: compilation fails because witness and canonicalizer interfaces do not exist.

- [ ] **Step 3: Implement semantic keys and generic canonicalization**

```cpp
struct SemanticKey {
  facts::SemanticRow row;
  auto operator<=>(const SemanticKey&) const = default;
};

struct WitnessEdge {
  SemanticKey result;
  std::string rule_id;
  SemanticKey input;
  std::uint32_t input_ordinal;
  auto operator<=>(const WitnessEdge&) const = default;
};

struct RawWpaEvaluation {
  std::vector<facts::SemanticRow> results;
  std::vector<WitnessEdge> witnesses;
  std::vector<std::string> diagnostics;
};

struct WpaComponentResult {
  core::StableId scc_id;
  WpaComponentKind component;
  std::string logical_input_hash;
  std::string fixpoint_hash;
  std::string external_hash;
  std::vector<facts::AnalysisFact> facts;
  std::vector<WitnessEdge> witnesses;
  std::vector<std::string> diagnostics;
};
```

The manifest defines exact rule IDs and integer priorities. The canonicalizer validates result and input schemas, rejects duplicate edges with conflicting ordinals, creates root nodes from `local_roots` and `successor_roots`, computes shortest rooted proof candidates without recursion, rejects unrooted/cyclic results, materializes stable `AnalysisFact` values, and hashes canonical facts plus selected witness edges for `fixpoint_hash`. `external_hash` contains only predecessor-visible semantic facts and excludes witness edges.

- [ ] **Step 4: Run canonicalizer and existing fact tests**

Run:

```bash
cmake --build --preset default --target ResultCanonicalizerTest WitnessTest AnalysisFactTest FactSchemaTest
./build/bin/ResultCanonicalizerTest
./build/bin/WitnessTest
./build/bin/AnalysisFactTest
./build/bin/FactSchemaTest
```

Expected: witness closure/selection is deterministic and existing V1 fact validation remains green.

- [ ] **Step 5: Commit generic witness canonicalization**

```bash
git add include/veritas/facts/Witness.h include/veritas/facts/RuleRegistry.h include/veritas/facts/ResultCanonicalizer.h src/facts/Witness.cpp src/facts/RuleRegistry.cpp src/facts/ResultCanonicalizer.cpp logic/common/rules.v2.manifest include/veritas/wpa/WpaComponent.h src/facts/CMakeLists.txt tests/unit/facts/ResultCanonicalizerTest.cpp tests/unit/facts/WitnessTest.cpp tests/unit/facts/CMakeLists.txt
git commit -m "feat: canonicalize generic WPA witnesses"
```

### Task 12: Implement V2 rule bundles and the matched C++ logical evaluator

**Files:**

- Create: `logic/schema/relations.v2.dl`
- Create: `logic/common/epistemic.dl`
- Create: `logic/common/semantic_key.dl`
- Create: `logic/reachability/reachability.v2.dl`
- Create: `logic/reachability/bundle.manifest`
- Create: `logic/memory_effects/may_write.v2.dl`
- Create: `logic/memory_effects/bundle.manifest`
- Create: `include/veritas/facts/RelationIo.h`
- Create: `src/facts/RelationIo.cpp`
- Create: `include/veritas/facts/SemanticKeyCodec.h`
- Create: `src/facts/SemanticKeyCodec.cpp`
- Create: `include/veritas/wpa/CppRuleEvaluator.h`
- Create: `src/wpa/CppRuleEvaluator.cpp`
- Modify: `include/veritas/facts/SouffleExporter.h`
- Modify: `src/facts/SouffleExporter.cpp`
- Modify: `include/veritas/wpa/FixpointEngine.h`
- Modify: `src/wpa/FixpointEngine.cpp`
- Modify: `src/facts/CMakeLists.txt`
- Modify: `src/wpa/CMakeLists.txt`
- Create: `tests/unit/wpa/CppRuleEvaluatorTest.cpp`
- Create: `tests/unit/facts/RelationIoTest.cpp`
- Create: `tests/unit/facts/SemanticKeyCodecTest.cpp`
- Modify: `tests/unit/facts/SouffleExporterTest.cpp`
- Modify: `tests/unit/wpa/FixpointEngineTest.cpp`
- Modify: `tests/unit/facts/CMakeLists.txt`
- Modify: `tests/unit/wpa/CMakeLists.txt`
- Modify: `tests/integration/facts/SouffleRunnerTest.cpp`

**Interfaces:**

- Consumes: exactly one engine-neutral `WpaLogicalComponentInput` relation set.
- Produces: `CppRuleEvaluator::Evaluate(input) -> RawWpaEvaluation`.
- Produces: typed relation I/O for all V2 EDB, result, and witness relations.
- Keeps: `FixpointEngine` as a temporary compatibility wrapper that materializes input, invokes `CppRuleEvaluator`, and calls the generic canonicalizer.
- Removes from active path: `SouffleExporter::ReconstructCanonicalProofs` and hard-coded M8 join reconstruction.
- Consumed by: production executor/oracle Task 13.

- [ ] **Step 1: Write golden recursive-domain tests**

```cpp
TEST(CppRuleEvaluatorTest, DerivesReachabilityAndImmediateWitnesses) {
  auto raw = CppRuleEvaluator().Evaluate(RecursiveReachabilityInput());
  ASSERT_TRUE(raw.ok());
  EXPECT_TRUE(ContainsSemantic(raw->results, Reachable("f", "h")));
  EXPECT_TRUE(ContainsWitness(*raw, Reachable("f", "h"),
                              "wpa.reachability.transitive.v2", 0));
  EXPECT_TRUE(ContainsWitness(*raw, Reachable("f", "h"),
                              "wpa.reachability.transitive.v2", 1));
}

TEST(CppRuleEvaluatorTest, PropagatesMayWriteFromSuccessorSupport) {
  auto raw = CppRuleEvaluator().Evaluate(MayWriteSuccessorInput());
  ASSERT_TRUE(raw.ok());
  EXPECT_TRUE(ContainsSemantic(raw->results, MayWrite("f", "heap")));
}

TEST(RelationIoTest, RejectsWrongColumnDomainAndUnexpectedOutput) {
  EXPECT_FALSE(ReadRawEvaluation(DirectoryWithMemoryIdInFunctionColumn()).ok());
  EXPECT_FALSE(ReadRawEvaluation(DirectoryWithUnknownRelation()).ok());
}

TEST(SemanticKeyCodecTest, AdversarialFieldsRemainInjective) {
  EXPECT_NE(EncodeFields({"a", "bc"}), EncodeFields({"ab", "c"}));
  EXPECT_NE(EncodeFields({"", ":1:|"}), EncodeFields({":1:", "|"}));
  EXPECT_EQ(DecodeFields(EncodeFields({"λ", "01", ""})),
            (std::vector<std::string>{"λ", "01", ""}));
}
```

- [ ] **Step 2: Build and confirm golden tests fail**

Run:

```bash
cmake --build --preset default --target CppRuleEvaluatorTest RelationIoTest SemanticKeyCodecTest FixpointEngineTest SouffleExporterTest
```

Expected: compilation fails because V2 rule evaluation and typed relation I/O are absent.

- [ ] **Step 3: Add V2 Datalog rules with generic witness output**

```souffle
.include "../schema/relations.v2.dl"
.include "../common/epistemic.dl"
.include "../common/semantic_key.dl"

.decl ReachableCall(source:FunctionId, target:FunctionId,
                    epistemic:Epistemic)
.decl Witness(result_key:symbol, rule_id:symbol,
              input_key:symbol, input_ordinal:unsigned)
.output ReachableCall(IO=csv, filename="ReachableCall.csv")
.output Witness(IO=csv, filename="Witness.csv")

ReachableCall(f, g, e) :- DirectCall(_, f, g, _, e).
ReachableCall(f, h, e) :-
  DirectCall(_, f, g, _, e1), ReachableCall(g, h, e2),
  WeakenEpistemic(e1, e2, e).
ReachableCall(f, h, e) :-
  DirectCall(_, f, g, _, e1), SupportReachableCall(g, h, e2),
  WeakenEpistemic(e1, e2, e).
```

`semantic_key.dl` declares the pinned Soufflé 2.5 stateful functor ABI:

```souffle
.functor veritas_key_field_symbol_v1(value:symbol):symbol stateful
.functor veritas_key_field_number_v1(value:number):symbol stateful
.functor veritas_key_field_unsigned_v1(value:unsigned):symbol stateful
```

`SouffleSemanticKeyFunctor.cpp` exports those exact C-linkage names with signature `souffle::RamDomain name(souffle::SymbolTable*, souffle::RecordTable*, souffle::RamDomain)`. Each adapter decodes its typed argument, calls `SemanticKeyCodec` to emit one type-tagged length-prefixed UTF-8 field, and interns the result through the supplied symbol table. The implementations are pure and reentrant. Nested `cat` may join only these self-delimiting encoded fields after an encoded `veritas.semantic-key.v1` prefix and relation name; concatenating raw cells or delimiters is forbidden.

`epistemic.dl` defines the finite `WeakenEpistemic(left, right, result)` relation for every state admitted by the bundle contract. Add corresponding immediate `Witness` rules for every direct, local-transitive, and successor-support derivation. `SemanticKeyCodecTest` covers empty cells, delimiter-like text, Unicode, digit-prefixed symbols, signed numbers, and unsigned bounds without requiring Soufflé. Task 13 adds the stateful adapter and mandatory generated-program comparison. The may-write bundle follows the same pattern with `DirectWrite`, `DirectCall`, `SupportMayWrite`, and `MayWrite`.

- [ ] **Step 4: Implement the C++ logical evaluator and typed I/O**

```cpp
class CppRuleEvaluator {
 public:
  StatusOr<RawWpaEvaluation>
  Evaluate(const WpaLogicalComponentInput& input) const;
};

class RelationIo {
 public:
  static Status WriteInput(const std::filesystem::path& directory,
                           const WpaLogicalComponentInput& input);
  static StatusOr<RawWpaEvaluation>
  ReadOutput(const std::filesystem::path& directory,
             const WpaLogicalComponentInput& input);
};
```

Implement the same direct/transitive/support joins and epistemic weakening as the rule bundles. Emit semantic rows and generic witness candidates only; do not compute stable fact IDs in the evaluator. Convert dense output cells back through the input maps and fail on missing mappings, duplicate dense IDs, schema drift, or output for another SCC.

- [ ] **Step 5: Run C++ goldens, existing M8 regressions, and optional file-Soufflé comparison**

Run:

```bash
cmake --build --preset default --target CppRuleEvaluatorTest RelationIoTest SemanticKeyCodecTest FixpointEngineTest SouffleExporterTest SouffleRunnerTest
./build/bin/CppRuleEvaluatorTest
./build/bin/RelationIoTest
./build/bin/SemanticKeyCodecTest
./build/bin/FixpointEngineTest
./build/bin/SouffleExporterTest
./build/bin/SouffleRunnerTest
```

Expected: C++ and existing M8 semantic goldens pass. `SouffleRunnerTest` runs rule comparison when Soufflé is present and reports a skip only in this transitional M8R.3 task when it is absent.

- [ ] **Step 6: Commit M8R.3**

```bash
git add logic/schema/relations.v2.dl logic/common/epistemic.dl logic/common/semantic_key.dl logic/reachability/reachability.v2.dl logic/reachability/bundle.manifest logic/memory_effects/may_write.v2.dl logic/memory_effects/bundle.manifest include/veritas/facts/RelationIo.h src/facts/RelationIo.cpp include/veritas/facts/SemanticKeyCodec.h src/facts/SemanticKeyCodec.cpp include/veritas/wpa/CppRuleEvaluator.h src/wpa/CppRuleEvaluator.cpp include/veritas/facts/SouffleExporter.h src/facts/SouffleExporter.cpp include/veritas/wpa/FixpointEngine.h src/wpa/FixpointEngine.cpp src/facts/CMakeLists.txt src/wpa/CMakeLists.txt tests/unit/wpa/CppRuleEvaluatorTest.cpp tests/unit/facts/RelationIoTest.cpp tests/unit/facts/SemanticKeyCodecTest.cpp tests/unit/facts/SouffleExporterTest.cpp tests/unit/wpa/FixpointEngineTest.cpp tests/unit/facts/CMakeLists.txt tests/unit/wpa/CMakeLists.txt tests/integration/facts/SouffleRunnerTest.cpp
git commit -m "feat: add V2 WPA rule bundles and oracle"
```

---

## M8R.4 — Production Soufflé WPA

### Task 13: Compile Soufflé bundles and add matched executor adapters

**Files:**

- Create: `cmake/VeritasSouffle.cmake`
- Modify: `CMakeLists.txt`
- Modify: `cmake/Dependencies.cmake`
- Create: `include/veritas/wpa/WpaExecutor.h`
- Create: `include/veritas/wpa/SouffleWpaExecutor.h`
- Create: `include/veritas/wpa/CppConformanceExecutor.h`
- Create: `src/wpa/SouffleWpaExecutor.cpp`
- Create: `src/wpa/CppConformanceExecutor.cpp`
- Create: `src/wpa/SouffleWorkerMain.cpp`
- Create: `src/facts/SouffleSemanticKeyFunctor.cpp`
- Modify: `src/facts/CMakeLists.txt`
- Modify: `src/wpa/CMakeLists.txt`
- Create: `tests/unit/wpa/WpaExecutorTest.cpp`
- Create: `tests/integration/wpa/SouffleWpaExecutorTest.cpp`
- Create: `tests/integration/wpa/WpaExecutorConformanceTest.cpp`
- Create: `tests/integration/facts/SemanticKeyFunctorTest.cpp`
- Modify: `tests/unit/wpa/CMakeLists.txt`
- Modify: `tests/integration/wpa/CMakeLists.txt`

**Interfaces:**

- Produces: `WpaExecutionLimits`, abstract `WpaExecutor`, `SouffleWpaExecutor`, and `CppConformanceExecutor`.
- Produces: generated compiled rule programs registered in a private `veritas-souffle-worker` executable.
- Returns: `RawWpaEvaluation` from both engines; canonicalization remains VERITAS-owned.
- Replaces build policy: optional interpreted Soufflé with default `VERITAS_WPA_ENGINE=souffle`; `cpp-emergency` is the only Soufflé-less configuration.
- Does not expose: Soufflé headers or generated types under `include/veritas/**`.

- [ ] **Step 1: Write executor identity, timeout, and conformance tests**

```cpp
TEST(WpaExecutorTest, CppExecutorRequiresNonProductionIdentity) {
  EXPECT_FALSE(CppConformanceExecutor::Create(EngineIdentity::kSouffle).ok());
  EXPECT_TRUE(
      CppConformanceExecutor::Create(EngineIdentity::kCppConformance).ok());
  EXPECT_TRUE(CppConformanceExecutor::Create(
                  EngineIdentity::kCppEmergency).ok());
}

TEST(SouffleWpaExecutorTest, TimeoutReturnsFailureWithoutOutput) {
  SouffleWpaExecutor executor(PathToWorker());
  auto result = executor.Execute(LargeRecursiveInput(),
                                 WpaExecutionLimits{.timeout = 1ms,
                                                    .memory_mb = 64,
                                                    .threads = 1});
  EXPECT_EQ(result.status().code(), StatusCode::kDeadlineExceeded);
  EXPECT_FALSE(OutputWasPublished());
}

TEST(WpaExecutorConformanceTest, EnginesProduceSameCanonicalFacts) {
  auto logical = RecursiveReachabilityLogicalInput();
  const auto logical_bytes = SerializeCanonical(*logical);
  auto souffle_input = EnvelopeFor(*logical, SouffleManifest());
  auto cpp_input = EnvelopeFor(*logical, CppConformanceManifest());
  ASSERT_NE(souffle_input.run.run_id, cpp_input.run.run_id);
  EXPECT_EQ(SerializeCanonical(souffle_input.logical), logical_bytes);
  EXPECT_EQ(SerializeCanonical(cpp_input.logical), logical_bytes);
  EXPECT_EQ(souffle_input.logical.mappings, cpp_input.logical.mappings);
  auto souffle = RunAndCanonicalize(SouffleExecutor(), souffle_input);
  auto cpp = RunAndCanonicalize(CppOracle(), cpp_input);
  ASSERT_TRUE(souffle.ok());
  ASSERT_TRUE(cpp.ok());
  EXPECT_EQ(souffle->facts, cpp->facts);
  EXPECT_EQ(souffle->external_hash, cpp->external_hash);
}
```

- [ ] **Step 2: Confirm the production configuration fails before integration**

Run:

```bash
cmake --preset default -DLLVM_PROJECT_BUILD_DIR=/Users/skg7on/Workspace/Projects/llvm-project/build -DVERITAS_WPA_ENGINE=souffle -DVERITAS_SOUFFLE_PROVENANCE_FILE=/path/to/souffle-provenance.json
cmake --build --preset default --target SouffleWpaExecutorTest WpaExecutorConformanceTest SemanticKeyFunctorTest
```

Expected before implementation: configure/build fails because compiled bundle generation and executor targets do not exist. If the host lacks Soufflé, configuration also confirms the new production dependency must be installed before completing this task.

- [ ] **Step 3: Implement required build-time Soufflé generation**

```cmake
set(VERITAS_WPA_ENGINE "souffle" CACHE STRING
    "Recursive WPA engine: souffle or cpp-emergency")
set_property(CACHE VERITAS_WPA_ENGINE PROPERTY STRINGS souffle cpp-emergency)

if(VERITAS_WPA_ENGINE STREQUAL "souffle")
  find_program(VERITAS_SOUFFLE_EXECUTABLE NAMES souffle REQUIRED)
  find_path(VERITAS_SOUFFLE_INCLUDE_DIR souffle/SouffleInterface.h REQUIRED)
  set(VERITAS_SOUFFLE_PROVENANCE_FILE "" CACHE FILEPATH
      "Provenance JSON emitted by the pinned Souffle build")
  if(NOT EXISTS "${VERITAS_SOUFFLE_PROVENANCE_FILE}")
    message(FATAL_ERROR "A verified Souffle provenance manifest is required")
  endif()
  execute_process(COMMAND ${VERITAS_SOUFFLE_EXECUTABLE} --version
                  OUTPUT_VARIABLE VERITAS_SOUFFLE_VERSION
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
  file(READ "${VERITAS_SOUFFLE_PROVENANCE_FILE}" VERITAS_SOUFFLE_PROVENANCE)
  string(JSON VERITAS_SOUFFLE_REVISION GET
         "${VERITAS_SOUFFLE_PROVENANCE}" source_revision)
  string(JSON VERITAS_SOUFFLE_RECORDED_SHA GET
         "${VERITAS_SOUFFLE_PROVENANCE}" executable_sha256)
  file(SHA256 "${VERITAS_SOUFFLE_EXECUTABLE}" VERITAS_SOUFFLE_ACTUAL_SHA)
  string(LENGTH "${VERITAS_SOUFFLE_REVISION}" VERITAS_SOUFFLE_REVISION_LENGTH)
  if(NOT VERITAS_SOUFFLE_VERSION MATCHES "^Souffle: 2\\.5($|[ -])" OR
     NOT VERITAS_SOUFFLE_REVISION_LENGTH EQUAL 40 OR
     NOT VERITAS_SOUFFLE_REVISION MATCHES "^[0-9a-f]+$" OR
     NOT VERITAS_SOUFFLE_REVISION STREQUAL "5682a9f12e2668ecdd26348fe63cc508bc0fcf47" OR
     NOT VERITAS_SOUFFLE_ACTUAL_SHA STREQUAL VERITAS_SOUFFLE_RECORDED_SHA)
    message(FATAL_ERROR
            "VERITAS requires qualified Souffle 2.5 at 5682a9f12e2668ecdd26348fe63cc508bc0fcf47")
  endif()
elseif(NOT VERITAS_WPA_ENGINE STREQUAL "cpp-emergency")
  message(FATAL_ERROR "VERITAS_WPA_ENGINE must be souffle or cpp-emergency")
else()
  message(WARNING "VERITAS: building explicit degraded cpp-emergency WPA")
endif()
```

The pinned-build provisioning step writes `souffle-provenance.json` with tag, full source revision, build configuration, compiler identity, and executable SHA-256. CMake validates the exact revision and executable digest, then canonicalizes the manifest plus generated-bundle hashes into `EngineToolchainIdentity`; every Soufflé run manifest records it. A version substring alone is never sufficient qualification.

Build `src/facts/SouffleSemanticKeyFunctor.cpp` and `SemanticKeyCodec.cpp` as the private shared library `veritas-souffle-functors`, exporting only the three `extern "C"` stateful functor symbols declared in `semantic_key.dl`. `veritas_generate_souffle_program(NAME ReachabilityV2 SOURCE logic/reachability/reachability.v2.dl)` and the corresponding `MayWriteV2` call run `souffle -g` into the build tree with the functor library available through pinned `-L`/`-l` arguments. Link the generated programs and `veritas-souffle-worker` to that exact target and set a build-tree rpath so the worker loads the same library at evaluation time. `SemanticKeyFunctorTest` invokes the generated worker, proving symbol resolution and byte equality with `SemanticKeyCodec`; an unresolved or mismatched functor is a hard test failure.

Compile generated sources privately into `veritas-souffle-worker`; the worker selects the registered program from a required component argument and uses `-F`/`-D` directories. No generated source, functor ABI, or Soufflé type is installed as a VERITAS public interface or checked-in generated artifact.

- [ ] **Step 4: Implement the executor interface and adapters**

```cpp
struct WpaExecutionLimits {
  std::chrono::milliseconds timeout;
  std::uint64_t memory_mb;
  std::uint32_t threads;
};

class WpaExecutor {
 public:
  virtual ~WpaExecutor() = default;
  virtual facts::EngineIdentity identity() const = 0;
  virtual StatusOr<RawWpaEvaluation>
  Execute(const WpaExecutionEnvelope& input,
          const WpaExecutionLimits& limits) const = 0;
};

class SouffleWpaExecutor final : public WpaExecutor {
 public:
  explicit SouffleWpaExecutor(std::filesystem::path worker);
  facts::EngineIdentity identity() const override;
  StatusOr<RawWpaEvaluation>
  Execute(const WpaExecutionEnvelope&, const WpaExecutionLimits&) const override;
};
```

`WpaExecutionEnvelope` contains an executor-specific `AnalysisRunManifest`, exact engine/toolchain provenance, and the immutable `WpaLogicalComponentInput`. The Soufflé adapter creates a unique run-local temporary directory, writes the canonical logical inputs atomically, executes the compiled worker with LLVM process limits and `threads == 1`, reads and validates outputs only after exit code zero, and removes temporary files after parsing. Timeout, signal, non-zero exit, missing output, schema mismatch, and witness parse failure return a non-OK `Status` with no `RawWpaEvaluation`.

Each adapter rejects an envelope whose manifest engine identity or toolchain identity differs from its own. The C++ adapter invokes `CppRuleEvaluator` and exposes only `kCppConformance` or `kCppEmergency`. It receives an envelope over the same immutable logical-input bytes used for Soufflé comparison; it cannot read summaries or construct another semantic relation set. The test harness creates one logical object, asserts its bytes and mappings remain identical, derives the two valid envelopes, and only then compares canonical engine-independent facts.

- [ ] **Step 5: Run compiled engine and conformance suites**

Run:

```bash
cmake --preset default -DLLVM_PROJECT_BUILD_DIR=/Users/skg7on/Workspace/Projects/llvm-project/build -DVERITAS_WPA_ENGINE=souffle -DVERITAS_SOUFFLE_PROVENANCE_FILE=/path/to/souffle-provenance.json
cmake --build --preset default --target WpaExecutorTest SouffleWpaExecutorTest WpaExecutorConformanceTest SemanticKeyFunctorTest
./build/bin/WpaExecutorTest
./build/bin/SouffleWpaExecutorTest
./build/bin/WpaExecutorConformanceTest
./build/bin/SemanticKeyFunctorTest
```

Expected: compiled Soufflé execution, process failure handling, stateful functor symbol resolution and byte equality, and semantic equality with the C++ oracle all pass; no test is skipped.

- [ ] **Step 6: Verify the explicit emergency build separately**

Run:

```bash
cmake -S . -B build-cpp-emergency -G Ninja -DLLVM_PROJECT_BUILD_DIR=/Users/skg7on/Workspace/Projects/llvm-project/build -DVERITAS_WPA_ENGINE=cpp-emergency
cmake --build build-cpp-emergency --target WpaExecutorTest
./build-cpp-emergency/bin/WpaExecutorTest
```

Expected: configuration prints the degraded-mode warning, builds without generated Soufflé programs, and the executor identity test passes.

- [ ] **Step 7: Commit compiled production executors**

```bash
git add cmake/VeritasSouffle.cmake CMakeLists.txt cmake/Dependencies.cmake include/veritas/wpa/WpaExecutor.h include/veritas/wpa/SouffleWpaExecutor.h include/veritas/wpa/CppConformanceExecutor.h src/facts/SouffleSemanticKeyFunctor.cpp src/facts/CMakeLists.txt src/wpa/SouffleWpaExecutor.cpp src/wpa/CppConformanceExecutor.cpp src/wpa/SouffleWorkerMain.cpp src/wpa/CMakeLists.txt tests/unit/wpa/WpaExecutorTest.cpp tests/integration/facts/SemanticKeyFunctorTest.cpp tests/integration/wpa/SouffleWpaExecutorTest.cpp tests/integration/wpa/WpaExecutorConformanceTest.cpp tests/unit/wpa/CMakeLists.txt tests/integration/wpa/CMakeLists.txt
git commit -m "feat: require compiled Souffle WPA execution"
```

### Task 14: Add SCC orchestration, versioned run state, and atomic failure policy

**Files:**

- Create: `include/veritas/wpa/WpaOrchestrator.h`
- Create: `include/veritas/wpa/WpaRunRepository.h`
- Create: `src/wpa/WpaOrchestrator.cpp`
- Create: `src/wpa/WpaRunRepository.cpp`
- Create: `src/summarydb/schema/v2.sql`
- Modify: `include/veritas/summarydb/MetadataStore.h`
- Modify: `src/summarydb/MetadataStore.cpp`
- Modify: `src/summarydb/CMakeLists.txt`
- Modify: `include/veritas/wpa/SccStateRepository.h`
- Modify: `src/wpa/SccStateRepository.cpp`
- Modify: `include/veritas/wpa/WpaCoordinator.h`
- Modify: `src/wpa/WpaCoordinator.cpp`
- Modify: `src/wpa/CMakeLists.txt`
- Create: `tests/unit/wpa/WpaRunRepositoryTest.cpp`
- Create: `tests/unit/wpa/WpaOrchestratorTest.cpp`
- Modify: `tests/unit/wpa/SccStateRepositoryTest.cpp`
- Modify: `tests/unit/wpa/WpaCoordinatorTest.cpp`
- Modify: `tests/unit/wpa/CMakeLists.txt`

**Interfaces:**

- Consumes: call/SCC graphs, materializer, executor, canonicalizer, and metadata store.
- Produces: `WpaRunStatus`, `WpaComponentStatus`, `WpaRunRequest`, `WpaRunResult`, `WpaRunRepository`, and `WpaOrchestrator`.
- Persists: run manifests/status, component input/fixpoint/external hashes, diagnostics, and stale linkage; it does not persist M9 facts yet.
- Persists: each canonical component result as an opaque immutable cache object so a reused successor still supplies semantic facts and witnesses; the cache has no M9 query/index API.
- Reuses: an immutable successful component across runs only when `LogicalInputHash`, SCC, component, exact executor/toolchain identity, schema, rule, and model identities match; each run stores its own cache-object reference.
- Schedules: predecessor work only when `ExternalHash` changes.

- [ ] **Step 1: Write orchestration and failure-publication tests**

```cpp
TEST(WpaOrchestratorTest, RunsSccsInReverseTopologicalOrder) {
  RecordingExecutor executor;
  auto result = Orchestrator(executor).Run(ThreeSccRequest());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(executor.scc_order(), ReverseTopologicalSccIds());
}

TEST(WpaOrchestratorTest, FailedComponentPublishesNoNewState) {
  FailingExecutor executor(Status::Internal("worker crashed"));
  auto result = Orchestrator(executor).Run(RequestWithPriorSuccess());
  EXPECT_FALSE(result.ok());
  EXPECT_FALSE(CurrentComponentResult(NewRunId()).has_value());
  EXPECT_EQ(HistoricalComponentResult(PriorRunId()),
            PriorSuccessfulComponentResult());
  EXPECT_TRUE(IsPriorResultStaleFor(NewRunId()));
  EXPECT_EQ(RunStatus(NewRunId()), WpaRunStatus::kIncomplete);
}

TEST(WpaOrchestratorTest, WitnessOnlyChangeDoesNotSchedulePredecessor) {
  auto result = RunWithSameExternalHashAndNewFixpointHash();
  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(result->scheduled_predecessors.empty());
}

TEST(WpaOrchestratorTest, ReusesUnchangedComponentAcrossRevisions) {
  auto first = Orchestrator(SouffleExecutor()).Run(RequestForRevision(1));
  auto second = Orchestrator(SouffleExecutor()).Run(RequestForRevision(2));
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  EXPECT_NE(first->run.run_id, second->run.run_id);
  EXPECT_EQ(first->completed_components[0].result.logical_input_hash,
            second->completed_components[0].result.logical_input_hash);
  EXPECT_EQ(first->completed_components[0].result_object_key,
            second->completed_components[0].result_object_key);
  EXPECT_EQ(SouffleExecutionCount(), 1u);
}

TEST(WpaOrchestratorTest, ToolchainOrSemanticChangeMissesCache) {
  ASSERT_TRUE(Orchestrator(SouffleExecutor()).Run(BaseRequest()).ok());
  ASSERT_TRUE(Orchestrator(OtherQualifiedSouffle()).Run(BaseRequest()).ok());
  ASSERT_TRUE(Orchestrator(SouffleExecutor()).Run(ChangedSummaryRequest()).ok());
  EXPECT_EQ(SouffleExecutionCount(), 3u);
}
```

- [ ] **Step 2: Build and confirm orchestration tests fail**

Run:

```bash
cmake --build --preset default --target WpaRunRepositoryTest WpaOrchestratorTest SccStateRepositoryTest WpaCoordinatorTest
```

Expected: compilation fails because the V2 run repository and orchestrator are absent.

- [ ] **Step 3: Add the V2 metadata migration**

```sql
INSERT OR IGNORE INTO schema_version (version) VALUES (2);

CREATE TABLE IF NOT EXISTS wpa_analysis_runs (
  run_id TEXT PRIMARY KEY NOT NULL,
  revision_id TEXT NOT NULL,
  build_variant_id TEXT NOT NULL,
  summary_schema_version TEXT NOT NULL,
  relation_schema_version TEXT NOT NULL,
  rule_bundle_version TEXT NOT NULL,
  model_bundle_version TEXT NOT NULL,
  svf_configuration_hash TEXT NOT NULL,
  wpa_configuration_hash TEXT NOT NULL,
  engine_identity INTEGER NOT NULL,
  engine_toolchain_identity TEXT NOT NULL,
  status INTEGER NOT NULL,
  stale_base_run_id TEXT,
  started_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
  completed_at INTEGER
);

CREATE TABLE IF NOT EXISTS wpa_component_states_v2 (
  run_id TEXT NOT NULL,
  scc_id TEXT NOT NULL,
  component_kind INTEGER NOT NULL,
  logical_input_hash TEXT NOT NULL,
  fixpoint_hash TEXT NOT NULL,
  external_hash TEXT NOT NULL,
  result_cache_key TEXT NOT NULL,
  result_object_key TEXT NOT NULL,
  status INTEGER NOT NULL,
  diagnostics TEXT NOT NULL,
  updated_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
  PRIMARY KEY (run_id, scc_id, component_kind),
  FOREIGN KEY (run_id) REFERENCES wpa_analysis_runs(run_id)
);

CREATE TABLE IF NOT EXISTS wpa_component_result_cache_v2 (
  result_cache_key TEXT PRIMARY KEY NOT NULL,
  logical_input_hash TEXT NOT NULL,
  engine_toolchain_identity TEXT NOT NULL,
  relation_schema_version TEXT NOT NULL,
  rule_bundle_version TEXT NOT NULL,
  model_bundle_version TEXT NOT NULL,
  result_object_key TEXT NOT NULL,
  fixpoint_hash TEXT NOT NULL,
  external_hash TEXT NOT NULL
);
```

Embed and apply V1 then V2 idempotently. Keep existing M8 tables readable. `WpaRunRepository::Open(db_path)` owns the shared metadata connection plus a dedicated immutable object store at `db_path/wpa-component-results`. Derive `result_cache_key` from `LogicalInputHash`, SCC/component identity, exact engine/toolchain identity, and schema/rule/model versions. `StoreSuccessfulComponent` writes the canonical component-result bytes and immutable cache row before committing the current run's reference; `LoadReusableComponent` validates every key field and the object hash before returning complete facts, rooted inputs, and witnesses. A revision-only change can reuse the object, but creates a distinct `wpa_analysis_runs` and `wpa_component_states_v2` row. Add repository methods `BeginRun`, `LoadReusableComponent`, `StoreSuccessfulComponent`, `RecordComponentFailure`, `CompleteRun`, and `MarkIncomplete` with transactions around every metadata state transition.

- [ ] **Step 4: Implement bottom-up orchestration**

```cpp
struct WpaRunRequest {
  facts::AnalysisRunManifest run;
  std::span<const summary::SummaryArtifact> summaries;
  const analysis::semantic::ModelBundle& models;
  std::span<const WpaComponentKind> components;
  WpaExecutionLimits limits;
};

struct WpaComponentKey {
  core::StableId scc_id;
  WpaComponentKind component;
  auto operator<=>(const WpaComponentKey&) const = default;
};

struct WpaComponentCompletion {
  WpaComponentKey key;
  std::string result_object_key;
  WpaComponentResult result;
};

struct WpaRunResult {
  facts::AnalysisRunManifest run;
  std::vector<WpaComponentKey> expected_components;
  std::vector<WpaComponentCompletion> completed_components;
  std::vector<core::StableId> rooted_input_fact_ids;
  std::vector<runtime::WorkItem> scheduled_predecessors;
};

class WpaOrchestrator {
 public:
  WpaOrchestrator(WpaExecutor& executor, WpaRunRepository& repository);
  StatusOr<WpaRunResult> Run(const WpaRunRequest& request);
};
```

Build one call/SCC graph, freeze the expected `(SccId, ComponentKind)` set, iterate requested components and reverse-topological SCCs, load validated successor results, materialize one logical input, reuse a matching content-addressed result, execute otherwise, canonicalize, and transactionally store success. `StoreSuccessfulComponent` returns `WpaComponentCompletion`, making its immutable `result_object_key` and complete hash-bearing `WpaComponentResult` the sole completion metadata source. The returned expected set, completion records, and rooted input IDs are immutable inputs to Task 17. On any non-OK executor or canonicalizer status, record diagnostics, mark the run incomplete, return the failure, and do not call `StoreSuccessfulComponent`. Use engine-neutral `ExternalHash` comparison to enqueue predecessors through the existing M7 scheduler.

- [ ] **Step 5: Run run-state, orchestration, and incremental tests**

Run:

```bash
cmake --build --preset default --target WpaRunRepositoryTest WpaOrchestratorTest SccStateRepositoryTest WpaCoordinatorTest
./build/bin/WpaRunRepositoryTest
./build/bin/WpaOrchestratorTest
./build/bin/SccStateRepositoryTest
./build/bin/WpaCoordinatorTest
```

Expected: reverse-topological execution, validated cross-revision cache reuse, toolchain/semantic cache misses, failure atomicity, stale diagnostics, and external-only predecessor scheduling pass.

- [ ] **Step 6: Commit the production orchestrator and run state**

```bash
git add include/veritas/wpa/WpaOrchestrator.h include/veritas/wpa/WpaRunRepository.h src/wpa/WpaOrchestrator.cpp src/wpa/WpaRunRepository.cpp src/summarydb/schema/v2.sql include/veritas/summarydb/MetadataStore.h src/summarydb/MetadataStore.cpp src/summarydb/CMakeLists.txt include/veritas/wpa/SccStateRepository.h src/wpa/SccStateRepository.cpp include/veritas/wpa/WpaCoordinator.h src/wpa/WpaCoordinator.cpp src/wpa/CMakeLists.txt tests/unit/wpa/WpaRunRepositoryTest.cpp tests/unit/wpa/WpaOrchestratorTest.cpp tests/unit/wpa/SccStateRepositoryTest.cpp tests/unit/wpa/WpaCoordinatorTest.cpp tests/unit/wpa/CMakeLists.txt
git commit -m "feat: orchestrate atomic Souffle WPA runs"
```

### Task 15: Integrate required WPA into standard project analysis

**Files:**

- Modify: `include/veritas/analysis/ProjectAnalyzer.h`
- Modify: `src/analysis/ProjectAnalyzer.cpp`
- Modify: `src/analysis/ProjectAnalyzerInternal.h`
- Modify: `src/tools/veritas-build.cpp`
- Modify: `src/analysis/CMakeLists.txt`
- Modify: `src/tools/CMakeLists.txt`
- Modify: `tests/integration/analysis/ProjectAnalyzerTest.cpp`
- Create: `tests/integration/analysis/ProjectAnalyzerWpaTest.cpp`
- Create: `tests/integration/analysis/WpaEmergencyModeTest.cpp`
- Modify: `tests/integration/analysis/CMakeLists.txt`
- Modify: `tests/integration/wpa/WpaEndToEndTest.cpp`

**Interfaces:**

- Adds: public `WpaEngineMode::{kSouffle,kCppEmergency}` and WPA limits/version fields to `AnalysisConfig`.
- Adds: `wpa_run_id`, `wpa_engine`, and `wpa_diagnostics` to `ProjectAnalysisResult`.
- Defaults: `AnalysisConfig::Default().wpa_engine == kSouffle`.
- Adds CLI: `veritas-build analyze --wpa-engine=souffle|cpp-emergency` with no `auto` mode.
- Executes: reachability and may-write through `WpaOrchestrator` after summary/CPG publication.

- [ ] **Step 1: Write standard, failure, and emergency-path tests**

```cpp
TEST(ProjectAnalyzerWpaTest, DefaultAnalysisPublishesSouffleRunIdentity) {
  auto result = AnalyzeFixture("multiple_tus", AnalysisConfig::Default());
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_FALSE(result->wpa_run_id.empty());
  EXPECT_EQ(result->wpa_engine, WpaEngineMode::kSouffle);
  EXPECT_TRUE(result->wpa_diagnostics.empty());
}

TEST(ProjectAnalyzerWpaTest, SouffleFailureDoesNotInvokeCpp) {
  RecordingCppExecutor cpp;
  auto analyzer = AnalyzerWithExecutors(FailingSouffleExecutor(), cpp);
  auto result = analyzer.AnalyzeProject(Request(), AnalysisConfig::Default());
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(cpp.invocation_count(), 0u);
  EXPECT_EQ(PersistedRunStatus(), WpaRunStatus::kIncomplete);
}

TEST(WpaEmergencyModeTest, ExplicitModeUsesDistinctRunIdentity) {
  auto config = AnalysisConfig::Default();
  config.wpa_engine = WpaEngineMode::kCppEmergency;
  auto emergency = AnalyzeFixture("multiple_tus", config);
  ASSERT_TRUE(emergency.ok());
  EXPECT_NE(emergency->wpa_run_id, SouffleRunIdForSameFixture());
  EXPECT_TRUE(ContainsDegradedOperationDiagnostic(*emergency));
}

TEST(WpaEmergencyModeTest, SoufflelessBuildNeverLooksUpSouffleProvenance) {
  FailingIfCalledSouffleProvenanceLookup souffle_lookup;
  auto analyzer = EmergencyBuildAnalyzer(souffle_lookup);
  auto config = AnalysisConfig::Default();
  config.wpa_engine = WpaEngineMode::kCppEmergency;
  auto result = analyzer.AnalyzeProject(Request(), config);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(souffle_lookup.invocation_count(), 0u);
  EXPECT_EQ(result->wpa_engine, WpaEngineMode::kCppEmergency);
}
```

- [ ] **Step 2: Build and confirm pipeline tests fail**

Run:

```bash
cmake --build --preset default --target ProjectAnalyzerWpaTest WpaEmergencyModeTest WpaEndToEndTest
```

Expected: compilation fails because project analysis has no WPA configuration/result fields or orchestrator call.

- [ ] **Step 3: Add explicit engine configuration and orchestration**

```cpp
enum class WpaEngineMode : std::uint8_t {
  kSouffle,
  kCppEmergency,
};

struct AnalysisConfig {
  std::chrono::seconds svf_soft_analysis_budget;
  std::size_t svf_max_graph_nodes;
  std::size_t svf_max_emitted_facts;
  WpaEngineMode wpa_engine;
  std::chrono::milliseconds wpa_component_timeout;
  std::uint64_t wpa_component_memory_mb;
  std::uint32_t wpa_threads;
  std::string rule_bundle_version;
  std::string model_bundle_version;
  bool run_cpp_conformance_oracle;
  static AnalysisConfig Default();
};
```

`AnalysisConfig::Default()` sets `wpa_threads` to `1` and validation rejects every other value in this qualification cycle. Select exactly one executor from `wpa_engine` before constructing an engine-specific manifest. A Soufflé selection requires the verified Soufflé provenance and creates a `kSouffle` manifest with that toolchain identity. A `cpp-emergency` selection performs no Soufflé discovery or provenance lookup and creates a `kCppEmergency` manifest from the VERITAS C++ build identity; configuration rejects `run_cpp_conformance_oracle` in emergency mode. This preserves a genuinely Soufflé-less emergency build without weakening normal production requirements.

After atomic summary/CPG publication, list the just-published V2 artifacts, materialize each engine-neutral logical component input once, wrap it in the selected manifest, and execute reachability plus may-write. In Soufflé mode, when `run_cpp_conformance_oracle` is true, create a second manifest with `EngineIdentity::kCppConformance` and its C++ toolchain identity, wrap the same immutable logical-input bytes in that distinct envelope, and compare canonical engine-independent fact IDs. Record that oracle under its distinct `RunId`; never publish or label it as the Soufflé run.

CLI parsing rejects unknown values and records `cpp-emergency` as a degraded operation. It never catches a Soufflé error to retry with C++.

- [ ] **Step 4: Run standard pipeline and emergency tests**

Run:

```bash
cmake --build --preset default --target ProjectAnalyzerWpaTest WpaEmergencyModeTest WpaEndToEndTest ProjectAnalyzerTest
./build/bin/ProjectAnalyzerWpaTest
./build/bin/WpaEmergencyModeTest
./build/bin/WpaEndToEndTest
./build/bin/ProjectAnalyzerTest
```

Expected: standard project analysis uses Soufflé, injected failure never invokes C++, and explicit emergency analysis uses a distinct run with degraded diagnostics and no Soufflé provenance lookup.

- [ ] **Step 5: Commit M8R.4**

```bash
git add include/veritas/analysis/ProjectAnalyzer.h src/analysis/ProjectAnalyzer.cpp src/analysis/ProjectAnalyzerInternal.h src/tools/veritas-build.cpp src/analysis/CMakeLists.txt src/tools/CMakeLists.txt tests/integration/analysis/ProjectAnalyzerTest.cpp tests/integration/analysis/ProjectAnalyzerWpaTest.cpp tests/integration/analysis/WpaEmergencyModeTest.cpp tests/integration/analysis/CMakeLists.txt tests/integration/wpa/WpaEndToEndTest.cpp
git commit -m "feat: require Souffle WPA in project analysis"
```

---

## M8R.5 — Qualification and M9 Handoff

### Task 16: Build the differential, determinism, migration, failure, and performance corpus

**Files:**

- Create: `tests/qualification/CMakeLists.txt`
- Create: `tests/qualification/wpa/CMakeLists.txt`
- Create: `tests/qualification/wpa/WpaDifferentialQualificationTest.cpp`
- Create: `tests/qualification/wpa/WpaDeterminismQualificationTest.cpp`
- Create: `tests/qualification/wpa/WpaFailureQualificationTest.cpp`
- Create: `tests/qualification/wpa/WpaMigrationQualificationTest.cpp`
- Create: `tests/qualification/wpa/WpaPerformanceQualificationTest.cpp`
- Create: `tests/qualification/check_no_skips.py`
- Create: `tests/qualification/wpa/performance-ceilings.json`
- Create: `tests/fixtures/projects/recursive_calls/compile_commands.json`
- Create: `tests/fixtures/projects/recursive_calls/recursive_calls.cpp`
- Create: `tests/fixtures/projects/callback_dispatch/compile_commands.json`
- Create: `tests/fixtures/projects/callback_dispatch/callback_dispatch.cpp`
- Create: `tests/fixtures/projects/unknown_external/compile_commands.json`
- Create: `tests/fixtures/projects/unknown_external/unknown_external.cpp`
- Create: `tests/fixtures/projects/abstract_memory/compile_commands.json`
- Create: `tests/fixtures/projects/abstract_memory/abstract_memory.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `.github/workflows/ci.yml`

**Interfaces:**

- Qualifies: every overlapping reachability/may-write result from one byte-identical engine-neutral `WpaLogicalComponentInput`, wrapped in distinct valid Soufflé and C++ conformance execution envelopes.
- Qualifies: input/member/evaluation-order determinism, V1 compatibility, native V2 reanalysis, failure atomicity, repeated-process analysis, and resource ceilings.
- Produces: aggregate `wpa-qualification` tests for this task and an exact cross-task registry assigning each M9 criterion label plus composite `m9-entry` to named aggregate tests as their owning tasks create them.
- Makes CI: install/build the pinned supported Soufflé version and run without skips.

- [ ] **Step 1: Write the qualification matrix as parameterized tests**

```cpp
struct QualificationCase {
  std::string name;
  std::string fixture;
  WpaComponentKind component;
};

class WpaDifferentialQualificationTest
    : public ::testing::TestWithParam<QualificationCase> {};

TEST_P(WpaDifferentialQualificationTest, SouffleEqualsCppOracle) {
  auto logical = MaterializeLogicalFixture(GetParam().fixture,
                                           GetParam().component);
  ASSERT_TRUE(logical.ok());
  const auto logical_bytes = SerializeCanonical(*logical);
  auto souffle_input = EnvelopeFor(*logical, SouffleManifest());
  auto cpp_input = EnvelopeFor(*logical, CppConformanceManifest());
  ASSERT_NE(souffle_input.run.run_id, cpp_input.run.run_id);
  EXPECT_EQ(SerializeCanonical(souffle_input.logical), logical_bytes);
  EXPECT_EQ(SerializeCanonical(cpp_input.logical), logical_bytes);
  EXPECT_EQ(souffle_input.logical.mappings, cpp_input.logical.mappings);
  auto souffle = ExecuteAndCanonicalize(SouffleExecutor(), souffle_input);
  auto cpp = ExecuteAndCanonicalize(CppOracle(), cpp_input);
  ASSERT_TRUE(souffle.ok());
  ASSERT_TRUE(cpp.ok());
  EXPECT_EQ(souffle->facts, cpp->facts);
  EXPECT_EQ(souffle->external_hash, cpp->external_hash);
  EXPECT_TRUE(AllFactsHaveClosedWitnesses(*souffle));
}

TEST(WpaDifferentialQualificationTest, UnknownRangeIsLosslessAcrossEngines) {
  auto logical = MaterializeLogicalFixture("abstract_memory_unknown_range",
                                           WpaComponentKind::kMemoryEffects);
  ASSERT_TRUE(logical.ok());
  EXPECT_TRUE(ContainsRangeKind(*logical, ByteRangeKind::kUnknown));
  EXPECT_EQ(RunBothValidEnvelopes(*logical).canonical_facts,
            ExpectedUnknownRangeFacts());
}

INSTANTIATE_TEST_SUITE_P(
    M9Entry, WpaDifferentialQualificationTest,
    ::testing::Values(
        QualificationCase{"direct", "multiple_tus",
                          WpaComponentKind::kReachability},
        QualificationCase{"recursive", "recursive_calls",
                          WpaComponentKind::kReachability},
        QualificationCase{"function_pointer", "function_pointer",
                          WpaComponentKind::kReachability},
        QualificationCase{"callback", "callback_dispatch",
                          WpaComponentKind::kReachability},
        QualificationCase{"memory", "abstract_memory",
                          WpaComponentKind::kMemoryEffects}));
```

- [ ] **Step 2: Add deterministic permutation and failure injection tests**

```cpp
TEST(WpaDeterminismQualificationTest, AllInputPermutationsHaveOneResult) {
  auto canonical = RunPermutation(0);
  ASSERT_TRUE(canonical.ok());
  for (std::uint32_t seed = 1; seed <= 64; ++seed) {
    auto permuted = RunPermutation(seed);
    ASSERT_TRUE(permuted.ok());
    EXPECT_EQ(permuted->facts, canonical->facts);
    EXPECT_EQ(permuted->witnesses, canonical->witnesses);
    EXPECT_EQ(permuted->fixpoint_hash, canonical->fixpoint_hash);
  }
}

TEST(WpaFailureQualificationTest, EveryFailureRetainsPriorSuccess) {
  for (auto failure : {FailureKind::kMissingWorker,
                       FailureKind::kIncompatibleBundle,
                       FailureKind::kTimeout,
                       FailureKind::kCrash,
                       FailureKind::kMalformedWitness,
                       FailureKind::kSchemaMismatch}) {
    SCOPED_TRACE(static_cast<int>(failure));
    EXPECT_TRUE(VerifyPriorSuccessRetainedAndNewRunIncomplete(failure));
  }
}
```

- [ ] **Step 3: Add migration and process-lifecycle qualification**

```cpp
TEST(WpaMigrationQualificationTest, V1ProjectionIsTaggedAndNeverFabricates) {
  auto input = MaterializeHistoricalV1Summary();
  ASSERT_TRUE(input.ok());
  EXPECT_TRUE(ContainsLegacyOpaqueMemory(*input));
  EXPECT_TRUE(ContainsUnknownDispatch(*input));
  EXPECT_EQ(input->semantics.summary_schema_version, "summary.v1-compat-v2");
}

TEST(WpaMigrationQualificationTest, ReanalysisSupersedesWithoutMutation) {
  const auto old_bytes = ReadHistoricalV1Bytes();
  ASSERT_TRUE(ReanalyzeAsV2().ok());
  EXPECT_EQ(ReadHistoricalV1Bytes(), old_bytes);
  EXPECT_TRUE(CurrentBindingIsV2());
  EXPECT_TRUE(HistoricalWpaResultIsStale());
}

TEST(WpaMigrationQualificationTest, RepeatedAnalysisCleansAllEngineState) {
  for (int run = 0; run < 20; ++run)
    ASSERT_TRUE(AnalyzeFixtureInCurrentProcess("multiple_tus").ok());
  EXPECT_TRUE(SvfGlobalStateIsCleanForTest());
  EXPECT_EQ(ActiveSouffleWorkers(), 0u);
}
```

- [ ] **Step 4: Add explicit performance ceilings and measurements**

```json
{
  "schema_version": "wpa-performance.v1",
  "fixture": "recursive_calls",
  "maximum_wall_time_ms": 30000,
  "maximum_peak_rss_mb": 2048,
  "maximum_output_facts": 1000000
}
```

The performance test runs five warmed iterations, records the median wall time and maximum RSS in CTest output, and fails only when a checked-in ceiling is exceeded. It also asserts identical semantic hashes across all five runs.

- [ ] **Step 5: Install the supported Soufflé package in CI and run qualification**

Add one CI setup step that checks out Soufflé tag `2.5` and verifies full revision `5682a9f12e2668ecdd26348fe63cc508bc0fcf47`, builds/installs it, emits the required provenance JSON with that 40-character revision and installed executable SHA-256, configures `-DVERITAS_WPA_ENGINE=souffle -DVERITAS_SOUFFLE_PROVENANCE_FILE=<manifest>`, and treats every missing, disabled, or skipped `m9-entry` test as failure.

Register the exact gate membership with `set_tests_properties(... PROPERTIES LABELS "m9-entry;<criterion>")` in each test's owning task and CMake file. Task 16 additionally labels its five qualification aggregates `wpa-qualification`:

| Criterion label | Exact aggregate tests | Registration owner |
|---|---|---|
| `summary-v2` | `FunctionSummaryV2Test`, `SummaryRepositoryVersionTest` | Tasks 5–6 |
| `indirect-calls` | `SvfFactMapperV2Test`, `CallGraphTest` | Tasks 8 and 10 |
| `stable-identity` | `StableValueIdentityTest`, `AbstractMemoryBuilderTest`, `DenseIdMapTest` | Tasks 4 and 7 |
| `relations-v2` | `RelationSchemaTest`, `WpaInputMaterializerTest`, `WpaDeterminismQualificationTest` | Tasks 3, 10, and 16 |
| `souffle-production` | `SouffleWpaExecutorTest`, `ProjectAnalyzerWpaTest`, `WpaPerformanceQualificationTest` | Tasks 13, 15, and 16 |
| `engine-conformance` | `WpaExecutorConformanceTest`, `WpaDifferentialQualificationTest` | Tasks 13 and 16 |
| `witness-closure` | `WitnessCanonicalizerTest`, `AnalysisFactBusTest` | Tasks 11 and 17 |
| `failure-atomicity` | `WpaFailureQualificationTest`, `WpaOrchestratorTest` | Tasks 16 and 14 |
| `run-identity` | `AnalysisRunTest`, `WpaMigrationQualificationTest` | Tasks 2 and 16 |
| `documentation-consistency` | `M9DocumentationConsistencyTest` | Task 18 |

Register each table entry as a named aggregate CTest test with `add_test(NAME <exact-name> COMMAND $<TARGET_FILE:<target>>)` (or the Python interpreter for the documentation test), then assign the two labels shown. Any separately discovered GoogleTest cases may retain their normal labels, but they are not substitutes for these aggregate gate members.

`check_no_skips.py` parses CTest JUnit XML, accepts the exact expected names for a selected label set, and fails on every `<skipped>` element, disabled test, missing or extra expected test, non-zero failure/error count, or duplicate test name. During Task 16 it checks the five `wpa-qualification` aggregates; Task 18's gate checks every criterion set. This makes skip rejection machine-readable without requiring the not-yet-created Task 17–18 tests to pass during Task 16.

Run locally:

```bash
cmake --preset default -DLLVM_PROJECT_BUILD_DIR=/Users/skg7on/Workspace/Projects/llvm-project/build -DVERITAS_WPA_ENGINE=souffle -DVERITAS_SOUFFLE_PROVENANCE_FILE=/path/to/souffle-provenance.json
cmake --build --preset default --target WpaDifferentialQualificationTest WpaDeterminismQualificationTest WpaFailureQualificationTest WpaMigrationQualificationTest WpaPerformanceQualificationTest
ctest --test-dir build -L wpa-qualification --no-tests=error --output-on-failure --output-junit build/wpa-qualification.xml
python3 tests/qualification/check_no_skips.py build/wpa-qualification.xml --expect WpaDifferentialQualificationTest,WpaDeterminismQualificationTest,WpaFailureQualificationTest,WpaMigrationQualificationTest,WpaPerformanceQualificationTest
```

Expected: all differential, determinism, failure, migration, lifecycle, and performance tests pass with zero skips.

- [ ] **Step 6: Commit the qualification corpus**

```bash
git add tests/qualification tests/fixtures/projects/recursive_calls tests/fixtures/projects/callback_dispatch tests/fixtures/projects/unknown_external tests/fixtures/projects/abstract_memory tests/CMakeLists.txt .github/workflows/ci.yml
git commit -m "test: qualify production Souffle WPA"
```

### Task 17: Add the generic Analysis Fact Bus handoff

**Files:**

- Create: `include/veritas/facts/AnalysisFactBus.h`
- Create: `src/facts/AnalysisFactBus.cpp`
- Modify: `include/veritas/wpa/WpaOrchestrator.h`
- Modify: `src/wpa/WpaOrchestrator.cpp`
- Modify: `src/facts/CMakeLists.txt`
- Create: `tests/unit/facts/AnalysisFactBusTest.cpp`
- Modify: `tests/unit/facts/CMakeLists.txt`
- Modify: `tests/integration/wpa/WpaEndToEndTest.cpp`

**Interfaces:**

- Consumes: only a successful `WpaRunResult`, including its frozen expected component set, completed component records/hashes, rooted input fact IDs, facts, witnesses, run manifest, and diagnostics.
- Produces: `AnalysisFactBatch`, abstract `AnalysisFactSink`, and `AnalysisFactBus::Publish`.
- Validates: one manifest, stable fact uniqueness, witness closure against declared rooted inputs, engine identity, exact expected/completed component equality, and no conflicting semantic row for one fact ID.
- Delivers: idempotent at least once to each named sink under canonical `(RunId, BatchId)`; partial fan-out is durably tracked per sink and safe to retry.
- Provides: the stable M9 ingestion seam without adding a durable M9 store early.

- [ ] **Step 1: Write bus validation and delivery tests**

```cpp
TEST(AnalysisFactBusTest, DeliversOneValidatedImmutableBatch) {
  RecordingSink sink;
  AnalysisFactBus bus(DeliveryRepository());
  bus.AddSink("recording", sink);
  auto status = bus.Publish(SuccessfulBatch());
  ASSERT_TRUE(status.ok());
  ASSERT_EQ(sink.batches().size(), 1u);
  EXPECT_EQ(sink.batches()[0].run.run_id, SuccessfulRunId());
}

TEST(AnalysisFactBusTest, RejectsIncompleteOrMixedRunBatch) {
  EXPECT_FALSE(Bus().Publish(IncompleteBatch()).ok());
  EXPECT_FALSE(Bus().Publish(BatchWithTwoRunIds()).ok());
}

TEST(AnalysisFactBusTest, RejectsFactWithoutClosedWitness) {
  EXPECT_EQ(Bus().Publish(BatchWithOrphan()).code(),
            StatusCode::kFailedPrecondition);
}

TEST(AnalysisFactBusTest, RejectsClosedSubsetWithMissingComponent) {
  auto batch = SuccessfulBatch();
  batch.completed_components.pop_back();
  EXPECT_EQ(Bus().Publish(std::move(batch)).code(),
            StatusCode::kFailedPrecondition);
}

TEST(AnalysisFactBusTest, RejectsWitnessLeafOutsideRootSet) {
  EXPECT_EQ(Bus().Publish(BatchWithUndeclaredRoot()).code(),
            StatusCode::kFailedPrecondition);
}

TEST(AnalysisFactBusTest, RetryAfterPartialFanoutIsIdempotent) {
  RecordingSink first;
  FailOnceSink second;
  AnalysisFactBus bus(DeliveryRepository());
  bus.AddSink("first", first);
  bus.AddSink("second", second);
  auto batch = SuccessfulBatch();
  EXPECT_FALSE(bus.Publish(batch).ok());
  EXPECT_TRUE(bus.Publish(batch).ok());
  EXPECT_EQ(first.logical_publication_count(batch.batch_id), 1u);
  EXPECT_EQ(second.logical_publication_count(batch.batch_id), 1u);
}
```

- [ ] **Step 2: Build and confirm bus tests fail**

Run:

```bash
cmake --build --preset default --target AnalysisFactBusTest WpaEndToEndTest
```

Expected: compilation fails because the Fact Bus contract is absent.

- [ ] **Step 3: Implement the M9-neutral handoff**

```cpp
struct AnalysisFactBatch {
  core::StableId batch_id;
  AnalysisRunManifest run;
  std::vector<wpa::WpaComponentKey> expected_components;
  std::vector<wpa::WpaComponentCompletion> completed_components;
  std::vector<core::StableId> rooted_input_fact_ids;
  std::vector<AnalysisFact> facts;
  std::vector<WitnessEdge> witnesses;
  std::vector<std::string> diagnostics;
};

class AnalysisFactSink {
 public:
  virtual ~AnalysisFactSink() = default;
  // Repeated (run_id, batch_id) publication is a successful no-op.
  virtual Status Publish(const AnalysisFactBatch& batch) = 0;
};

class AnalysisFactBus {
 public:
  explicit AnalysisFactBus(wpa::WpaRunRepository& delivery_state);
  void AddSink(std::string sink_id, AnalysisFactSink& sink);
  Status Publish(AnalysisFactBatch batch) const;
};
```

Construct the batch only from `WpaRunResult`. Canonicalize component, rooted-input, fact, and witness ordering; derive `BatchId` from that immutable semantic content. Require set equality between `expected_components` and the keys in `completed_components`, verify each completion hash against the run result, and close every witness leaf over either a published fact or `rooted_input_fact_ids`. Only then deliver the immutable batch.

Every sink must implement idempotent at-least-once publication keyed by `(run_id, batch_id)`. The bus records per-sink pending/completed delivery state in the run repository. A sink failure returns non-OK without mutating component success; retry visits only pending sinks, and a defensive repeated call to a completed sink is a successful no-op. Cross-sink atomicity is not claimed. M9 will implement a transactional durable sink and `explainFact` over these exact witness edges.

- [ ] **Step 4: Connect successful orchestration to the bus and run tests**

Run:

```bash
cmake --build --preset default --target AnalysisFactBusTest WpaEndToEndTest
./build/bin/AnalysisFactBusTest
./build/bin/WpaEndToEndTest
```

Expected: only complete validated batches are delivered, rooted closure is independently checked, partial fan-out retries without duplicate logical publication, and end-to-end WPA exposes the exact fact/witness handoff expected by M9.

- [ ] **Step 5: Commit the Fact Bus**

```bash
git add include/veritas/facts/AnalysisFactBus.h src/facts/AnalysisFactBus.cpp include/veritas/wpa/WpaOrchestrator.h src/wpa/WpaOrchestrator.cpp src/facts/CMakeLists.txt tests/unit/facts/AnalysisFactBusTest.cpp tests/unit/facts/CMakeLists.txt tests/integration/wpa/WpaEndToEndTest.cpp
git commit -m "feat: add WPA analysis fact bus"
```

### Task 18: Synchronize architecture/milestones and enforce the M9 entry gate

**Files:**

- Create: `docs/specs/milestones/m8r-souffle-wpa-remediation-design-spec.md`
- Create: `tools/check_m9_entry.py`
- Create: `tests/qualification/M9EntryGateTest.py`
- Create: `tests/qualification/M9DocumentationConsistencyTest.py`
- Modify: `README.md`
- Modify: `docs/architecture/veritas-platform-architecture-design.md`
- Modify: `docs/architecture/veritas-whole-program-analysis-design.md`
- Modify: `docs/architecture/veritas-thin-summarydb-backends-design.md`
- Modify: `docs/specs/milestones/README.md`
- Modify: `docs/specs/milestones/m8-scc-wpa-souffle-fact-engine-design-spec.md`
- Modify: `docs/specs/milestones/m9-provenance-fact-store-explain-api-design-spec.md`
- Modify: `docs/specs/veritas-backbone-milestones-and-implementation-plan.md`
- Modify: `docs/brainstorm/souffle-analysis-architecture.md`
- Modify: `CMakeLists.txt`
- Modify: `tests/qualification/CMakeLists.txt`

**Interfaces:**

- Documents: M8R.1–M8R.5 as remediation after immutable M8 history; divides M10 into M10A/M10B; keeps M13 research independent.
- States current behavior: SVF points-to owner, Summary IR durable boundary, Soufflé production recursion owner, C++ oracle/emergency only.
- Marks: the brainstorm as superseded design input while preserving its contents.
- Produces: `python3 tools/check_m9_entry.py --build-dir build` as the single executable M9 gate.

- [ ] **Step 1: Write the gate test before the checker**

```python
class M9EntryGateTest(unittest.TestCase):
    def test_gate_rejects_cpp_emergency_build(self):
        result = run_gate(build_dir=fixture_build("cpp-emergency"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("production engine is not souffle", result.stderr)

    def test_gate_requires_all_ten_criteria(self):
        result = run_gate(build_dir=fixture_build("missing-witness-test"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("criterion 7", result.stderr)

    def test_gate_rejects_skipped_or_disabled_test(self):
        result = run_gate(build_dir=fixture_build("skipped-conformance"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("skipped or disabled", result.stderr)
```

- [ ] **Step 2: Run and confirm the gate test fails**

Run:

```bash
python3 tests/qualification/M9EntryGateTest.py
```

Expected: failure because `tools/check_m9_entry.py` does not exist.

- [ ] **Step 3: Implement the executable gate**

```python
REQUIRED_CTEST_LABELS = {
    1: "summary-v2",
    2: "indirect-calls",
    3: "stable-identity",
    4: "relations-v2",
    5: "souffle-production",
    6: "engine-conformance",
    7: "witness-closure",
    8: "failure-atomicity",
    9: "run-identity",
    10: "documentation-consistency",
}

def check_cache(build_dir: pathlib.Path) -> None:
    cache = (build_dir / "CMakeCache.txt").read_text()
    require("VERITAS_WPA_ENGINE:STRING=souffle" in cache,
            "production engine is not souffle")

def run_criteria(build_dir: pathlib.Path) -> None:
    for number, label in REQUIRED_CTEST_LABELS.items():
        junit = build_dir / f"m9-entry-{number}.xml"
        completed = subprocess.run(
            ["ctest", "--test-dir", str(build_dir), "-L", label,
             "--no-tests=error", "--output-on-failure",
             "--output-junit", str(junit)], text=True, capture_output=True)
        require(completed.returncode == 0,
                f"criterion {number} failed: {label}\n{completed.stdout}\n{completed.stderr}")
        report = xml.etree.ElementTree.parse(junit)
        cases = report.findall(".//testcase")
        names = {case.attrib["name"] for case in cases}
        require(names == EXPECTED_TESTS_BY_LABEL[label],
                f"criterion {number} test membership mismatch: {names}")
        require(not any(case.find("skipped") is not None for case in cases),
                f"criterion {number} contains skipped or disabled tests")
        require(not report.findall(".//failure") and
                not report.findall(".//error"),
                f"criterion {number} contains failures or errors")
```

Define `EXPECTED_TESTS_BY_LABEL` from Task 16's exact table, including `M9DocumentationConsistencyTest`. Register that Python test with labels `m9-entry;documentation-consistency`. Also check that rule/model manifests exist and match the current run defaults, the configured Soufflé provenance manifest contains the exact 40-character source revision `5682a9f12e2668ecdd26348fe63cc508bc0fcf47` and the actual executable digest, generated compiled targets exist, and the canonical architecture/milestone/README files contain the same ownership statements. Emit one pass/fail line per criterion and a non-zero exit on any missing, extra, disabled, skipped, failed, or errored test.

- [ ] **Step 4: Update canonical documentation**

The M8 document remains an implemented historical record and gains one forward link to the M8R spec. The M8R document links to the approved architecture spec and records each gate’s delivered commit/test labels. The M9 document replaces `FactTuple` input with `AnalysisFactBatch`. The platform and WPA architecture documents contain the target component ownership and run-local relational projection. The brainstorm receives a header note identifying the approved replacement spec; its original analysis remains below the note.

- [ ] **Step 5: Run the gate test and documentation checks**

Run:

```bash
python3 tests/qualification/M9EntryGateTest.py
python3 tools/check_m9_entry.py --build-dir build
git diff --check
```

Expected: unit tests for the gate pass, all ten live-build criteria pass, and `git diff --check` emits no errors.

- [ ] **Step 6: Run full repository verification**

Run:

```bash
cmake --preset default -DLLVM_PROJECT_BUILD_DIR=/Users/skg7on/Workspace/Projects/llvm-project/build -DVERITAS_WPA_ENGINE=souffle -DVERITAS_SOUFFLE_PROVENANCE_FILE=/path/to/souffle-provenance.json
cmake --build --preset default
ctest --preset default
python3 tools/check_m9_entry.py --build-dir build
```

Expected: configure and build exit zero, CTest reports zero failed and zero skipped M9-entry tests, and the M9 gate passes all ten criteria.

- [ ] **Step 7: Commit M8R.5 and the M9 handoff**

```bash
git add docs/specs/milestones/m8r-souffle-wpa-remediation-design-spec.md tools/check_m9_entry.py tests/qualification/M9EntryGateTest.py tests/qualification/M9DocumentationConsistencyTest.py README.md docs/architecture/veritas-platform-architecture-design.md docs/architecture/veritas-whole-program-analysis-design.md docs/architecture/veritas-thin-summarydb-backends-design.md docs/specs/milestones/README.md docs/specs/milestones/m8-scc-wpa-souffle-fact-engine-design-spec.md docs/specs/milestones/m9-provenance-fact-store-explain-api-design-spec.md docs/specs/veritas-backbone-milestones-and-implementation-plan.md docs/brainstorm/souffle-analysis-architecture.md CMakeLists.txt tests/qualification/CMakeLists.txt
git commit -m "docs: qualify M9 handoff after Souffle WPA remediation"
```

---

## Spec Coverage Matrix

| Approved design requirement | Implemented by |
|---|---|
| Component ownership and no silent fallback | Tasks 8, 13, 15 |
| `summary.v2`, `relations.v2`, `wpa-run.v1` | Tasks 2, 3, 5 |
| Stable/dense dual identity | Tasks 1, 4, 10 |
| Semantic/epistemic separation | Tasks 1, 3, 5, 8 |
| Abstract object + access path + byte range | Tasks 5, 7, 8 |
| Indirect calls from SVF | Tasks 8, 9, 10 |
| Per-SCC successor support | Tasks 10, 12, 14 |
| Engine-neutral logical input/fixpoint/external hashes and cross-revision cache | Tasks 10, 11, 14 |
| Generic finite rooted witnesses | Tasks 11, 12, 17 |
| Atomic failure and stale prior results | Tasks 13, 14, 16 |
| C++ conformance/emergency identities | Tasks 2, 13, 15, 16 |
| V1 compatibility without fabricated precision | Tasks 4, 6, 16 |
| Complete, rooted, idempotently delivered Fact Bus M9 handoff | Task 17 |
| M9 hard entry criteria and documentation | Tasks 16, 18 |

## Final Verification Checklist

- [ ] `summary.v1` serialized fixtures remain byte-identical and readable.
- [ ] Native analysis publishes only `summary.v2`.
- [ ] Function-pointer and callback fixtures create stable MAY call edges.
- [ ] Distinct unnamed values and allocations have distinct stable IDs.
- [ ] All alias kinds and all six epistemic states cross the fact boundary.
- [ ] Every hot relation uses typed dense IDs and round-trips through stable maps.
- [ ] Compiled Soufflé is selected in the production CMake cache and its exact source/executable provenance is verified.
- [ ] C++ consumes byte-identical engine-neutral logical inputs under a distinct valid envelope and is never selected implicitly.
- [ ] Every derived fact has a deterministic finite rooted witness.
- [ ] Timeout/crash/schema/witness failures publish no replacement component.
- [ ] Witness-only changes leave `ExternalHash` and predecessor scheduling unchanged.
- [ ] Qualification reports semantic equality, determinism, migration safety, and performance within ceilings.
- [ ] `AnalysisFactBatch` is the only M9 input contract and proves component completion, rooted-input closure, and idempotent sink identity.
- [ ] `python3 tools/check_m9_entry.py --build-dir build` passes all ten criteria.

## Execution Handoff

Implement Tasks 1–18 in order. Treat each milestone gate as a review checkpoint: do not start the next gate until the focused verification command for the current gate passes and its commits have been reviewed. M9 implementation begins only after Task 18’s full verification and M9 entry gate both pass.
