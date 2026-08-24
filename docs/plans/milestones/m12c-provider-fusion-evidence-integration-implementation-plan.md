# M12C Provider Fusion and Evidence Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expose one pinned SummaryDB query view over native and explicitly selected provider state, derive non-destructive comparison records, and carry provider bindings/capabilities/assumptions/unknowns through M10B `EvidenceBuildInput` into validated M10C Evidence IR.

**Architecture:** `UnifiedProgramGraphQuery` reads one M12A `ProgramGraphSnapshot`, composes native M6 topology and selected provider projections without materializing a merged authority graph, and returns canonical members plus source-specific observations/facts. Fusion derives explicit corroboration, contradiction, refinement, and unresolved-identity records while preserving original epistemic states. M10B query-completion facts pin the exact provider run/projection set; M10C remains unchanged semantically and receives provider evidence only through the typed handoff.

**Tech Stack:** C++20, M6 `CpgQuery`, M9 FactStore/ProvenanceStore, M10B EvidenceQueryService/query-completion contracts, M10C EvidenceCaseBuilder/EIR serializers, M12A provider repositories/snapshots/dependencies, GoogleTest, CMake/CTest.

**Spec:** `docs/specs/milestones/m12-joern-cpg-summarydb-importer-design-spec.md`

## Global Constraints

- M10B, M10C, M12A, and M12B must be implemented and passing before this plan begins.
- Provider use is explicit. With no selected provider, native query and Evidence behavior is byte-for-byte unchanged.
- A snapshot pins ordered `{ProviderRunID, ProviderProjectionID}` bindings, capabilities, mapping versions, and assumption-set digests.
- Unified representation never means equal authority: canonical members, native observations, provider observations, facts, and provenance remain separately addressable.
- Fusion records never mutate or replace source facts and never promote `INFERRED` or `ASSUMED` to `MUST`.
- Imported absence is open-world and cannot satisfy a negative-evidence rule.
- A selected positive provider contradiction or unresolved in-scope candidate blocks an unqualified negative result.
- Truncated/partial provider state remains incomplete or unknown and propagates to query completion.
- M10B and M10C public boundaries expose no GraphSON, GraphML, Joern label/ID, TinkerPop, XML, JSON parser, JVM, or provider-native type.
- Provider deltas invalidate only dependent unified-query/Evidence cache entries and never schedule native summary/WPA work.
- Every new VERITAS-authored source, header, and CMake file carries the repository's Apache-2.0 SPDX header; C++ uses no RTTI or exceptions.

---

## File and Interface Map

| File | Responsibility |
| --- | --- |
| `include/veritas/provider/FusionTypes.h` | Canonical member observations, comparison kinds/status, completeness, and contradiction/refinement records. |
| `src/provider/FusionTypes.cpp` | Validation, canonical comparison IDs, ordering, and diagnostic text. |
| `include/veritas/provider/UnifiedProgramGraphQuery.h` | Provider-neutral pinned query surface from specification section 15.2. |
| `src/provider/UnifiedProgramGraphQuery.cpp` | Native/provider graph and fact reads, budgets, canonical aggregation, capabilities, assumptions, and comparison dispatch. |
| `include/veritas/evidence/EvidenceInputSnapshot.h` | M10B snapshot value containing native, fact, and selected provider bindings. |
| `src/evidence/EvidenceInputSnapshot.cpp` | Cross-layer snapshot validation and fingerprint formation. |
| `include/veritas/evidence/ProviderEvidenceDependencies.h` | Query/Evidence dependency registration against provider components and binding fingerprints. |
| `src/evidence/ProviderEvidenceDependencies.cpp` | Registration and stale checks using M12A `ProviderDependencyIndex`. |
| `include/veritas/evidence/SliceTypes.h` | Adds semantic provider observation/comparison/assumption/unknown references to M10B results. |
| `include/veritas/evidence/QueryCompletion.h` | Adds exact provider snapshot inputs to completion descriptors. |
| `src/evidence/EvidenceQueryService.cpp` | Executes M10B queries through the unified snapshot and enforces negative-evidence rules. |
| `src/tools/veritas-query.cpp` | Explicit `--providers native[,joern:<configuration-id>]` selection and diagnostics. |

## Public Interface Lock

