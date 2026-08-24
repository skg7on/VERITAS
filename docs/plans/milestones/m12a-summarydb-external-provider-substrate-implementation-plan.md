# M12A SummaryDB External-Provider Substrate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the provider-neutral identities, graph model, validated external-fact batch, SummaryDB storage, atomic publication, history, and immutable snapshot primitives required by external analysis providers.

**Architecture:** M12A adds a parallel provider publication path above the existing M3/M6 storage foundations and future M9 fact/provenance stores. Immutable provider artifacts and normalized projections are stored independently from the native M6 `ThinCpg`; one SQLite visibility transaction publishes the provider graph, facts, witnesses, history, and current `{ProviderRunID, ProviderProjectionID}` binding. M12A contains no Joern parser and exposes no provider-native types.

**Tech Stack:** C++20, `veritas::Status`/`StatusOr<T>`, canonical SHA-256 stable IDs, RocksDB object storage, SQLite metadata/graph/fact storage, GoogleTest, CMake/CTest.

**Spec:** `docs/specs/milestones/m12-joern-cpg-summarydb-importer-design-spec.md`

## Global Constraints

- M2, M3, M6 storage foundations, M8R, and M9 must be implemented and passing before this plan begins.
- M6 `ThinCpg`, `ProjectionID`, `CpgRepository`, and native summary publication remain unchanged.
- Provider graph topology and provider observations are distinct from canonical M9 semantic facts.
- `AnalysisFactBatch` remains the only WPA input; external providers publish only through the separately typed `ExternalFactBatch`.
- External facts may be `INFERRED` or `ASSUMED`, never `MUST`.
- Provider publication is atomic and idempotent; a failed publication advances no provider binding and leaves native bindings untouched.
- Current provider identity is the pair `{ProviderRunID, ProviderProjectionID}` under `(repository, revision, build variant, provider, configuration)`.
- Equivalent semantic projections may share `ProviderProjectionID`; distinct artifacts and imports retain distinct run/provenance identity.
- Provider deltas invalidate only provider-query and Evidence dependencies; they never schedule native summary recomputation or WPA.
- Every new VERITAS-authored source, header, and CMake file carries the repository's Apache-2.0 SPDX header.
- C++ code uses no RTTI or exceptions.

---

## File and Interface Map

| File | Responsibility |
| --- | --- |
| `include/veritas/provider/ProviderTypes.h` | Closed provider, format, context, capability, artifact, run, projection, assumption, and diagnostic value types. |
| `src/provider/ProviderTypes.cpp` | Enum text conversion, validation, and canonical encodings for provider metadata. |
| `include/veritas/provider/ProviderProgramGraph.h` | Provider-neutral entities, relations, observations, extensions, component digests, and graph validation API. |
| `src/provider/ProviderProgramGraph.cpp` | Graph well-formedness, canonical ordering, component partitioning, and canonical ID formation. |
| `include/veritas/facts/ExternalFactBatch.h` | Typed external publication envelope and `ExternalFactObservation`. |
| `include/veritas/facts/FactPublicationValidator.h` | Shared validation overloads for M8R `AnalysisFactBatch` and M12 `ExternalFactBatch`. |
| `src/facts/FactPublicationValidator.cpp` | Schema, completion, witness-closure, run-binding, epistemic-floor, and batch-ID validation. |
| `include/veritas/summarydb/ProviderGraphRepository.h` | Historical/current provider graph persistence and read APIs. |
| `src/summarydb/ProviderGraphRepository.cpp` | SQLite graph/index/history implementation over the shared `MetadataStore`. |
| `include/veritas/provider/ProviderPublicationCoordinator.h` | Atomic provider publication request/result and coordinator. |
| `src/provider/ProviderPublicationCoordinator.cpp` | Immutable object writes followed by one provider visibility transaction. |
| `include/veritas/provider/ProgramGraphSnapshot.h` | Pinned native/fact/provider snapshot and selection types. |
| `src/provider/ProgramGraphSnapshot.cpp` | Provider binding resolution and snapshot fingerprinting. |
| `include/veritas/summarydb/ProviderDependencyIndex.h` | Provider-component dependency registration, deltas, and stale markers. |
| `src/summarydb/ProviderDependencyIndex.cpp` | Provider-only dependency and invalidation implementation. |

## Public Interface Lock

Implement tasks against these signatures; later plans consume them unchanged:

```cpp
namespace veritas::provider {

enum class ProviderKind { kUnspecified, kJoern };
enum class ProviderFormat { kUnspecified, kGraphson, kGraphml };
enum class ContextBindingBasis {
  kUnspecified,
  kVerified,
  kSourceVerifiedBuildAsserted,
  kUserAsserted,
};

struct ProviderBindingKey {
  core::StableId repository_id;
  core::StableId revision_id;
  core::StableId build_variant_id;
  ProviderKind provider = ProviderKind::kUnspecified;
  core::SHA256Digest provider_configuration_digest;
  auto operator<=>(const ProviderBindingKey&) const = default;
};

struct ProviderBinding {
  core::StableId provider_run_id;
  core::StableId provider_projection_id;
  core::SHA256Digest capability_digest;
  std::string mapping_version;
  core::SHA256Digest assumption_set_digest;
  auto operator<=>(const ProviderBinding&) const = default;
};

struct ProviderPublication {
  ProviderArtifact artifact;
  std::optional<std::string> retained_raw_bytes;
  ProviderRun run;
  ProviderProgramGraph graph;
  facts::ExternalFactBatch facts;
};

struct ProviderPublicationResult {
  core::StableId provider_artifact_id;
  core::StableId provider_run_id;
  core::StableId provider_projection_id;
  ProviderComponentDelta delta;
  bool current_binding_advanced = false;
};

class ProviderPublicationCoordinator {
 public:
  static StatusOr<std::unique_ptr<ProviderPublicationCoordinator>> Open(
      const std::string& db_path);
  StatusOr<ProviderPublicationResult> Publish(ProviderPublication publication);
};

}  // namespace veritas::provider
```

---

### Task 1: Provider Metadata, Stable-ID Kinds, and Canonical Values

**Files:**
- Modify: `include/veritas/core/Ids.h`
- Modify: `src/core/Ids.cpp`
- Create: `include/veritas/provider/ProviderTypes.h`
- Create: `src/provider/ProviderTypes.cpp`
- Create: `src/provider/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Test: `tests/unit/provider/ProviderTypesTest.cpp`
- Create: `tests/unit/provider/CMakeLists.txt`
- Modify: `tests/unit/CMakeLists.txt`
- Create: `tests/support/provider/ProviderTestSupport.h`

**Interfaces:**
- Consumes: `core::MakeStableId(IdKind, std::span<const std::byte>)` and `core::CanonicalValue`.
- Produces: `ProviderArtifact`, `ProviderContextBinding`, `ProviderCapability`, `ProviderRun`, `ProviderProjectionIdentityInput`, `ProviderProjection`, `ProviderComponentDigest`, `ProviderAssumption`, `ProviderBindingKey`, `ProviderBinding`, and their validation/canonical-byte functions.
- Produces: new `IdKind` members `kProviderArtifact`, `kProviderRawGraph`, `kProviderRecord`, `kProviderRun`, `kProviderProjection`, `kProviderObservation`, `kProviderExtensionObservation`, `kProviderAssumption`, `kProgramOccurrence`, `kProgramEntity`, and `kProgramRelation`.
- Produces: shared provider test assertions `ASSERT_OK`, `EXPECT_OK`, and `ASSERT_OK_AND_ASSIGN`, used by all M12 plans.

```cpp
#define VERITAS_TEST_CONCAT_INNER(x, y) x##y
#define VERITAS_TEST_CONCAT(x, y) VERITAS_TEST_CONCAT_INNER(x, y)
#define ASSERT_OK(expr) ASSERT_TRUE((expr).ok())
#define EXPECT_OK(expr) EXPECT_TRUE((expr).ok())
#define ASSERT_OK_AND_ASSIGN(lhs, expr)                                      \
  auto VERITAS_TEST_CONCAT(status_or_, __LINE__) = (expr);                   \
  ASSERT_TRUE(VERITAS_TEST_CONCAT(status_or_, __LINE__).ok());               \
  lhs = std::move(*VERITAS_TEST_CONCAT(status_or_, __LINE__))
```

- [ ] **Step 1: Write failing canonical identity tests**

```cpp
TEST(ProviderTypesTest, ArtifactIdentityUsesExactBytesButNotPath) {
  const auto left = MakeArtifact(ProviderFormat::kGraphson, "{\"x\":1}",
                                 "/host/a/graph.json");
  const auto right = MakeArtifact(ProviderFormat::kGraphson, "{\"x\":1}",
                                  "/host/b/renamed.bin");
  EXPECT_EQ(left.artifact_id, right.artifact_id);
  EXPECT_NE(left.diagnostic_origin_path, right.diagnostic_origin_path);
}