```cpp
namespace veritas::provider {

enum class FusionStatus {
  kUnspecified,
  kNativeOnly,
  kExternalOnly,
  kCorroborated,
  kContradicted,
  kUnresolved,
};

enum class ComparisonKind {
  kUnspecified,
  kSameProgramEntity,
  kCorroborates,
  kContradicts,
  kRefines,
  kUnresolvedIdentity,
};

enum class UnifiedQueryCompleteness {
  kUnspecified,
  kComplete,
  kTruncated,
};

enum class UnifiedTruncationReason {
  kMaxDepth,
  kMaxNodes,
  kMaxPaths,
  kMaxFacts,
  kProviderPartial,
  kProviderUnknown,
};

struct UnifiedQueryBudget {
  std::size_t max_depth;
  std::size_t max_nodes;
  std::size_t max_paths;
  std::size_t max_facts;
};

struct RelationFilter {
  std::vector<ProgramRelationKind> relation_kinds;
  std::optional<core::StableId> source_ref;
  std::optional<core::StableId> target_ref;
};

struct CanonicalGraphMember {
  std::variant<ProgramEntity, ProgramRelation> semantic_member;
  bool observed_by_native = false;
  std::vector<core::StableId> provider_observation_ids;
  FusionStatus fusion_status = FusionStatus::kUnspecified;
};

struct ComparisonRecord {
  core::StableId comparison_id;
  ComparisonKind kind = ComparisonKind::kUnspecified;
  core::StableId canonical_subject_ref;
  core::StableId left_ref;
  core::StableId right_ref;
  std::vector<core::StableId> assumption_refs;
};

struct ProviderUnknown {
  core::StableId unknown_id;
  std::string kind;
  core::StableId scope_ref;
  std::vector<core::StableId> assumption_refs;
};

struct UnifiedQueryResult {
  ProgramGraphSnapshot snapshot;
  UnifiedQueryCompleteness completeness;
  std::vector<UnifiedTruncationReason> truncation_reasons;
  std::vector<CanonicalGraphMember> members;
  std::vector<ProviderObservation> provider_observations;
  std::vector<facts::AnalysisFact> facts;
  std::vector<ComparisonRecord> comparisons;
  std::vector<ProviderAssumption> assumptions;
  std::vector<ProviderUnknown> unknowns;
  std::size_t examined_items = 0;
};

class UnifiedProgramGraphQuery {
 public:
  static StatusOr<UnifiedProgramGraphQuery> OpenSnapshot(
      const std::string& db_path, ProgramGraphSnapshot snapshot);
  StatusOr<UnifiedQueryResult> GetEntity(core::StableId entity_id) const;
  StatusOr<UnifiedQueryResult> GetObservations(core::StableId canonical_id) const;
  StatusOr<UnifiedQueryResult> Traverse(RelationFilter filter,
                                        UnifiedQueryBudget budget) const;
  StatusOr<UnifiedQueryResult> GetCalls(core::StableId id,
                                        UnifiedQueryBudget budget) const;
  StatusOr<UnifiedQueryResult> GetValueFlow(core::StableId source,
                                            core::StableId sink,
                                            UnifiedQueryBudget budget) const;
  StatusOr<UnifiedQueryResult> GetMemoryAccesses(core::StableId memory_ref,
                                                 UnifiedQueryBudget budget) const;
  StatusOr<std::vector<ProviderCapability>> GetCapabilities(
      core::StableId provider_projection_id) const;
  StatusOr<std::vector<ProviderAssumption>> GetAssumptions(
      core::StableId observation_or_fact_id) const;
  StatusOr<std::vector<ComparisonRecord>> CompareProviders(
      core::StableId entity_or_relation_id) const;
};

}  // namespace veritas::provider
```

---

### Task 1: Fusion Value Types and Snapshot Read Consistency

**Files:**
- Create: `include/veritas/provider/FusionTypes.h`
- Create: `src/provider/FusionTypes.cpp`
- Create: `include/veritas/evidence/EvidenceInputSnapshot.h`
- Create: `src/evidence/EvidenceInputSnapshot.cpp`
- Modify: `src/provider/CMakeLists.txt`
- Modify: `src/evidence/CMakeLists.txt`
- Test: `tests/unit/provider/FusionTypesTest.cpp`
- Test: `tests/unit/evidence/EvidenceInputSnapshotTest.cpp`
- Modify: `tests/unit/provider/CMakeLists.txt`
- Modify: `tests/unit/evidence/CMakeLists.txt`

**Interfaces:**
- Consumes: M12A `ProgramGraphSnapshot`, provider bindings/capabilities/assumptions, and M10B completeness types.
- Produces: types in the public interface lock, including provider-owned query completeness/budget enums that avoid a provider/evidence library cycle, plus `EvidenceInputSnapshot`, `ValidateEvidenceInputSnapshot`, and deterministic diagnostic JSON.

- [ ] **Step 1: Write failing type/snapshot tests**

```cpp
TEST(EvidenceInputSnapshotTest, RejectsFingerprintThatOmitsProviderRun) {
  auto snapshot = ValidProviderSnapshot();
  snapshot.selected_provider_bindings.front().provider_run_id = RunId("other");
  EXPECT_EQ(ValidateEvidenceInputSnapshot(snapshot).code(),
            StatusCode::kDataLoss);
}

TEST(FusionTypesTest, ContradictionRetainsBothSources) {
  const auto record = Contradiction(NativeFact(), JoernFact());
  EXPECT_EQ(record.kind, ComparisonKind::kContradicts);
  EXPECT_EQ(record.left_ref, NativeFact().fact_id);
  EXPECT_EQ(record.right_ref, JoernFact().fact_id);
}
```

- [ ] **Step 2: Run the tests to verify failure**

Run: `ctest --test-dir build -R "FusionTypesTest|EvidenceInputSnapshotTest" --output-on-failure`

Expected: FAIL because the types do not exist.

- [ ] **Step 3: Define closed comparison and result types**

Require canonical left/right ordering, explicit native/provider source descriptors, original fact/observation refs, reason code, identity/capability/assumption refs, and no replacement epistemic field. Compute comparison ID from kind, canonical subject, source refs, and semantic qualifiers; exclude diagnostic prose.

- [ ] **Step 4: Define the exact M10B snapshot extension**

```cpp
struct EvidenceInputSnapshot {
  core::StableId repository_id;
  core::StableId revision_id;
  core::StableId build_variant_id;
  core::StableId native_projection_id;
  core::StableId fact_snapshot_id;
  std::vector<provider::ProviderBinding> selected_provider_bindings;
  core::SHA256Digest provider_binding_fingerprint;
  core::SHA256Digest input_snapshot_fingerprint;
};
```

Validate ID kinds, sorted unique provider bindings, capability/mapping/assumption digests, and recompute both fingerprints. `input_snapshot_fingerprint` hashes the complete native/fact/provider snapshot.

- [ ] **Step 5: Run the value-layer tests**

```bash
cmake --build --preset default
ctest --test-dir build -R "FusionTypesTest|EvidenceInputSnapshotTest" --output-on-failure
```

Expected: invalid ordering, missing run/projection, stale capability digest, contradiction closure, and deterministic fingerprint cases pass.

- [ ] **Step 6: Commit the value layer**

```bash
git add include/veritas/provider src/provider include/veritas/evidence src/evidence tests/unit/provider tests/unit/evidence
git commit -m "feat: define provider fusion snapshots"
```

---

### Task 2: Unified Native/Provider Query View

**Files:**
- Create: `include/veritas/provider/UnifiedProgramGraphQuery.h`
- Create: `src/provider/UnifiedProgramGraphQuery.cpp`
- Modify: `src/provider/CMakeLists.txt`
- Test: `tests/unit/provider/UnifiedProgramGraphQueryTest.cpp`
- Test: `tests/integration/provider/UnifiedProgramGraphQueryIntegrationTest.cpp`
- Modify: `tests/unit/provider/CMakeLists.txt`
- Modify: `tests/integration/provider/CMakeLists.txt`

**Interfaces:**
- Consumes: pinned snapshot, M6 `CpgRepository`/`CpgQuery`, M12A `ProviderGraphRepository`, M9 fact/provenance read snapshots, and query budgets.
- Produces: `UnifiedProgramGraphQuery` from the public interface lock.

- [ ] **Step 1: Write failing native-only/provider-only/budget tests**

```cpp
TEST_F(UnifiedProgramGraphQueryTest, NativeOnlyMatchesM6Exactly) {
  ASSERT_OK_AND_ASSIGN(const auto unified,
                       OpenNativeOnly().GetCalls(FunctionId(), Budget()));
  ASSERT_OK_AND_ASSIGN(const auto native,
                       NativeQuery().GetCallees(FunctionId()));
  EXPECT_EQ(MemberIds(unified), NodeIds(native));
  EXPECT_TRUE(unified.provider_observations.empty());
}
```

Also test provider-only inferred results, exact-boundary versus overflow, complete-empty versus truncated-empty, snapshot change during open, capabilities/assumptions/unknowns, and provider-native type absence at compile time.

- [ ] **Step 2: Run the query tests to verify failure**

Run: `ctest --test-dir build -R "UnifiedProgramGraphQueryTest|UnifiedProgramGraphQueryIntegrationTest" --output-on-failure`

Expected: FAIL because the unified view does not exist.

- [ ] **Step 3: Open every backend at one immutable snapshot**