TEST(ProviderTypesTest, RunIdentityIncludesArtifactAndContextAssumptions) {
  const auto base = ValidRun();
  auto changed = base;
  changed.context_binding.basis = ContextBindingBasis::kUserAsserted;
  changed.context_binding.assumption_ids = {AssumptionId("context")};
  EXPECT_NE(ProviderRunId(base), ProviderRunId(changed));
}
```

- [ ] **Step 2: Run the test to verify the contract is absent**

Run:

```bash
cmake --preset default
cmake --build --preset default
ctest --test-dir build -R ProviderTypesTest --output-on-failure
```

Expected: FAIL because `veritas/provider/ProviderTypes.h` and provider ID kinds do not exist.

- [ ] **Step 3: Add closed value types and rejecting text conversions**

Define invalid sentinels for every enum. `Validate(ProviderContextBinding)` rejects wrong ID kinds, missing required fingerprints, missing assumptions for asserted dimensions, and assumptions on a fully verified binding. `Validate(ProviderRun)` requires artifact/raw-graph/context/schema/frontend/importer/mapping/configuration/capability fields while allowing `joern_version` to be explicitly unknown.

```cpp
Status Validate(const ProviderContextBinding& binding);
Status Validate(const ProviderCapability& capability);
Status Validate(const ProviderRun& run);
Status Validate(const ProviderProjectionIdentityInput& input);
Status Validate(const ProviderProjection& projection);
core::StableId ProviderArtifactId(ProviderKind, ProviderFormat,
                                  std::span<const std::byte> exact_bytes);
core::StableId ProviderRunId(const ProviderRun& run);
core::StableId ProviderProjectionId(
    const ProviderProjectionIdentityInput& identity,
    const core::SHA256Digest& canonical_graph_digest,
    std::span<const ProviderComponentDigest> component_digests);
```

- [ ] **Step 4: Implement artifact/run IDs and declare the projection formula**

Implement `provider.artifact.v1` and `provider.run.v1` from specification section 6. Declare the `provider.projection.v1` function for Task 2, which supplies canonical graph/component digests. Exclude lifecycle status, diagnostics, diagnostic paths, and raw blob retention from all identities. Add round-trip `IdKindToString`/`ParseStableId` tests for every new kind.

- [ ] **Step 5: Run the focused tests**

Run:

```bash
cmake --build --preset default
ctest --test-dir build -R "IdsTest|ProviderTypesTest" --output-on-failure
```

Expected: PASS, including `SCH-012`, `SCH-013`, and the identity-field exclusion assertions.

- [ ] **Step 6: Commit the provider identity layer**

```bash
git add CMakeLists.txt include/veritas/core/Ids.h src/core/Ids.cpp include/veritas/provider/ProviderTypes.h src/provider tests/support/provider tests/unit/provider tests/unit/CMakeLists.txt
git commit -m "feat: define external provider identities"
```

---

### Task 2: Provider-Neutral Graph and Canonical Projection

**Files:**
- Create: `include/veritas/provider/ProviderProgramGraph.h`
- Create: `src/provider/ProviderProgramGraph.cpp`
- Modify: `src/provider/CMakeLists.txt`
- Test: `tests/unit/provider/ProviderProgramGraphTest.cpp`
- Test: `tests/unit/provider/ProviderGraphCanonicalizerTest.cpp`
- Modify: `tests/unit/provider/CMakeLists.txt`

**Interfaces:**
- Consumes: validated provider metadata and new stable-ID kinds from Task 1.
- Produces: `ProgramEntityKind`, `ProgramRelationKind`, `ProgramEntity`, `ProgramRelation`, `ProviderObservation`, `ProviderExtensionObservation`, `ProviderComponentDelta`, `ProviderProgramGraphInput`, `ProviderProgramGraph`, `ValidateProviderProgramGraph`, `CanonicalProviderGraphBytes`, `ProviderProjectionId`, and `CanonicalizeProviderProgramGraph`.
- Produces: `ExternalEntityId(provider, raw_graph_id, typed_record_id)`, `ProgramOccurrenceId`, and provider-independent `ProgramRelationId`.

- [ ] **Step 1: Write failing graph-invariant tests**

```cpp
TEST(ProviderProgramGraphTest, RejectsDanglingRelationEndpoint) {
  auto graph = MinimalProviderGraph();
  graph.relations.front().target_ref = EntityId("missing");
  EXPECT_EQ(ValidateProviderProgramGraph(graph).code(),
            StatusCode::kDataLoss);
}

TEST(ProviderGraphCanonicalizerTest, ObservationRunDoesNotChangeProjection) {
  auto first = MinimalProviderGraph();
  auto second = first;
  second.observations.front().provider_run_id = ProviderRunId("other-run");
  EXPECT_EQ(CanonicalizeProviderProgramGraph(first)->metadata.provider_projection_id,
            CanonicalizeProviderProgramGraph(second)->metadata.provider_projection_id);
}
```

- [ ] **Step 2: Run the focused tests and observe failure**

Run: `ctest --test-dir build -R "ProviderProgramGraphTest|ProviderGraphCanonicalizerTest" --output-on-failure`

Expected: FAIL because the graph model is absent.

- [ ] **Step 3: Define the closed graph model**

Implement the section 8 entity/relation allowlists, `ProviderObservation`, inert `ProviderExtensionObservation`, and a typed canonical property value restricted to null, boolean, signed/unsigned integer, finite double, UTF-8 string, stable ID, and recursively budgeted lists/maps. Keep provider record IDs and locators only in observations/extensions.

```cpp
struct ProviderProgramGraphInput {
  ProviderProjectionIdentityInput identity;
  std::vector<ProgramEntity> entities;
  std::vector<ProgramRelation> relations;
  std::vector<ProviderObservation> observations;
  std::vector<ProviderExtensionObservation> extensions;
};

struct ProviderProgramGraph {
  ProviderProjection metadata;
  std::vector<ProgramEntity> entities;
  std::vector<ProgramRelation> relations;
  std::vector<ProviderObservation> observations;
  std::vector<ProviderExtensionObservation> extensions;
  std::vector<ProviderComponentDigest> component_digests;
};

StatusOr<ProviderProgramGraph> CanonicalizeProviderProgramGraph(
    ProviderProgramGraphInput input);
```

- [ ] **Step 4: Implement graph validation and deterministic canonicalization**

Sort and deduplicate entities/relations by canonical ID; require byte-equivalent duplicates; validate endpoint existence, ID kind, occurrence/entity separation, observation reference closure, and extension inertness. Compute component digests for `syntax`, `control_flow`, `calls`, `references`, `def_use`, `control_dependence`, `types`, `memory_access`, `capabilities`, and `assumptions`, with per-function digests where ownership is resolved.

- [ ] **Step 5: Prove identity boundaries**

Add cases for `ID-001`, `ID-007`, `ID-008`, `ID-009`, `ID-010`, and `NRM-011`. Mutate provider ordinals, absolute host roots, raw code formatting, record locators, observation run IDs, and extension payloads; only explicitly provider-local IDs or run-specific evidence digests may change.

- [ ] **Step 6: Run the graph-layer tests**

```bash
cmake --build --preset default
ctest --test-dir build -R "ProviderProgramGraphTest|ProviderGraphCanonicalizerTest" --output-on-failure
```

Expected: both suites pass.

- [ ] **Step 7: Commit the graph layer**

```bash
git add include/veritas/provider/ProviderProgramGraph.h src/provider tests/unit/provider
git commit -m "feat: add canonical provider program graph"
```

---

### Task 3: Typed External Fact Batch and Shared Publication Validator

**Files:**
- Create: `include/veritas/facts/ExternalFactBatch.h`
- Create: `src/facts/ExternalFactBatch.cpp`
- Create: `include/veritas/facts/FactPublicationValidator.h`
- Create: `src/facts/FactPublicationValidator.cpp`
- Modify: `src/facts/CMakeLists.txt`
- Test: `tests/unit/facts/ExternalFactBatchTest.cpp`
- Test: `tests/unit/facts/FactPublicationValidatorTest.cpp`
- Modify: `tests/unit/facts/CMakeLists.txt`

**Interfaces:**
- Consumes: M8R `AnalysisFactBatch`, M9 `AnalysisFact`, `RunFactBinding`, `FactWitness`, `FactWitnessEdge`, relation registry, and Task 1 provider IDs.
- Produces: `ExternalFactObservation`, `ExternalComponentKey`, `ExternalComponentCompletion`, `ExternalFactBatch`, `ExternalBatchId`, and overloads `FactPublicationValidator::Validate(const AnalysisFactBatch&)` and `Validate(const ExternalFactBatch&)`.

- [ ] **Step 1: Write failing admission tests**

```cpp
TEST(FactPublicationValidatorTest, RejectsExternalMust) {
  auto batch = ValidExternalFactBatch();
  batch.canonical_facts.front().epistemic = EpistemicState::kMust;
  EXPECT_EQ(FactPublicationValidator::Validate(batch).code(),
            StatusCode::kFailedPrecondition);
}