Verify the requested native projection, fact snapshot, and each provider run/projection binding still exists and matches its pinned metadata. Use one SQLite read transaction for metadata/fact/provider rows and immutable M6/provider projection IDs for graph reads. Return stable `FailedPrecondition(\"program graph snapshot is stale; reopen\")` instead of mixing current state.

- [ ] **Step 4: Implement canonical aggregation and budgets**

Translate M6 native nodes/edges to provider-neutral query members at the read boundary without modifying M6 storage. Load selected provider members/observations and matching M9 facts; group by canonical entity/relation ID; sort by canonical ID/source/run/observation; apply limit-plus-one probing; and report completeness, reasons, and examined count. Never infer an entity bridge during query.

- [ ] **Step 5: Return provider metadata as first-class result state**

Attach snapshot, capabilities, assumptions, unknowns, facts, and provenance refs to every result. A missing provider relation is simply absent from members, never a negative row. Provider-only positive observations remain visible with `INFERRED`/`ASSUMED` origin.

- [ ] **Step 6: Run the unified-query tests**

```bash
cmake --build --preset default
ctest --test-dir build -R "UnifiedProgramGraphQueryTest|UnifiedProgramGraphQueryIntegrationTest" --output-on-failure
```

Expected: `FUS-001`, `FUS-002`, and provider-neutral result boundary tests pass.

- [ ] **Step 7: Commit the unified query**

```bash
git add include/veritas/provider/UnifiedProgramGraphQuery.h src/provider tests/unit/provider tests/integration/provider
git commit -m "feat: query native and provider graph snapshots"
```

---

### Task 3: Non-Destructive Provider Comparison and Fusion

**Files:**
- Create: `src/provider/ProviderFusion.cpp`
- Create: `src/provider/ProviderFusion.h`
- Modify: `src/provider/CMakeLists.txt`
- Test: `tests/unit/provider/ProviderFusionTest.cpp`
- Modify: `tests/unit/provider/CMakeLists.txt`

**Interfaces:**
- Consumes: canonical grouped members/observations/facts and registered comparison rules.
- Produces: closed `ComparisonRuleRegistry`, `CompareProviderObservations`, and `UnifiedProgramGraphQuery::CompareProviders`.

```cpp
StatusOr<std::vector<ComparisonRecord>> CompareProviderObservations(
    core::StableId canonical_id,
    std::span<const CanonicalGraphMember> members,
    std::span<const ProviderObservation> observations,
    std::span<const facts::AnalysisFact> facts,
    const ComparisonRuleRegistry& rules);
```

- [ ] **Step 1: Write failing corroboration/contradiction tests**

```cpp
TEST(ProviderFusionTest, CorroborationDoesNotPromoteExternalFact) {
  const auto result = Compare(NativeMayCall(), JoernMayCall());
  EXPECT_THAT(result.comparisons,
              Contains(ComparisonKindIs(ComparisonKind::kCorroborates)));
  EXPECT_EQ(result.external_fact.epistemic, EpistemicState::kInferred);
  EXPECT_EQ(result.native_fact.epistemic, NativeMayCall().epistemic);
}
```

Add contradiction, refinement, unresolved identity, multiple providers, same projection/different run, and stable ordering cases.

- [ ] **Step 2: Run the fusion test to verify failure**

Run: `ctest --test-dir build -R ProviderFusionTest --output-on-failure`

Expected: FAIL because comparison rules are absent.

- [ ] **Step 3: Implement registered comparison rules**

Create `SameProgramEntity` only from persisted exact identity resolution; `Corroborates` for equivalent canonical relation/fact semantics; `Contradicts` for registered mutually incompatible positive claims; `Refines` for a strictly more specific positive observation without incompatibility; and `UnresolvedIdentity` for ambiguous/external candidates. Keep both source refs and assumption/capability context.

- [ ] **Step 4: Enforce authority and epistemic invariants**

Comparison output is separate from facts and observations. It cannot modify current bindings, canonical `FactID`, epistemic state, confidence, or selected witness. Agreement among external providers is corroboration only. Contradictions produce `kContradicted` and remain visible to all consumers.

- [ ] **Step 5: Run the provider-fusion tests**

```bash
cmake --build --preset default
ctest --test-dir build -R ProviderFusionTest --output-on-failure
```

Expected: `FUS-003`–`FUS-005` pass.

- [ ] **Step 6: Commit provider fusion**

```bash
git add src/provider tests/unit/provider
git commit -m "feat: compare provider observations"
```

---

### Task 4: Provider-Aware M10B Query Completion and Handoff

**Files:**
- Modify: `include/veritas/evidence/SliceTypes.h`
- Modify: `include/veritas/evidence/QueryCompletion.h`
- Modify: `include/veritas/evidence/EvidenceQueryService.h`
- Modify: `src/evidence/SliceTypes.cpp`
- Modify: `src/evidence/QueryCompletion.cpp`
- Modify: `src/evidence/EvidenceQueryService.cpp`
- Modify: `src/evidence/CMakeLists.txt`
- Test: `tests/unit/evidence/ProviderEvidenceQueryServiceTest.cpp`
- Test: `tests/unit/evidence/ProviderEvidenceHandoffTest.cpp`
- Modify: `tests/unit/evidence/CMakeLists.txt`

**Interfaces:**
- Consumes: Tasks 1–3, M10B `QueryCompletionDescriptor`, `EvidenceBuildInput`, and `EvidenceQueryService`.
- Produces: provider-aware query result fields and `EvidenceQueryService::BuildEvidenceInput(..., ProviderSelection)`.

```cpp
StatusOr<EvidenceBuildInput> EvidenceQueryService::BuildEvidenceInput(
    ClaimSeed claim, EvidenceQueryBudget budget,
    provider::ProviderSelection selection) const;

QueryCompletionDescriptor MakeProviderAwareCompletion(
    QueryKind kind, const EvidenceInputSnapshot& snapshot,
    const provider::UnifiedQueryResult& result,
    const EvidenceQueryBudget& budget);
```

- [ ] **Step 1: Write failing completion/handoff tests**

```cpp
TEST(ProviderEvidenceHandoffTest, CompletionPinsExactProviderBinding) {
  ASSERT_OK_AND_ASSIGN(const auto input, BuildWithJoern());
  const auto& snapshot = input.snapshot;
  EXPECT_EQ(snapshot.selected_provider_bindings.front().provider_run_id,
            ImportedRunId());
  EXPECT_TRUE(AllCompletionFactsUse(
      input.query_completion_facts, snapshot.input_snapshot_fingerprint));
}
```

Also test capability/mapping/assumption mutation, binding change during assembly, provider IDs in provenance, provider-neutral public types, and distinct GraphSON/GraphML runs over one projection.

- [ ] **Step 2: Run the tests to verify failure**

Run: `ctest --test-dir build -R "ProviderEvidenceQueryServiceTest|ProviderEvidenceHandoffTest" --output-on-failure`

Expected: FAIL because M10B pins only native/fact state.

- [ ] **Step 3: Extend M10B semantic result types**

Add `EvidenceInputSnapshot snapshot` to `EvidenceBuildInput`. Add canonical provider observation refs, comparison refs, capabilities, assumptions, and unknowns to the relevant flow/fact result types. Do not add provider-native IDs/labels or parser types. Preserve supporting and contradicting facts as separate collections.

- [ ] **Step 4: Extend query-completion identity and witness closure**

Require every `QueryCompletionDescriptor.input_snapshot_fingerprint` to include selected provider bindings and metadata digests. Completion witnesses reference provider run/projection/capability/assumption IDs and the selected provider fact/observation provenance where used. Recompute and reject any descriptor omitting or changing a selected binding.

- [ ] **Step 5: Execute M10B queries through one unified snapshot**

Open `UnifiedProgramGraphQuery` once at the beginning of `BuildEvidenceInput`; run flow/range/capacity/alias/unknown/dominating-check reads against it; carry native/provider members and comparisons into the handoff; and return `evidence snapshot changed; retry` if exact bindings cannot remain pinned. Never retry one subquery against a newer provider binding.

- [ ] **Step 6: Run the M10B provider-integration tests**

```bash
cmake --build --preset default
ctest --test-dir build -R "ProviderEvidenceQueryServiceTest|ProviderEvidenceHandoffTest|EvidenceHandoffTest" --output-on-failure
```

Expected: `FUS-006`, `FUS-007`, `EVD-001`, and `EVD-002` pass.

- [ ] **Step 7: Commit M10B provider integration**

```bash
git add include/veritas/evidence src/evidence tests/unit/evidence
git commit -m "feat: carry provider state into evidence inputs"
```

---

### Task 5: Open-World Negative-Evidence and Completeness Guards

**Files:**
- Modify: `src/evidence/EvidenceQueryService.cpp`
- Modify: `src/evidence/QueryCompletion.cpp`
- Test: `tests/unit/evidence/ProviderNegativeEvidenceTest.cpp`
- Modify: `tests/unit/evidence/CMakeLists.txt`

**Interfaces:**
- Consumes: provider capabilities, unknowns, comparisons, positive provider candidates, and M10B completeness rules.
- Produces: `NegativeEvidenceEligibility EvaluateNegativeEvidence(const UnifiedQueryResult&)`.