TEST(FactPublicationValidatorTest, RejectsUnclosedWitnessLeaf) {
  auto batch = ValidExternalFactBatch();
  batch.witness_edges.push_back(UnknownLeafEdge());
  EXPECT_EQ(FactPublicationValidator::Validate(batch).code(),
            StatusCode::kDataLoss);
}
```

- [ ] **Step 2: Run the tests to verify failure**

Run: `ctest --test-dir build -R "ExternalFactBatchTest|FactPublicationValidatorTest" --output-on-failure`

Expected: FAIL because the typed provider batch and validator overload do not exist.

- [ ] **Step 3: Define the immutable external envelope**

```cpp
struct ExternalFactBatch {
  core::StableId provider_run_id;
  core::StableId batch_id;
  core::StableId provider_projection_id;
  std::vector<ExternalComponentKey> expected_components;
  std::vector<ExternalComponentCompletion> completed_components;
  std::vector<core::StableId> artifact_root_ids;
  std::vector<AnalysisFact> canonical_facts;
  std::vector<RunFactBinding> run_bindings;
  std::vector<FactWitness> witnesses;
  std::vector<FactWitnessEdge> witness_edges;
  std::vector<provider::ProviderAssumption> assumptions;
  std::vector<provider::ProviderDiagnostic> diagnostics;
};
```

Canonicalize every collection before `ExternalBatchId`; exclude diagnostics. Keep provider/run/witness fields outside canonical `FactID`.

- [ ] **Step 4: Implement common and external-only validation**

Factor schema, stable-ID, witness DAG, closure, selected-witness, binding, and canonical ordering checks into private common helpers. For external batches additionally require expected/completed set equality, correct run/projection IDs, artifact roots, every binding to name a fact in the batch or existing store, context assumptions inherited by every imported fact witness, and no `MUST` fact. Do not add a `PublishFacts(std::vector<ExternalFact>)` overload.

- [ ] **Step 5: Cover semantic admission cases**

Implement typed cases `FCT-001` through `FCT-011`: relation registration, structural-only omission, rooted closure, completion mismatch, MUST rejection, `MayCall`/`INFERRED` separation, explicit assumptions/unknowns, record-digest explanation reachability, lack of raw-vector bypass, and context-assumption inheritance.

- [ ] **Step 6: Run the admission-boundary tests**

```bash
cmake --build --preset default
ctest --test-dir build -R "ExternalFactBatchTest|FactPublicationValidatorTest" --output-on-failure
```

Expected: all `FCT` cases owned by M12A pass.

- [ ] **Step 7: Commit the admission boundary**

```bash
git add include/veritas/facts src/facts tests/unit/facts
git commit -m "feat: validate external fact batches"
```

---

### Task 4: SummaryDB Provider Schema and Historical Graph Repository

**Files:**
- Modify: `src/summarydb/schema/v1.sql`
- Create: `include/veritas/summarydb/ProviderGraphRepository.h`
- Create: `src/summarydb/ProviderGraphRepository.cpp`
- Create: `include/veritas/summarydb/ProviderDependencyIndex.h`
- Create: `src/summarydb/ProviderDependencyIndex.cpp`
- Modify: `src/summarydb/CMakeLists.txt`
- Test: `tests/unit/summarydb/ProviderGraphRepositoryTest.cpp`
- Test: `tests/unit/summarydb/ProviderDependencyIndexTest.cpp`
- Modify: `tests/unit/summarydb/CMakeLists.txt`

**Interfaces:**
- Consumes: shared `MetadataStore`, provider types, and validated canonical provider graph.
- Produces: `StageProjection`, `LoadProjection`, `LoadRun`, `LoadArtifact`, `ResolveCurrentBinding`, `ListObservations`, `LookupNormalizedRef`, `Outgoing`, `Incoming`, and the provider-only `ProviderDependencyIndex` storage API used by Task 5.
- Assumes: caller owns an active `MetadataStore` transaction for all `Stage*` methods.

```cpp
class ProviderGraphRepository {
 public:
  explicit ProviderGraphRepository(MetadataStore& metadata_store);
  Status StageArtifact(const provider::ProviderArtifact& artifact,
                       std::optional<std::string> retained_blob_ref);
  Status StageProjection(const provider::ProviderRun& run,
                         const provider::ProviderProgramGraph& graph);
  Status StageCurrentBinding(const provider::ProviderBindingKey& key,
                             const provider::ProviderBinding& binding);
  StatusOr<provider::ProviderProgramGraph> LoadProjection(
      core::StableId projection_id) const;
  StatusOr<provider::ProviderBinding> ResolveCurrentBinding(
      const provider::ProviderBindingKey& key) const;
};
```

- [ ] **Step 1: Write failing storage/history/index tests**

```cpp
TEST_F(ProviderGraphRepositoryTest, ResolvesExactRunProjectionPair) {
  ASSERT_OK(StageAndCommit(FirstPublication()));
  ASSERT_OK(StageAndCommit(SecondRunSameProjection()));
  ASSERT_OK_AND_ASSIGN(const auto current,
                       repository_->ResolveCurrentBinding(BindingKey()));
  EXPECT_EQ(current.provider_run_id, SecondRunId());
  EXPECT_EQ(current.provider_projection_id, SharedProjectionId());
  EXPECT_OK(repository_->LoadRun(FirstRunId()).status());
}
```

- [ ] **Step 2: Run the repository test to verify failure**

Run: `ctest --test-dir build -R ProviderGraphRepositoryTest --output-on-failure`

Expected: FAIL because provider tables and repository APIs are absent.

- [ ] **Step 3: Add normalized provider tables and indexes**

Add foreign-keyed tables for artifacts, retained-blob refs, runs, capabilities, assumptions, projections, component digests, nodes, relations, observations, extensions, record-to-canonical mappings, current bindings, and binding history. Add the six indexes in specification section 14.2. Store typed canonical bytes or stable typed columns; do not serialize C++ structs as diagnostic JSON.

- [ ] **Step 4: Implement staged writes and historical reads**

`StageProjection` validates that the transaction is active, inserts immutable rows idempotently, verifies equality on conflicts, records the run-to-projection binding, and stages the current-binding swap last. `LoadProjection` returns canonical semantic graph content; run-specific observations and extensions are loaded by run ID.

- [ ] **Step 5: Prove current/history and native isolation**

Test distinct configurations, same projection/different runs, historical reads, outgoing/incoming adjacency, record lookup, and rollback. Capture native `current_summaries` and M6 `cpg_current` rows before provider staging and assert byte-for-byte equality afterward (`PUB-005`, `PUB-008`, `PUB-009`).

- [ ] **Step 6: Run the provider-storage tests**

```bash
cmake --build --preset default
ctest --test-dir build -R "MetadataStoreTest|ProviderGraphRepositoryTest" --output-on-failure
```

Expected: schema migration, graph index, history, and native-isolation tests pass.

- [ ] **Step 7: Commit provider storage**

```bash
git add src/summarydb/schema/v1.sql include/veritas/summarydb src/summarydb tests/unit/summarydb
git commit -m "feat: store provider graph history in summarydb"
```

---

### Task 5: Atomic Provider Publication Coordinator

**Files:**
- Create: `include/veritas/provider/ProviderPublicationCoordinator.h`
- Create: `src/provider/ProviderPublicationCoordinator.cpp`
- Modify: `src/provider/CMakeLists.txt`
- Test: `tests/integration/provider/ProviderPublicationCoordinatorTest.cpp`
- Create: `tests/integration/provider/CMakeLists.txt`
- Modify: `tests/integration/CMakeLists.txt`

**Interfaces:**
- Consumes: M3 `ObjectStore`, shared `MetadataStore`, Task 3 validator, Task 4 repository, and M9 `FactStore`/`ProvenanceStore` staged publication APIs.
- Produces: `ProviderPublicationCoordinator::Open` and `Publish` from the public interface lock.

- [ ] **Step 1: Write failing atomicity and idempotency tests**

```cpp
TEST(ProviderPublicationCoordinatorTest, FactFailureRollsBackGraphAndBinding) {
  ASSERT_OK(Publish(BaselinePublication()).status());
  fault_injector_->FailAt(PublicationStep::kFactBinding);
  EXPECT_FALSE(Publish(ChangedPublication()).ok());
  EXPECT_EQ(CurrentBinding(), BaselineBinding());
  EXPECT_FALSE(GraphExists(ChangedProjectionId()));
  EXPECT_FALSE(FactRunExists(ChangedRunId()));
}
```

- [ ] **Step 2: Run the integration test to verify failure**

Run: `ctest --test-dir build -R ProviderPublicationCoordinatorTest --output-on-failure`

Expected: FAIL because the coordinator does not exist.

- [ ] **Step 3: Implement immutable writes followed by one visibility transaction**

Validate the entire graph and fact batch before storage. Put artifact descriptor, optional raw blob, and canonical extension payload objects first with content-addressed `PutIfAbsent`. Then call `BeginTransaction`, stage run/projection/graph, fact bindings, witnesses, assumptions, dependency deltas, history, and the current binding, and finally commit. On any staged or commit failure call rollback and return the original status.

- [ ] **Step 4: Implement idempotent and equivalent-projection behavior**

An identical artifact/context/configuration/mapping re-import returns the existing result with `current_binding_advanced=false`. A distinct artifact/run with the same normalized projection inserts new artifact/run/provenance and advances the pair binding without duplicating projection rows (`PUB-006`, `PUB-007`). Artifact descriptors remain durable whether the raw blob is retained (`PUB-012`).

- [ ] **Step 5: Exercise every publication fault boundary**

Inject failures at graph insertion, fact insertion, witness insertion, dependency insertion, binding swap, and commit. Assert no partial provider visibility and retention of the prior binding (`PUB-003`, `PUB-004`, `PUB-005`). Assert native current summary and CPG bindings never change (`PUB-009`).

- [ ] **Step 6: Run the atomic-publication tests**

```bash
cmake --build --preset default
ctest --test-dir build -R ProviderPublicationCoordinatorTest --output-on-failure
```

Expected: all injected failure and idempotency cases pass.

- [ ] **Step 7: Commit atomic publication**

```bash
git add include/veritas/provider src/provider tests/integration/provider tests/integration/CMakeLists.txt
git commit -m "feat: publish provider state atomically"
```

---

### Task 6: Immutable Program-Graph Snapshot and Provider Binding Fingerprint

**Files:**
- Create: `include/veritas/provider/ProgramGraphSnapshot.h`
- Create: `src/provider/ProgramGraphSnapshot.cpp`
- Modify: `src/provider/CMakeLists.txt`
- Test: `tests/unit/provider/ProgramGraphSnapshotTest.cpp`
- Modify: `tests/unit/provider/CMakeLists.txt`

**Interfaces:**
- Consumes: M6 current `ProjectionID`, M9 fact snapshot ID, and Task 4 current provider bindings.
- Produces: `ProviderSelection`, `ProgramGraphSnapshot`, `ProviderBindingFingerprint`, and `OpenProgramGraphSnapshot`.

- [ ] **Step 1: Write failing snapshot determinism tests**

```cpp
TEST(ProgramGraphSnapshotTest, FingerprintPinsRunAndProjection) {
  const auto first = SnapshotWith({Binding(RunId("a"), ProjectionId("p"))});
  const auto second = SnapshotWith({Binding(RunId("b"), ProjectionId("p"))});
  EXPECT_NE(first.provider_binding_fingerprint,
            second.provider_binding_fingerprint);
}
```

- [ ] **Step 2: Run the test to verify failure**

Run: `ctest --test-dir build -R ProgramGraphSnapshotTest --output-on-failure`

Expected: FAIL because provider-aware snapshots are absent.

- [ ] **Step 3: Define exact selection and snapshot types**

```cpp
struct ProviderSelection {
  bool include_native = true;
  std::vector<ProviderBindingKey> enabled_provider_configurations;
};