```cpp
enum class NegativeEvidenceEligibility {
  kEligibleNativeClosedWorld,
  kOpenWorldProviderAbsence,
  kBlockedByContradiction,
  kIncompleteProvider,
  kUnresolvedProviderIdentity,
};

NegativeEvidenceEligibility EvaluateNegativeEvidence(
    const provider::UnifiedQueryResult& result);
```

- [ ] **Step 1: Write failing open-world cases**

```cpp
TEST(ProviderNegativeEvidenceTest, PositiveProviderCandidateBlocksNativeEmpty) {
  auto result = NativeCompleteEmptyDominatingChecks();
  AddJoernInferredDominatingCheck(result);
  EXPECT_EQ(EvaluateNegativeEvidence(result),
            NegativeEvidenceEligibility::kBlockedByContradiction);
}

TEST(ProviderNegativeEvidenceTest, MissingProviderEdgeNeverProvesAbsence) {
  const auto result = SelectedJoernCompleteWithoutReachingDef();
  EXPECT_NE(EvaluateNegativeEvidence(result),
            NegativeEvidenceEligibility::kClosedWorldComplete);
}
```

- [ ] **Step 2: Run the test to verify failure**

Run: `ctest --test-dir build -R ProviderNegativeEvidenceTest --output-on-failure`

Expected: FAIL because M10B does not yet account for selected provider state.

- [ ] **Step 3: Implement explicit eligibility states**

Return `kEligibleNativeClosedWorld` only when the native rule's existing closed-world requirements hold and every selected provider is irrelevant to the queried domain with no in-scope positive/unresolved candidate. Return `kOpenWorldProviderAbsence`, `kBlockedByContradiction`, `kIncompleteProvider`, or `kUnresolvedProviderIdentity` otherwise.

- [ ] **Step 4: Propagate incompleteness and unknowns**

Provider-side truncation, partial capability, unresolved identity, or incomplete witness adds stable query-completion reasons and typed unknown entries. A positive external contradiction remains supporting/contradicting evidence with its original epistemic state; it does not disappear into a generic incomplete flag.

- [ ] **Step 5: Run the negative-evidence tests**

```bash
cmake --build --preset default
ctest --test-dir build -R "ProviderNegativeEvidenceTest|EvidenceQueryServiceTest" --output-on-failure
```

Expected: `EVD-003`–`EVD-005` pass.

- [ ] **Step 6: Commit negative-evidence guards**

```bash
git add src/evidence tests/unit/evidence
git commit -m "fix: preserve open-world provider uncertainty"
```

---

### Task 6: Provider Query and Evidence Dependency Freshness

**Files:**
- Create: `include/veritas/evidence/ProviderEvidenceDependencies.h`
- Create: `src/evidence/ProviderEvidenceDependencies.cpp`
- Modify: `src/evidence/CMakeLists.txt`
- Test: `tests/integration/evidence/ProviderEvidenceFreshnessTest.cpp`
- Modify: `tests/integration/evidence/CMakeLists.txt`

**Interfaces:**
- Consumes: M12A `ProviderDependencyIndex`, query completion member/component refs, and Evidence input snapshot.
- Produces: `ProviderFreshness`, `RegisterProviderDependencies`, `CheckProviderFreshness`, and stale reasons `kProviderComponentChanged`/`kProviderBindingChanged`.

```cpp
Status RegisterProviderDependencies(
    const EvidenceBuildInput& input,
    summarydb::ProviderDependencyIndex& dependencies);
StatusOr<ProviderFreshness> CheckProviderFreshness(
    core::StableId evidence_or_query_id,
    const provider::ProgramGraphSnapshot& current,
    const summarydb::ProviderDependencyIndex& dependencies);
```

- [ ] **Step 1: Write failing selective-staleness tests**

```cpp
TEST_F(ProviderEvidenceFreshnessTest, SameProjectionNewRunStalesEvidenceOnly) {
  const auto evidence_key = BuildAndRegisterEvidence();
  const auto semantic_query_key = RegisterSemanticQuery();
  ASSERT_OK(ImportEquivalentGraphWithNewArtifact());
  EXPECT_TRUE(CheckProviderFreshness(evidence_key).is_stale);
  EXPECT_FALSE(CheckProviderFreshness(semantic_query_key).is_stale);
}
```

- [ ] **Step 2: Run the test to verify failure**

Run: `ctest --test-dir build -R ProviderEvidenceFreshnessTest --output-on-failure`

Expected: FAIL because M10B does not register provider components.

- [ ] **Step 3: Register exact dependencies**