struct ProgramGraphSnapshot {
  core::StableId repository_id;
  core::StableId revision_id;
  core::StableId build_variant_id;
  core::StableId native_projection_id;
  core::StableId fact_snapshot_id;
  std::vector<ProviderBinding> ordered_provider_bindings;
  core::SHA256Digest provider_binding_fingerprint;
};
```

Sort selections by provider/configuration, reject duplicates and unspecified providers, and resolve every requested binding in one SQLite read transaction.

- [ ] **Step 4: Implement the fingerprint exactly**

Hash the ordered provider selection, run IDs, projection IDs, capability digests, mapping versions, and assumption-set digests. A provider projection equality with different run provenance must change the fingerprint. Empty provider selection has one stable native-only fingerprint.

- [ ] **Step 5: Run the snapshot tests**

```bash
cmake --build --preset default
ctest --test-dir build -R ProgramGraphSnapshotTest --output-on-failure
```

Expected: ordering, duplicate selection, missing binding, native-only, and run/projection fingerprint cases pass.

- [ ] **Step 6: Commit snapshot primitives**

```bash
git add include/veritas/provider/ProgramGraphSnapshot.h src/provider tests/unit/provider
git commit -m "feat: pin provider graph snapshots"
```

---

### Task 7: Provider Component Deltas and Scoped Invalidation Integration

**Files:**
- Modify: `include/veritas/summarydb/ProviderDependencyIndex.h`
- Modify: `src/summarydb/ProviderDependencyIndex.cpp`
- Modify: `src/summarydb/CMakeLists.txt`
- Test: `tests/integration/provider/ProviderIncrementalPublicationTest.cpp`
- Modify: `tests/integration/provider/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 2 component digests/delta type, Task 4 `ProviderDependencyIndex`, M7 dependency-index conventions, and Task 5 publication transaction.
- Produces: provider-component delta calculation and scoped publication invalidation through `RegisterConsumer`, `ComputeDelta`, `StageStaleConsumers`, and `ListStaleConsumers`.