For each query completion, register the provider projection/component/function digests actually read. For an `EvidenceBuildInput` and resulting `EvidenceCase`, additionally register the exact provider binding fingerprint because evidence provenance is run-specific.

- [ ] **Step 4: Implement stale checks without native scheduling**

Changed semantic component stales dependent unified queries and Evidence. Same projection/new run keeps semantic query content reusable but stales provenance-bearing Evidence. Unrelated component changes leave entries fresh. Assert that no M7 invalidation or WPA schedule row is created.

- [ ] **Step 5: Run the freshness tests**

```bash
cmake --build --preset default
ctest --test-dir build -R "ProviderEvidenceFreshnessTest|ProviderIncrementalPublicationTest" --output-on-failure
```

Expected: provider binding changes make old combined snapshots stale and invalidation remains scoped.

- [ ] **Step 6: Commit freshness integration**

```bash
git add include/veritas/evidence src/evidence tests/integration/evidence
git commit -m "feat: track provider evidence freshness"
```

---

### Task 7: Explicit Provider Selection in `veritas-query`

**Files:**
- Modify: `src/tools/veritas-query.cpp`
- Modify: `src/tools/CMakeLists.txt`
- Test: `tests/integration/evidence/VeritasQueryProviderSelectionTest.cpp`
- Modify: `tests/integration/evidence/CMakeLists.txt`
- Create: `tests/golden/evidence/provider-selection.txt`

**Interfaces:**
- Consumes: `ProviderSelection`, current provider binding resolver, unified query, and existing M10B/M10C CLI paths.
- Produces: `--providers native` and `--providers native,joern:<configuration-id>`.

```cpp
StatusOr<provider::ProviderSelection> ParseProviderSelection(
    std::string_view text,
    const core::StableId& repository_id,
    const core::StableId& revision_id,
    const core::StableId& build_variant_id);
```

- [ ] **Step 1: Write failing selection and stale-binding tests**

```cpp
TEST(VeritasQueryProviderSelectionTest, PinsConfiguredJoernPair) {
  const auto result = RunQuery(
      {"evidence", "--providers", "native,joern:default"});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_THAT(result.stdout_text, HasSubstr(ToString(CurrentJoernRunId())));
  EXPECT_THAT(result.stdout_text,
              HasSubstr(ToString(CurrentJoernProjectionId())));
}
```

Test default/native-only behavior, unknown provider/configuration, duplicates, deterministic ordering, provider binding change, and output of capabilities/assumptions/unknowns/comparisons.

- [ ] **Step 2: Run the CLI test to verify failure**

Run: `ctest --test-dir build -R VeritasQueryProviderSelectionTest --output-on-failure`

Expected: FAIL because provider selection is not parsed.

- [ ] **Step 3: Parse provider selection without guessing**

`native` is mandatory in V1 query syntax. Each `joern:<configuration-id>` resolves exactly one current binding under the query's repository/revision/build context; duplicates and absent/ambiguous configurations are `InvalidArgument` or `NotFound`. Resolve all bindings in one snapshot transaction and print the exact pair.

- [ ] **Step 4: Preserve native default output**

When `--providers` is omitted, use the deployment/query configuration's explicitly enabled set; for the V1 CLI default this is `native`. Ensure existing native-only golden JSON/EIR-T is byte-for-byte unchanged.

- [ ] **Step 5: Run the provider-selection tests**

```bash
cmake --build --preset default
ctest --test-dir build -R "VeritasQueryProviderSelectionTest|VeritasQueryEvidenceTest|VeritasQueryEirTest" --output-on-failure
```

Expected: `CLI-006`, `FUS-001`, `FUS-006`, and native default non-regression pass.

- [ ] **Step 6: Commit query selection**

```bash
git add src/tools tests/integration/evidence tests/golden/evidence
git commit -m "feat: select provider snapshots in queries"
```

---

### Task 8: Joern-to-Evidence-IR End-to-End Qualification

**Files:**
- Create: `tests/integration/evidence/JoernEvidenceIrIntegrationTest.cpp`
- Create: `tests/integration/evidence/M12cConformanceTest.cpp`
- Modify: `tests/integration/evidence/CMakeLists.txt`
- Create: `tests/golden/evidence/joern-corroborated.l1.eir`
- Create: `tests/golden/evidence/joern-contradicted.l1.eir`
- Create: `tests/golden/evidence/joern-incomplete.l1.eir`
- Modify: `docs/specs/milestones/m12-joern-cpg-summarydb-importer-design-spec.md`
- Modify: `docs/plans/README.md`