```cpp
class ProviderDependencyIndex {
 public:
  Status RegisterConsumer(const ProviderComponentKey& component,
                          const ProviderConsumerKey& consumer);
  StatusOr<provider::ProviderComponentDelta> ComputeDelta(
      core::StableId old_projection_id,
      core::StableId new_projection_id) const;
  Status StageStaleConsumers(const provider::ProviderComponentDelta& delta,
                             const provider::ProviderBinding& binding);
  StatusOr<std::vector<ProviderConsumerKey>> ListStaleConsumers() const;
};
```

- [ ] **Step 1: Write failing scoped invalidation tests**

```cpp
TEST(ProviderDependencyIndexTest, DefUseChangeInvalidatesOnlyItsConsumers) {
  RegisterEvidence("calls-case", Component("calls"));
  RegisterEvidence("flow-case", Component("def_use"));
  ASSERT_OK(StageStaleConsumers(DeltaOnly("def_use")));
  EXPECT_THAT(ListStaleConsumers(), ElementsAre("flow-case"));
}
```

- [ ] **Step 2: Run the tests to verify failure**

Run: `ctest --test-dir build -R "ProviderDependencyIndexTest|ProviderIncrementalPublicationTest" --output-on-failure`

Expected: FAIL because provider component dependencies are absent.

- [ ] **Step 3: Add provider-only dependency keys and history deltas**

Key dependencies by provider binding key, provider projection ID, component name, optional function owner, and component digest. Reuse the `DependencyIndex` transactional pattern but use distinct provider tables and consumer kinds `query_cache` and `evidence_cache` only.