**Interfaces:**
- Consumes: M12B import, Tasks 1–7, M10B `EvidenceQueryService::BuildEvidenceInput`, and M10C `EvidenceCaseBuilder`/validator/EIR-T/Protobuf/full-JSON codecs.
- Produces: CTest label `m12c-provider-evidence` and the final M12A–M12C exit gate.

- [ ] **Step 1: Write the typed end-to-end cases before goldens**

Import equivalent GraphSON and GraphML snapshots over the buffer-overflow fixture. Build native-only, Joern-only positive, corroborated, contradicted, unresolved, and partial/truncated inputs. Assert exact snapshot bindings, completion facts, provider observations, comparison kinds, epistemic states, assumptions, unknowns, selected witnesses, and forbidden negative claims.

```cpp
TEST_F(JoernEvidenceIrIntegrationTest, ContradictionSurvivesToValidatedEir) {
  ASSERT_OK_AND_ASSIGN(const auto input, BuildContradictedJoernInput());
  EXPECT_THAT(input.comparisons,
              Contains(ComparisonKindIs(ComparisonKind::kContradicts)));
  EXPECT_FALSE(ContainsUnqualifiedNegativeCheckAbsence(input));
  ASSERT_OK_AND_ASSIGN(const auto evidence, EvidenceCaseBuilder::Build(input));
  EXPECT_OK(ValidateEvidenceCase(evidence));
  EXPECT_THAT(evidence.facts,
              Contains(EpistemicIs(EpistemicState::kInferred)));
}
```

- [ ] **Step 2: Pass only `EvidenceBuildInput` to M10C**

Call `EvidenceCaseBuilder::Build(input)`; do not pass a query object, provider repository, Joern record, or parser value. Validate the `EvidenceCase`, canonicalize it, compute `EvidenceID`, and round-trip Protobuf, EIR-T, and full-EIR JSON. Add a compile-time dependency test proving the M10C target does not link the Joern reader target.

- [ ] **Step 3: Add goldens after typed assertions pass**

Generate canonical EIR-T and diagnostic full-EIR JSON for corroborated, contradicted, and incomplete cases. Goldens must expose semantic provider facts, assumptions, unknowns, comparison status, and provenance refs while containing no Joern ordinal, GraphSON/GraphML object, host-absolute path, or parser-native type.

- [ ] **Step 4: Run all M12C and Evidence gates**

Run:

```bash
cmake --build --preset default
ctest --test-dir build -L m12c-provider-evidence --output-on-failure
ctest --test-dir build -L evidence-contract --output-on-failure
ctest --test-dir build -L evidence-ir --output-on-failure
```

Expected: no required case is missing, disabled, skipped, failed, or errored; `EVD-001`–`EVD-006` pass.

- [ ] **Step 5: Run the complete M12 and native non-regression gate**

Run:

```bash
ctest --test-dir build -L m12a-provider-substrate --output-on-failure
ctest --test-dir build -L m12b-joern-importer --output-on-failure
ctest --test-dir build -R "VeritasBuildAnalyze|CpgRepositoryTest|Wpa|Evidence" --output-on-failure
```

Expected: all pass; removing provider selection produces the existing native output.

- [ ] **Step 6: Record completion status**

Update the M12 spec and plan index only after all 104 stable acceptance IDs have exactly one registered owning test and all M12A–M12C exit criteria pass.

- [ ] **Step 7: Commit the M12C qualification**

```bash
git add docs tests/integration/evidence tests/golden/evidence
git commit -m "test: qualify provider evidence integration"
```

---

## M12C Registered Acceptance Ownership

Each stable case has one registered owning test; the final conformance test
composes those cases without registering duplicate IDs.

| Owning test | Stable acceptance cases |
| --- | --- |
| `UnifiedProgramGraphQueryTest` | `FUS-001`, `FUS-002` |
| `ProviderFusionTest` | `FUS-003`, `FUS-004`, `FUS-005` |
| `ProviderEvidenceHandoffTest` | `FUS-006`, `FUS-007`, `EVD-001`, `EVD-002` |
| `ProviderNegativeEvidenceTest` | `EVD-003`, `EVD-004`, `EVD-005` |
| `VeritasQueryProviderSelectionTest` | `CLI-006` |
| `JoernEvidenceIrIntegrationTest` | `EVD-006` |
| `M12cConformanceTest` | Full M12A–M12C/native non-regression gate; no duplicate stable ID registration |

M12C is complete only when native-only behavior is unchanged, selected Joern state is fully pinned and explainable, fusion preserves source authority, imported absence cannot become negative evidence, and every Joern-derived Evidence case reaches EIR exclusively through `EvidenceBuildInput`.