- [ ] **Step 4: Enforce semantic-versus-provenance freshness**

Unchanged semantic components reuse semantic query entries. A different provider run over the same projection marks provenance-bearing Evidence entries stale because the snapshot fingerprint changed. A changed component invalidates only registered consumers. No provider delta inserts M7 summary invalidations, scheduler work, or WPA component work (`PUB-010`, `PUB-011`).

- [ ] **Step 5: Run the scoped-invalidation tests**

```bash
cmake --build --preset default
ctest --test-dir build -R "ProviderDependencyIndexTest|ProviderIncrementalPublicationTest" --output-on-failure
```

Expected: component, provenance-only, and native scheduler isolation cases pass.

- [ ] **Step 6: Commit scoped invalidation**

```bash
git add include/veritas/summarydb/ProviderDependencyIndex.h src/summarydb tests/unit/summarydb tests/integration/provider
git commit -m "feat: invalidate provider query dependencies"
```

---

### Task 8: M12A Conformance Gate and Documentation

**Files:**
- Modify: `docs/specs/milestones/m12-joern-cpg-summarydb-importer-design-spec.md`
- Modify: `docs/plans/README.md`
- Test: `tests/integration/provider/M12aConformanceTest.cpp`
- Modify: `tests/integration/provider/CMakeLists.txt`

**Interfaces:**
- Consumes: every M12A public interface.
- Produces: CTest label `m12a-provider-substrate` and a typed conformance fixture usable by M12B.

- [ ] **Step 1: Add the end-to-end synthetic provider test**

Construct a provider-neutral graph and external fact batch without Joern input, publish it, reopen SummaryDB, query historical/current state, verify the rooted witness, open a snapshot, and exercise a component-only update. Assert the required and forbidden typed outcomes before checking diagnostics.

```cpp
TEST_F(M12aConformanceTest, PublishesAndReopensSyntheticProviderState) {
  ASSERT_OK_AND_ASSIGN(const auto published,
                       coordinator_->Publish(SyntheticPublication()));
  ASSERT_OK_AND_ASSIGN(const auto binding,
                       repository_->ResolveCurrentBinding(BindingKey()));
  EXPECT_EQ(binding.provider_run_id, published.provider_run_id);
  EXPECT_EQ(binding.provider_projection_id,
            published.provider_projection_id);
  EXPECT_OK(provenance_->Explain(binding.provider_run_id, FactId(), Budget())
                .status());
  EXPECT_EQ(NativeBindingAfterPublish(), NativeBindingBeforePublish());
}
```

- [ ] **Step 2: Register the conformance label and run it**

Run:

```bash
cmake --build --preset default
ctest --test-dir build -L m12a-provider-substrate --output-on-failure
```

Expected: no test is missing, disabled, skipped, failed, or errored.

- [ ] **Step 3: Run the native non-regression boundary**

Run:

```bash
ctest --test-dir build -R "CpgRepositoryTest|ProjectPublication|SummaryRepositoryTest|Wpa" --output-on-failure
```

Expected: PASS with no provider selected and no provider tables changing native behavior.

- [ ] **Step 4: Record delivered test IDs**

Mark M12A implemented only after the test manifest contains `SCH-012`, `SCH-013`, `ID-001`, `ID-007` through `ID-010`, `NRM-011`, `FCT-001` through `FCT-011`, and `PUB-003` through `PUB-012` at their owning tests.

- [ ] **Step 5: Commit the M12A qualification**

```bash
git add docs tests/integration/provider
git commit -m "test: qualify m12a provider substrate"
```

---

## M12A Registered Acceptance Ownership

Each stable case has one registered owning test; other task-level assertions are
supporting coverage rather than duplicate registrations.

| Owning test | Stable acceptance cases |
| --- | --- |
| `ProviderTypesTest` | `SCH-012`, `SCH-013` |
| `ProviderGraphCanonicalizerTest` | `ID-001`, `ID-007`, `ID-008`, `ID-009`, `ID-010`, `NRM-011` |
| `FactPublicationValidatorTest` | `FCT-001`, `FCT-002`, `FCT-003`, `FCT-004`, `FCT-005`, `FCT-006`, `FCT-007`, `FCT-008`, `FCT-009`, `FCT-010`, `FCT-011` |
| `ProviderGraphRepositoryTest` | `PUB-008` |
| `ProviderPublicationCoordinatorTest` | `PUB-003`, `PUB-004`, `PUB-005`, `PUB-006`, `PUB-007`, `PUB-009`, `PUB-012` |
| `ProviderDependencyIndexTest` | `PUB-010`, `PUB-011` |
| `M12aConformanceTest` | Cross-layer/native non-regression gate; no duplicate stable ID registration |

M12A is complete only when the synthetic provider path works without any Joern parser, the exact current run/projection pair is pinned, publication is fully atomic, and the M6/native analysis path remains behaviorally unchanged.
