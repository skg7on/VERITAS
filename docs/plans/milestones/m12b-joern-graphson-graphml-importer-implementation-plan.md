# M12B Joern GraphSON/GraphML Importer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Directly import supported whole-program Joern GraphSON and GraphML exports into the M12A provider graph/fact publication boundary with deterministic identity, bounded untrusted-input handling, explicit assumptions, and rooted provenance.

**Architecture:** Private format readers convert GraphSON and GraphML into one typed `RawProviderGraph`; no JSON, XML, TinkerPop, or Joern type crosses that boundary. A versioned Joern schema adapter validates snapshot correspondence, extracts capabilities, resolves stable identities, normalizes topology/memory/facts, and constructs one complete M12A `ProviderPublication`. The existing M12A coordinator is the only persistence path.

**Tech Stack:** C++20, LLVM `llvm::json` for bounded GraphSON DOM parsing, Expat SAX parsing for GraphML with DTD/external entities disabled, M12A provider APIs, M9 relation/witness APIs, GoogleTest, CMake/CTest.

**Spec:** `docs/specs/milestones/m12-joern-cpg-summarydb-importer-design-spec.md`

## Global Constraints

- M12A must be implemented and passing before this plan begins.
- Upstream-derived acceptance fixtures are generated with Joern `v4.0.592` at commit `cddc360962169d02bfd2aa9a67730cd1017be549`; their manifest records the exact emitted CPG schema version and export digests.
- Inputs are specifically whole-graph `joern-export --repr=all --format=graphson|graphml` exports, not arbitrary JSON/XML, per-method DOT, `cpg.bin`, a live Joern workspace, or Joern slice JSON.
- Content determines format; filename extension is only a hint, and an explicit mismatch is `InvalidArgument`.
- One usable `META_DATA` record, a supported export dialect, a supported CPG schema adapter, valid endpoints, and snapshot correspondence are mandatory.
- Verified source or build mismatch is never overridable. Missing proof requires `--accept-unverified-context` and stable assumptions inherited by every observation and fact.
- Unknown labels/properties/overlays are preserved as inert run-specific extensions and never guessed into semantic relations.
- Joern ordinals, host paths, raw `CODE`, timestamps, format ordering, and export-only edge IDs never enter native semantic identity.
- Joern semantic facts are `INFERRED`; explicit external premises are `ASSUMED`; neither is `MUST`.
- Imported absence is open-world and never narrows native alias/points-to results or creates a negative fact.
- Parse, schema, identity, normalization, witness, budget, input-change, or storage failure publishes nothing.
- Readers perform no network access, path dereference, Joern execution, JVM loading, plugin execution, or embedded script evaluation.
- Every new VERITAS-authored source, header, and CMake file carries the repository's Apache-2.0 SPDX header; C++ uses no RTTI or exceptions.

---

## File and Interface Map

| File | Responsibility |
| --- | --- |
| `include/veritas/provider/joern/JoernImporter.h` | Public provider-neutral import request, budgets, retention policy, result, and importer facade. |
| `src/provider/joern/InputSnapshot.h/.cpp` | Bounded immutable file read, start/end file identity, digest, and input-change detection. |
| `src/provider/joern/RawProviderGraph.h/.cpp` | Private typed IDs/properties/nodes/edges/metadata and raw graph validation. |
| `src/provider/joern/JoernFormatDetector.h/.cpp` | Content-based GraphSON/GraphML detection and explicit-format mismatch rejection. |
| `src/provider/joern/GraphsonReader.h/.cpp` | Bounded supported GraphSON envelope/type decoding into `RawProviderGraph`. |
| `src/provider/joern/GraphmlReader.h/.cpp` | Secure Expat SAX GraphML decoding into the same raw model. |
| `src/provider/joern/JoernSchemaRegistry.h/.cpp` | Versioned label/property/edge/operator dispositions and supported schema/dialect registry. |
| `src/provider/joern/JoernContextValidator.h/.cpp` | `META_DATA`, source/build fingerprint, frontend/language/overlay, capability, and context-assumption validation. |
| `src/provider/joern/RawGraphCanonicalizer.h/.cpp` | Format/order-independent `ProviderRawGraphID` and provider-record identity. |
| `src/provider/joern/JoernIdentityResolver.h/.cpp` | Exact function/source/call/member bridges and explicit unresolved/ambiguous results. |
| `src/provider/joern/JoernNormalizer.h/.cpp` | Entity, relation, operator, type, call, data-flow, control, extension, and component normalization. |
| `src/provider/joern/JoernMemoryNormalizer.h/.cpp` | Member-aware memory access and compatible native `MemoryRef` resolution. |
| `src/provider/joern/JoernFactBuilder.h/.cpp` | Registered M9 facts, external batch completion, assumptions, unknowns, bindings, and rooted witnesses. |
| `src/provider/joern/JoernImporter.cpp` | Stage orchestration and call to `ProviderPublicationCoordinator::Publish`. |
| `src/tools/veritas-build.cpp` | `import --joern` CLI parsing and deterministic diagnostics. |

## Public Interface Lock

```cpp
namespace veritas::provider::joern {

enum class RequestedJoernFormat { kAuto, kGraphson, kGraphml };

struct ImportBudget {
  std::uint64_t max_input_bytes = 512ULL * 1024 * 1024;
  std::uint64_t max_nodes = 5'000'000;
  std::uint64_t max_edges = 20'000'000;
  std::uint64_t max_properties_per_record = 256;
  std::uint64_t max_property_bytes = 1ULL * 1024 * 1024;
  std::uint64_t max_collection_items = 1'000'000;
  std::uint32_t max_nesting = 128;
  std::uint64_t max_diagnostic_samples = 100;
  std::uint64_t max_peak_memory_bytes = 2ULL * 1024 * 1024 * 1024;
};

struct JoernImportRequest {
  std::filesystem::path input_path;
  std::filesystem::path project_root;
  std::filesystem::path output_root;
  RequestedJoernFormat requested_format = RequestedJoernFormat::kAuto;
  ImportBudget budget;
  bool accept_unverified_context = false;
  bool retain_raw_artifact = false;
  std::string importer_version;
  std::string mapping_version;
};

struct ProviderImportStatistics {
  std::uint64_t raw_nodes = 0;
  std::uint64_t raw_edges = 0;
  std::uint64_t normalized_entities = 0;
  std::uint64_t normalized_relations = 0;
  std::uint64_t semantic_facts = 0;
  std::uint64_t extensions = 0;
  std::uint64_t resolved_identities = 0;
  std::uint64_t ambiguous_identities = 0;
  std::uint64_t unresolved_identities = 0;
};

struct JoernImportResult {
  ProviderPublicationResult publication;
  ProviderImportStatistics statistics;
  std::vector<ProviderDiagnostic> diagnostics;
};

class JoernImporter {
 public:
  static StatusOr<JoernImportResult> Import(const JoernImportRequest& request);
};

}  // namespace veritas::provider::joern
```

The public header includes only VERITAS provider types, filesystem/value types, and status types. All raw and parser types stay under `src/provider/joern`.

---

### Task 1: Bounded Input Snapshot, Raw Graph Boundary, and Format Detection

**Files:**
- Create: `include/veritas/provider/joern/JoernImporter.h`
- Create: `src/provider/joern/InputSnapshot.h`
- Create: `src/provider/joern/InputSnapshot.cpp`
- Create: `src/provider/joern/RawProviderGraph.h`
- Create: `src/provider/joern/RawProviderGraph.cpp`
- Create: `src/provider/joern/JoernFormatDetector.h`
- Create: `src/provider/joern/JoernFormatDetector.cpp`
- Create: `src/provider/joern/CMakeLists.txt`
- Modify: `src/provider/CMakeLists.txt`
- Test: `tests/unit/provider/joern/InputSnapshotTest.cpp`
- Test: `tests/unit/provider/joern/RawProviderGraphTest.cpp`
- Test: `tests/unit/provider/joern/JoernFormatDetectorTest.cpp`
- Create: `tests/unit/provider/joern/CMakeLists.txt`
- Modify: `tests/unit/provider/CMakeLists.txt`

**Interfaces:**
- Consumes: M12A provider types and core hashing/status APIs.
- Produces: public `RequestedJoernFormat`, request/budget/statistics/result types; private `InputSnapshot`, `TypedProviderId`, `TypedPropertyValue`, `RawNode`, `RawEdge`, `RawProviderGraph`, `ValidateRawProviderGraph`, and `DetectJoernFormat`.

- [ ] **Step 1: Write failing detection, budget, and raw-invariant tests**

```cpp
TEST(JoernFormatDetectorTest, DetectsContentIndependentOfExtension) {
  EXPECT_EQ(DetectJoernFormat(Snapshot("misleading.xml", GraphsonBytes())),
            ProviderFormat::kGraphson);
}

TEST(RawProviderGraphTest, RejectsConflictingDuplicateNodeId) {
  auto graph = MinimalRawGraph();
  graph.nodes.push_back(ChangedDuplicate(graph.nodes.front()));
  EXPECT_EQ(ValidateRawProviderGraph(graph, DefaultBudget()).code(),
            StatusCode::kInvalidArgument);
}
```

- [ ] **Step 2: Run the tests to verify failure**

Run:

```bash
cmake --preset default
cmake --build --preset default
ctest --test-dir build -R "InputSnapshotTest|RawProviderGraphTest|JoernFormatDetectorTest" --output-on-failure
```

Expected: FAIL because the importer boundary does not exist.

- [ ] **Step 3: Implement an immutable bounded file snapshot**

Open one regular file without following an input-provided secondary path. Record device/inode, size, and modification metadata before reading; reject size above `max_input_bytes`; read exactly to EOF while hashing; re-check the open file and pathname after reading. Return `Aborted` when identity, size, or modification state changes. Never use embedded graph paths for I/O.

- [ ] **Step 4: Define the private typed raw graph**

```cpp
struct TypedPropertyValue {
  using List = std::vector<TypedPropertyValue>;
  using Object = std::map<std::string, TypedPropertyValue>;
  std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double,
               std::string, TypedProviderId, List, Object> value;
};

using PropertyObject = TypedPropertyValue::Object;

struct RawNode {
  TypedProviderId typed_provider_id;
  std::string label;
  PropertyObject typed_properties;
  core::SHA256Digest source_record_digest;
  SourceRecordLocator source_record_locator;
};
```

Reject non-finite numbers, invalid UTF-8, over-budget strings/lists/maps/nesting, conflicting duplicate IDs, dangling endpoints, and counts above budget. Accept only byte-equivalent duplicate nodes; preserve duplicate edges as a canonical multiset.

- [ ] **Step 5: Implement content-based format detection**

Skip UTF-8 BOM and whitespace, then recognize only the supported GraphSON object envelope or GraphML XML root. An explicit request must equal detected content; unsupported envelopes and arbitrary application JSON return `InvalidArgument`. Do not use extension to resolve ambiguous or invalid content.

- [ ] **Step 6: Run the reader-boundary tests**

```bash
cmake --build --preset default
ctest --test-dir build -R "InputSnapshotTest|RawProviderGraphTest|JoernFormatDetectorTest" --output-on-failure
```

Expected: `FMT-006`, `FMT-007`, `CLI-002`, `SEC-003`–`SEC-006`, and `SEC-008` boundary cases pass.

- [ ] **Step 7: Commit the reader boundary**

```bash
git add include/veritas/provider/joern src/provider tests/unit/provider
git commit -m "feat: define bounded Joern input boundary"
```

---

### Task 2: Supported GraphSON Reader

**Files:**
- Create: `src/provider/joern/GraphsonReader.h`
- Create: `src/provider/joern/GraphsonReader.cpp`
- Modify: `src/provider/joern/CMakeLists.txt`
- Test: `tests/unit/provider/joern/GraphsonReaderTest.cpp`
- Modify: `tests/unit/provider/joern/CMakeLists.txt`
- Create: `tests/fixtures/provider/joern/graphson/minimal-all.json`
- Create: `tests/fixtures/provider/joern/graphson/typed-values-all.json`
- Create: `tests/fixtures/provider/joern/invalid/application.json`
- Create: `tests/fixtures/provider/joern/invalid/per-method.json`

**Interfaces:**
- Consumes: `InputSnapshot`, `ImportBudget`, and private raw graph types.
- Produces: `StatusOr<RawProviderGraph> ReadGraphson(const InputSnapshot&, const ImportBudget&)`.

- [ ] **Step 1: Write failing supported-dialect and typed-value tests**

```cpp
TEST(GraphsonReaderTest, ReadsWholeGraphAndTypedIds) {
  ASSERT_OK_AND_ASSIGN(auto graph, ReadFixture("minimal-all.json"));
  EXPECT_EQ(graph.metadata.export_representation, "all");
  EXPECT_EQ(graph.nodes.front().typed_provider_id,
            TypedProviderId::Signed(1));
  EXPECT_EQ(graph.edges.front().source_provider_id,
            TypedProviderId::Signed(1));
}
```

Also reject missing vertices/edges arrays, per-method envelopes, unsupported GraphSON type wrappers, trailing bytes, duplicate object keys used by the envelope, deep nesting, and non-finite numeric encodings.

- [ ] **Step 2: Run the reader test to verify failure**

Run: `ctest --test-dir build -R GraphsonReaderTest --output-on-failure`

Expected: FAIL because `ReadGraphson` is absent.

- [ ] **Step 3: Add a preflight nesting and memory-budget scan**

Scan bytes once with JSON string/escape awareness to enforce `max_nesting`, maximum token/string lengths, and a conservative DOM peak estimate before invoking `llvm::json::parse`. Reject a file whose `input_bytes + estimated_dom_bytes + raw_graph_bytes` exceeds `max_peak_memory_bytes`.

- [ ] **Step 4: Decode only the registered whole-graph envelope**

Decode Joern/TinkerPop typed scalar/list/map wrappers into `TypedPropertyValue`, preserve typed numeric/string IDs without coercion, compute each source-record digest from canonical typed record content, and retain bounded JSON-pointer-style locators. Verify complete consumption and `repr=all` evidence from envelope/metadata.

- [ ] **Step 5: Run the GraphSON tests**

```bash
cmake --build --preset default
ctest --test-dir build -R GraphsonReaderTest --output-on-failure
```

Expected: `FMT-001`, `FMT-007`, `FMT-008`, `SEC-002`, and GraphSON portions of `SEC-003`/`PER-003` pass.

- [ ] **Step 6: Commit GraphSON support**

```bash
git add src/provider/joern tests/unit/provider/joern tests/fixtures/provider/joern
git commit -m "feat: read Joern GraphSON exports"
```

---

### Task 3: Secure GraphML Reader

**Files:**
- Modify: `cmake/Dependencies.cmake`
- Create: `src/provider/joern/GraphmlReader.h`
- Create: `src/provider/joern/GraphmlReader.cpp`
- Modify: `src/provider/joern/CMakeLists.txt`
- Test: `tests/unit/provider/joern/GraphmlReaderTest.cpp`
- Modify: `tests/unit/provider/joern/CMakeLists.txt`
- Create: `tests/fixtures/provider/joern/graphml/minimal-all.graphml`
- Create: `tests/fixtures/provider/joern/graphml/typed-values-all.graphml`
- Create: `tests/fixtures/provider/joern/invalid/external-entity.graphml`
- Create: `tests/fixtures/provider/joern/invalid/deep-nesting.graphml`

**Interfaces:**
- Consumes: Expat, `InputSnapshot`, `ImportBudget`, and raw graph types.
- Produces: `StatusOr<RawProviderGraph> ReadGraphml(const InputSnapshot&, const ImportBudget&)`.

- [ ] **Step 1: Write failing secure-parser tests**

```cpp
TEST(GraphmlReaderTest, RejectsDoctypeWithoutResolvingEntity) {
  NetworkProbe probe;
  const auto result = ReadFixture("external-entity.graphml");
  EXPECT_EQ(result.status().code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(probe.request_count(), 0);
}
```

Test supported key declarations/defaults, typed IDs/properties, directed edges, duplicate IDs, dangling endpoints, excessive nesting, property/count budgets, and complete EOF consumption.

- [ ] **Step 2: Run the reader test to verify failure**

Run: `ctest --test-dir build -R GraphmlReaderTest --output-on-failure`

Expected: FAIL because Expat and `ReadGraphml` are not wired.

- [ ] **Step 3: Add the single XML dependency**

Use `find_package(EXPAT REQUIRED)` and link `EXPAT::EXPAT` privately to the Joern reader target. Do not expose Expat headers from a public header.

- [ ] **Step 4: Implement a bounded SAX state machine**

Register start/end/text handlers for `graphml`, `key`, `graph`, `node`, `edge`, and `data`. Reject DTD declarations, entity declarations, external entity callbacks, processing instructions, unknown structural elements, nested graphs, and undirected/mixed graph modes. Enforce depth/count/text/property budgets during callbacks and stop parsing on first failure.

- [ ] **Step 5: Emit the same typed raw model**

Resolve GraphML `key` declarations to registered typed properties, preserve provider ID type, canonicalize whitespace only where the schema disposition permits it, compute record digests, and retain bounded line/column locators.

- [ ] **Step 6: Run the GraphML tests**

```bash
cmake --build --preset default
ctest --test-dir build -R GraphmlReaderTest --output-on-failure
```

Expected: `FMT-002`, `SEC-001`–`SEC-005`, and XML portions of `PER-003` pass.

- [ ] **Step 7: Commit GraphML support**

```bash
git add cmake/Dependencies.cmake src/provider/joern tests/unit/provider/joern tests/fixtures/provider/joern
git commit -m "feat: securely read Joern GraphML exports"
```

---

### Task 4: Versioned Schema, Context Correspondence, and Capabilities

**Files:**
- Create: `src/provider/joern/JoernSchemaRegistry.h`
- Create: `src/provider/joern/JoernSchemaRegistry.cpp`
- Create: `src/provider/joern/JoernContextValidator.h`
- Create: `src/provider/joern/JoernContextValidator.cpp`
- Modify: `src/provider/joern/CMakeLists.txt`
- Test: `tests/unit/provider/joern/JoernSchemaRegistryTest.cpp`
- Test: `tests/integration/provider/joern/JoernContextValidatorTest.cpp`
- Create: `tests/integration/provider/joern/CMakeLists.txt`
- Modify: `tests/integration/provider/CMakeLists.txt`
- Create: `tests/fixtures/provider/joern/schema/supported-cpg.graphson`
- Create: `tests/fixtures/provider/joern/schema/unsupported-cpg.graphson`
- Create: `tests/fixtures/provider/joern/schema/unknown-overlay.graphson`

**Interfaces:**
- Consumes: raw graph metadata and the existing M1 `AnalysisManifest` loaded from `--project`/SummaryDB.
- Produces: `JoernSchemaAdapter`, `PropertyDisposition`, `OperatorMapping`, `ValidatedJoernContext`, `ProviderContextBinding`, capabilities, assumptions, and extensions.

```cpp
StatusOr<const JoernSchemaAdapter*> SelectSchemaAdapter(
    const RawProviderGraph& graph);
StatusOr<ValidatedJoernContext> ValidateJoernContext(
    const RawProviderGraph& graph,
    const build::AnalysisManifest& manifest,
    bool accept_unverified_context);
std::vector<provider::ProviderCapability> ExtractCapabilities(
    const RawProviderGraph& graph, const JoernSchemaAdapter& adapter);
```

- [ ] **Step 1: Write failing schema/context cases**

```cpp
TEST_F(JoernContextValidatorTest, VerifiedMismatchCannotBeOverridden) {
  auto request = MatchingRequest();
  request.accept_unverified_context = true;
  auto graph = GraphWithDifferentSourceFingerprint();
  EXPECT_EQ(ValidateContext(graph, request, Manifest()).status().code(),
            StatusCode::kFailedPrecondition);
}

TEST_F(JoernContextValidatorTest, MissingBuildProofCreatesInheritedAssumption) {
  ASSERT_OK_AND_ASSIGN(
      const auto context,
      ValidateContext(SourceMatchedGraph(), OptInRequest(), Manifest()));
  EXPECT_EQ(context.binding.basis,
            ContextBindingBasis::kSourceVerifiedBuildAsserted);
  EXPECT_THAT(context.assumptions,
              Contains(AssumptionWithKind("provider.context.build_asserted.v1")));
}
```

- [ ] **Step 2: Run the focused tests to verify failure**

Run: `ctest --test-dir build -R "JoernSchemaRegistryTest|JoernContextValidatorTest" --output-on-failure`

Expected: FAIL because schema/context adapters are absent.

- [ ] **Step 3: Register every V1 label, edge, property, and operator disposition**

Use closed tables for the section 10 allowlists and property dispositions `SEMANTIC`, `OCCURRENCE`, `PROVENANCE_ONLY`, `DIAGNOSTIC`, and `REJECT`. Unknown vocabulary returns an inert extension descriptor. Schema lookup keys include export dialect and CPG schema version; Joern distribution version is optional metadata and is never inferred from schema version.

- [ ] **Step 4: Validate exactly one usable `META_DATA` record**

Reject missing metadata, conflicting multiple metadata nodes, unsupported schema/dialect, and non-whole-graph exports. Extract language, frontend, overlays, source/build fingerprints, and producer version. Unknown overlay names become extension metadata instead of enabling capabilities.

- [ ] **Step 5: Implement correspondence proof and explicit opt-in**

Require an existing compatible native summary/M6 binding for the requested repository/revision/build; detached provider imports are `FailedPrecondition`. Compare recognized provider fingerprints with SummaryDB/M1 source and build fingerprints. Return `kVerified` only when both match. With source match and missing build proof, require the flag and emit the build-asserted assumption. With missing source/build proof, require the flag and emit the user-asserted assumption. Reject every verified mismatch before consulting the flag.

- [ ] **Step 6: Derive bounded capability records**

Combine declared overlays and observed registered vocabulary into `ABSENT|PRESENT|PARTIAL|UNKNOWN` with `DECLARED|OBSERVED|VALIDATED` basis. Absence of a data-flow overlay cannot claim `DEF_USE`; observed vocabulary never claims global sound completeness. Record unresolved counts and assumptions without free-form promotion.

- [ ] **Step 7: Run the schema/context tests**

```bash
cmake --build --preset default
ctest --test-dir build -R "JoernSchemaRegistryTest|JoernContextValidatorTest" --output-on-failure
```

Expected: `SCH-001`–`SCH-011` and `SCH-013` pass.

- [ ] **Step 8: Commit schema/context validation**

```bash
git add src/provider/joern tests/unit/provider/joern tests/integration/provider tests/fixtures/provider/joern
git commit -m "feat: validate Joern schema and snapshot context"
```

---

### Task 5: Raw-Graph Canonicalization and Cross-Format Identity Parity

**Files:**
- Create: `src/provider/joern/RawGraphCanonicalizer.h`
- Create: `src/provider/joern/RawGraphCanonicalizer.cpp`
- Modify: `src/provider/joern/CMakeLists.txt`
- Test: `tests/unit/provider/joern/RawGraphCanonicalizerTest.cpp`
- Test: `tests/integration/provider/joern/CrossFormatParityTest.cpp`
- Modify: `tests/unit/provider/joern/CMakeLists.txt`
- Modify: `tests/integration/provider/joern/CMakeLists.txt`
- Create: `tests/fixtures/provider/joern/parity/simple.graphson`
- Create: `tests/fixtures/provider/joern/parity/simple.graphml`
- Create: `tests/fixtures/provider/joern/parity/simple-reordered.graphson`

**Interfaces:**
- Consumes: validated raw graph and schema dispositions from Task 4.
- Produces: `CanonicalRawGraph`, `ProviderRawGraphId`, `ProviderRecordId`, and canonical raw-record digests used by identity/provenance stages.

```cpp
StatusOr<CanonicalRawGraph> CanonicalizeRawGraph(
    const RawProviderGraph& graph, const JoernSchemaAdapter& adapter);
core::StableId ProviderRawGraphId(const CanonicalRawGraph& graph);
core::StableId ProviderRecordId(const provider::ProviderArtifact& artifact,
                                RawRecordKind kind,
                                const TypedProviderId& provider_id);
```

- [ ] **Step 1: Write failing format/order parity tests**

```cpp
TEST(CrossFormatParityTest, EquivalentExportsShareRawAndProjectionInputs) {
  ASSERT_OK_AND_ASSIGN(const auto json, CanonicalizeFixture("simple.graphson"));
  ASSERT_OK_AND_ASSIGN(const auto xml, CanonicalizeFixture("simple.graphml"));
  EXPECT_EQ(json.raw_graph_id, xml.raw_graph_id);
  EXPECT_EQ(json.semantic_records, xml.semantic_records);
  EXPECT_NE(json.artifact_id, xml.artifact_id);
}
```

- [ ] **Step 2: Run the tests to verify failure**

Run: `ctest --test-dir build -R "RawGraphCanonicalizerTest|CrossFormatParityTest" --output-on-failure`

Expected: FAIL because format-independent raw identity is absent.

- [ ] **Step 3: Implement canonical typed metadata/node/edge ordering**

Exclude serialization format, record/property order, source locators, artifact/run IDs, absolute roots, timestamps, raw `CODE`, diagnostic/provenance-only properties, unknown extensions, and export-only edge IDs. Include typed node ID, label, semantic/occurrence properties, typed endpoints, edge label, semantic qualifiers, and duplicate-edge multiplicity.

- [ ] **Step 4: Implement record and raw-graph IDs without hash cycles**

`ProviderRawGraphID` hashes the canonical typed raw graph under `provider.raw_graph.v1`. `ProviderRecordID` hashes artifact ID, record kind, and typed provider record ID under `joern.record.v1`; record IDs remain run/artifact specific and are excluded from raw/projection semantic identity.

- [ ] **Step 5: Run the canonical raw-identity tests**

```bash
cmake --build --preset default
ctest --test-dir build -R "RawGraphCanonicalizerTest|CrossFormatParityTest" --output-on-failure
```

Expected: `FMT-003`–`FMT-005`, `FMT-009`, and `FMT-010` pass at the raw/canonical boundary; projection equality is completed in Task 7.

- [ ] **Step 6: Commit canonical raw identity**

```bash
git add src/provider/joern tests/unit/provider/joern tests/integration/provider tests/fixtures/provider/joern
git commit -m "feat: canonicalize Joern raw graphs"
```

---

### Task 6: Exact Identity Resolution and Explicit Unresolved Results

**Files:**
- Create: `src/provider/joern/JoernIdentityResolver.h`
- Create: `src/provider/joern/JoernIdentityResolver.cpp`
- Modify: `src/provider/joern/CMakeLists.txt`
- Test: `tests/integration/provider/joern/JoernIdentityResolverTest.cpp`
- Modify: `tests/integration/provider/joern/CMakeLists.txt`
- Create: `tests/fixtures/projects/joern_identity/compile_commands.json`
- Create: `tests/fixtures/projects/joern_identity/identity.cpp`
- Create: `tests/fixtures/provider/joern/identity/exact.graphson`
- Create: `tests/fixtures/provider/joern/identity/ambiguous.graphson`

**Interfaces:**
- Consumes: canonical raw graph, validated program context, M2 identities, M6 native projection, M9 source anchors, and manifest/build context.
- Produces: `IdentityResolution` variants `ResolvedEntity`, `ResolvedOccurrence`, `AmbiguousIdentity`, and `UnresolvedIdentity`; also `ResolveFunction`, `ResolveSourceAnchor`, `ResolveCallSite`, and `ResolveMemoryRef`.

```cpp
using IdentityResolution = std::variant<ResolvedEntity, ResolvedOccurrence,
                                        AmbiguousIdentity, UnresolvedIdentity>;

class JoernIdentityResolver {
 public:
  StatusOr<IdentityResolution> ResolveFunction(const RawNode& method) const;
  StatusOr<IdentityResolution> ResolveSourceAnchor(const RawNode& node) const;
  StatusOr<IdentityResolution> ResolveCallSite(const RawNode& call) const;
  StatusOr<IdentityResolution> ResolveMemoryRef(
      const RawNode& access_node,
      std::span<const ProgramRelation> incident_relations) const;
};
```

- [ ] **Step 1: Write failing exact/ambiguous/unresolved bridge tests**

```cpp
TEST_F(JoernIdentityResolverTest, AmbiguousFunctionKeepsSortedCandidates) {
  ASSERT_OK_AND_ASSIGN(const auto resolution,
                       resolver_.ResolveFunction(AmbiguousMethod()));
  ASSERT_TRUE(std::holds_alternative<AmbiguousIdentity>(resolution));
  const auto& candidates = std::get<AmbiguousIdentity>(resolution).candidates;
  EXPECT_TRUE(std::ranges::is_sorted(candidates));
  EXPECT_EQ(candidates.size(), 2);
}
```

- [ ] **Step 2: Run the test to verify failure**

Run: `ctest --test-dir build -R JoernIdentityResolverTest --output-on-failure`

Expected: FAIL because the resolver does not exist.

- [ ] **Step 3: Implement exact bridges only**

Resolve functions by mangled name, normalized signature, and build context; anchors by normalized project-relative path and exact source span; call sites by function plus canonical occurrence; parameters by function/position/type; memory refs by resolved base plus compatible field path. Require agreement among all present stable inputs and never use display name or line alone.

- [ ] **Step 4: Implement occurrence and external identity**

Build `ProgramOccurrenceID` from revision/build/path/enclosing function/span/kind/structural discriminator. Derive the discriminator from normalized AST ancestry and semantic sibling keys, not reader order. When no stable resolution exists, use `ExternalEntityID(provider, raw_graph_id, typed_provider_record_id)`; ambiguity stores sorted candidates and never chooses one.

- [ ] **Step 5: Run the identity-resolution tests**

```bash
cmake --build --preset default
ctest --test-dir build -R JoernIdentityResolverTest --output-on-failure
```

Expected: `ID-002`–`ID-007` pass.

- [ ] **Step 6: Commit identity resolution**

```bash
git add src/provider/joern tests/integration/provider tests/fixtures/projects/joern_identity tests/fixtures/provider/joern
git commit -m "feat: resolve Joern entities to stable IDs"
```

---

### Task 7: Entity, Relation, Operator, and Extension Normalization

**Files:**
- Create: `src/provider/joern/JoernNormalizer.h`
- Create: `src/provider/joern/JoernNormalizer.cpp`
- Modify: `src/provider/joern/CMakeLists.txt`
- Test: `tests/unit/provider/joern/JoernNormalizerTest.cpp`
- Test: `tests/integration/provider/joern/JoernNormalizationGoldenTest.cpp`
- Modify: `tests/unit/provider/joern/CMakeLists.txt`
- Modify: `tests/integration/provider/joern/CMakeLists.txt`
- Create: `tests/fixtures/provider/joern/normalization/all-v1-labels.graphson`
- Create: `tests/golden/provider/joern/all-v1-labels.normalized.txt`

**Interfaces:**
- Consumes: canonical raw graph, schema adapter, validated context/capabilities, and identity resolutions.
- Produces: a complete canonical `ProviderProgramGraph` except memory-specific enrichments handled in Task 8.

```cpp
StatusOr<provider::ProviderProgramGraphInput> NormalizeJoernGraph(
    const CanonicalRawGraph& graph,
    const JoernSchemaAdapter& adapter,
    const ValidatedJoernContext& context,
    const JoernIdentityResolver& identities);
```

- [ ] **Step 1: Write failing allowlist and forbidden-inference tests**

```cpp
TEST(JoernNormalizerTest, UnknownOperatorIsInertExtension) {
  ASSERT_OK_AND_ASSIGN(const auto graph, Normalize(UnknownOperatorGraph()));
  EXPECT_TRUE(graph.relations.empty());
  EXPECT_THAT(graph.extensions,
              Contains(ExtensionWithLabel("<operator>.vendorMagic")));
}
```

Add table-driven cases for every section 10.1 entity and section 10.2 relation mapping, coherent call/argument/receiver grouping, relation distinction, reaching-def variable qualifiers, known operators, and unknown vocabulary round-trip.

- [ ] **Step 2: Run normalization tests to verify failure**

Run: `ctest --test-dir build -R "JoernNormalizerTest|JoernNormalizationGoldenTest" --output-on-failure`

Expected: FAIL because the normalizer is absent.

- [ ] **Step 3: Normalize registered entities and relations**

Map only the V1 allowlists. Emit graph-only `SyntaxChild`/containment without semantic fact inflation. Preserve `ControlFlow`, `DefUse`, `ControlDependency`, `Dominates`, and `PostDominates` as distinct kinds. Always map Joern call targets to `MayCall`, even for one target.

- [ ] **Step 4: Normalize registered operators before relation construction**

Map assignment, addition, subtraction, field/indirect-field access, indirection, address-of, index access, and conditional using the versioned registry. Never infer load/store/alias from raw source spelling. Preserve unregistered operators and properties as `ProviderExtensionObservation`.

- [ ] **Step 5: Complete canonical projection parity**

Run GraphSON, GraphML, and reordered GraphSON fixtures through normalization and `CanonicalizeProviderProgramGraph`. Assert equal entity/relation/component bytes and `ProviderProjectionID`, while artifact/run/observation provenance remains distinct.

- [ ] **Step 6: Run the semantic-normalization tests**

```bash
cmake --build --preset default
ctest --test-dir build -R "JoernNormalizerTest|JoernNormalizationGoldenTest|CrossFormatParityTest" --output-on-failure
```

Expected: `FMT-003`–`FMT-005`, `NRM-001`–`NRM-011`, and extension query round-trip pass.

- [ ] **Step 7: Commit semantic normalization**

```bash
git add src/provider/joern tests/unit/provider/joern tests/integration/provider tests/fixtures/provider/joern tests/golden/provider/joern
git commit -m "feat: normalize Joern program graphs"
```

---

### Task 8: Member-Aware Memory and External Fact/Witness Construction

**Files:**
- Create: `src/provider/joern/JoernMemoryNormalizer.h`
- Create: `src/provider/joern/JoernMemoryNormalizer.cpp`
- Create: `src/provider/joern/JoernFactBuilder.h`
- Create: `src/provider/joern/JoernFactBuilder.cpp`
- Modify: `src/provider/joern/CMakeLists.txt`
- Test: `tests/unit/provider/joern/JoernMemoryNormalizerTest.cpp`
- Test: `tests/unit/provider/joern/JoernFactBuilderTest.cpp`
- Modify: `tests/unit/provider/joern/CMakeLists.txt`

**Interfaces:**
- Consumes: normalized graph, identity resolutions, capabilities, assumptions, provider record IDs/digests, M9 `relations.v2`, and M12A `ExternalFactBatch`.
- Produces: `ProviderMemoryAccess`, `NormalizedJoernImport`, enriched provider graph, and a complete validated `ExternalFactBatch BuildExternalFactBatch(const NormalizedJoernImport&)`.

```cpp
StatusOr<ProviderMemoryAccess> NormalizeJoernMemoryAccess(
    const ProgramEntity& operation,
    std::span<const ProgramRelation> incident_relations,
    const JoernIdentityResolver& identities);
StatusOr<facts::ExternalFactBatch> BuildExternalFactBatch(
    const NormalizedJoernImport& input,
    const facts::RelationRegistry& relations);
```

- [ ] **Step 1: Write failing memory/fact/witness tests**

```cpp
TEST(JoernMemoryNormalizerTest, PreservesBaseFieldDereferenceAndIndex) {
  ASSERT_OK_AND_ASSIGN(const auto access, NormalizeMemoryAccess(FieldGraph()));
  EXPECT_EQ(access.base_entity_ref, EntityId("packet"));
  EXPECT_THAT(access.canonical_field_path, ElementsAre("header", "length"));
  EXPECT_EQ(access.dereference_depth, 1);
  EXPECT_THAT(access.index_components, ElementsAre(IndexRef("i")));
}

TEST(JoernFactBuilderTest, ContextAssumptionRootsEveryFactWitness) {
  ASSERT_OK_AND_ASSIGN(const auto batch, Build(UserAssertedImport()));
  for (const auto& binding : batch.run_bindings) {
    EXPECT_TRUE(WitnessReaches(binding.selected_witness_id,
                               AssumptionId("context")));
  }
}
```

- [ ] **Step 2: Run the tests to verify failure**

Run: `ctest --test-dir build -R "JoernMemoryNormalizerTest|JoernFactBuilderTest" --output-on-failure`

Expected: FAIL because memory/fact builders are absent.

- [ ] **Step 3: Implement member-aware memory normalization**

Preserve access kind, resolved/external base, canonical field path, dereference depth, index components, declared/recovered type, source anchor, and uncertainty reasons. Reuse a native `MemoryRef` only when base and field path resolve compatibly. Otherwise retain an external entity and inferred observation. Never create guessed ranges or narrow native alias/points-to state by omission.

- [ ] **Step 4: Map only registered semantic relations to M9 facts**

Use the relation registry for calls, selected control flow/dependency, def-use/value flow, references, types, reads/writes, and aliases. Structural-only topology emits no fact. Keep relation modality in the fact cells and epistemic origin in the fact epistemic field. External semantic analysis is `INFERRED`; provider premise/model declarations are `ASSUMED`.

- [ ] **Step 5: Build complete components and rooted witnesses**

Derive expected components from the validated projection and mapping registry; produce exactly matching completions. For every fact, emit one run binding and selected finite acyclic witness whose leaves reach provider artifact, provider records/raw digests, assumptions, or prior facts. Include unresolved identities and unsupported mappings as bounded unknown observations, not all-pairs expansions.

- [ ] **Step 6: Validate memory/fact construction**

```bash
cmake --build --preset default
ctest --test-dir build -R "JoernMemoryNormalizerTest|JoernFactBuilderTest|FactPublicationValidatorTest" --output-on-failure
```

Expected: `MEM-001`–`MEM-005` and M12B generation sides of `FCT-001`–`FCT-011` pass.

- [ ] **Step 7: Commit memory/fact construction**

```bash
git add src/provider/joern tests/unit/provider/joern
git commit -m "feat: build Joern memory facts and witnesses"
```

---

### Task 9: Import Orchestrator and Atomic M12A Publication

**Files:**
- Create: `src/provider/joern/JoernImporter.cpp`
- Modify: `src/provider/joern/CMakeLists.txt`
- Test: `tests/integration/provider/joern/JoernImporterTest.cpp`
- Modify: `tests/integration/provider/joern/CMakeLists.txt`

**Interfaces:**
- Consumes: Tasks 1–8 and M12A `ProviderPublicationCoordinator::Publish`.
- Produces: `JoernImporter::Import(const JoernImportRequest&)`, private `BuildJoernPublication`, and private `CollectImportStatistics`.

- [ ] **Step 1: Write failing end-to-end success/failure tests**

```cpp
TEST_F(JoernImporterTest, MalformedSecondImportRetainsPriorBinding) {
  ASSERT_OK(JoernImporter::Import(ValidRequest()).status());
  const auto prior = CurrentJoernBinding();
  EXPECT_FALSE(JoernImporter::Import(MalformedRequest()).ok());
  EXPECT_EQ(CurrentJoernBinding(), prior);
  EXPECT_EQ(ProviderRunCount(), 1);
}
```

- [ ] **Step 2: Run the importer test to verify failure**

Run: `ctest --test-dir build -R JoernImporterTest --output-on-failure`

Expected: FAIL because the public facade has no implementation.

- [ ] **Step 3: Implement the single staged pipeline**

Execute: input snapshot → format detection → format reader → raw validation → schema/context validation → raw canonicalization → identity resolution → graph/memory normalization → external fact/witness construction → complete pre-publication validation → M12A publication. Return immediately on the first non-OK status; never open a publication transaction before the complete in-memory result validates.

```cpp
StatusOr<JoernImportResult> JoernImporter::Import(
    const JoernImportRequest& request) {
  auto publication = BuildJoernPublication(request);
  if (!publication.ok()) return publication.status();
  auto coordinator = ProviderPublicationCoordinator::Open(
      request.output_root.string());
  if (!coordinator.ok()) return coordinator.status();
  auto statistics = CollectImportStatistics(*publication);
  auto diagnostics = publication->run.diagnostics;
  auto published = (*coordinator)->Publish(std::move(*publication));
  if (!published.ok()) return published.status();
  return JoernImportResult{.publication = std::move(*published),
                           .statistics = std::move(statistics),
                           .diagnostics = std::move(diagnostics)};
}
```

- [ ] **Step 4: Produce bounded deterministic import statistics**

Return artifact/run/projection IDs; context binding; format/schema/frontend/language/overlays; raw/normalized node/edge counts; fact/extension/resolution counts; capabilities/assumptions; component delta; and binding-advanced state. Sort diagnostics by stable stage/code/record key and cap samples at `max_diagnostic_samples`.

- [ ] **Step 5: Exercise parser, identity, normalization, witness, and transaction failure**

Assert each failure publishes no new graph/fact/witness/history/current row. Inject M12A transaction failure after successful normalization and assert rollback. Re-import identical input for no-op behavior, then import the equivalent other format and assert shared projection/distinct run provenance.

- [ ] **Step 6: Run the importer-orchestrator tests**

```bash
cmake --build --preset default
ctest --test-dir build -R "JoernImporterTest|ProviderPublicationCoordinatorTest" --output-on-failure
```

Expected: `PUB-001`–`PUB-009` and `PUB-012` importer paths pass.

- [ ] **Step 7: Commit the orchestrator**

```bash
git add src/provider/joern tests/integration/provider
git commit -m "feat: import Joern graphs into SummaryDB"
```

---

### Task 10: `veritas-build import --joern` CLI

**Files:**
- Modify: `src/tools/veritas-build.cpp`
- Modify: `src/tools/CMakeLists.txt`
- Test: `tests/integration/provider/joern/VeritasBuildJoernImportCliTest.cpp`
- Modify: `tests/integration/provider/joern/CMakeLists.txt`
- Create: `tests/golden/provider/joern/import-success.txt`

**Interfaces:**
- Consumes: public `JoernImporter` request/result types only.
- Produces: the exact CLI contract in specification section 19, private `ParseJoernImportArguments`, `PrintJoernImportResult`, and `PrintStatus` helpers.

- [ ] **Step 1: Write failing CLI contract tests**

```cpp
TEST(VeritasBuildJoernImportCliTest, RequiresAllThreeScopeArguments) {
  const auto result = Run({"import", "--joern", Fixture("simple.graphson")});
  EXPECT_NE(result.exit_code, 0);
  EXPECT_THAT(result.stderr_text,
              HasSubstr("--project and --output are required"));
}
```

Cover missing/duplicate/unknown flags, auto detection, explicit mismatch, absent context proof without/with opt-in, raw retention, success diagnostics, and failure exit status. Assert no Joern executable/JVM/network subprocess is invoked.

- [ ] **Step 2: Run the CLI test to verify failure**

Run: `ctest --test-dir build -R VeritasBuildJoernImportCliTest --output-on-failure`

Expected: FAIL because `veritas-build` recognizes only native analysis arguments.

- [ ] **Step 3: Add an explicit command parser**

Dispatch `analyze` and `import` before parsing command-specific flags. `import` requires exactly one `--joern`, `--project`, and `--output`; accepts `--format auto|graphson|graphml`, `--accept-unverified-context`, and a raw-retention flag; rejects analyze-only options and positional ambiguity.

```cpp
if (command == "analyze") return Analyze(ParseAnalyzeArguments(args));
if (command == "import") {
  auto request = ParseJoernImportArguments(args);
  if (!request.ok()) return PrintStatus(request.status());
  return PrintJoernImportResult(joern::JoernImporter::Import(*request));
}
return PrintStatus(Status::InvalidArgument("expected analyze or import"));
```

- [ ] **Step 4: Print stable diagnostics without treating them as serialization**

Print IDs and counts in a fixed field order, sorted overlays/capabilities/assumptions/deltas, explicit unknown Joern version, and whether the binding advanced. Keep origin path and diagnostic samples out of identity comparisons.

- [ ] **Step 5: Run the Joern-import CLI tests**

```bash
cmake --build --preset default
ctest --test-dir build -R VeritasBuildJoernImportCliTest --output-on-failure
```

Expected: `CLI-001`–`CLI-005` and `CLI-007` pass.

- [ ] **Step 6: Commit the CLI**

```bash
git add src/tools tests/integration/provider tests/golden/provider/joern
git commit -m "feat: expose Joern import CLI"
```

---

### Task 11: Adversarial, Cross-Format, and Scale Qualification

**Files:**
- Create: `tests/integration/provider/joern/JoernSecurityTest.cpp`
- Create: `tests/integration/provider/joern/JoernScaleTest.cpp`
- Create: `tests/integration/provider/joern/M12bConformanceTest.cpp`
- Modify: `tests/integration/provider/joern/CMakeLists.txt`
- Create: `tests/support/provider/JoernFixtureBuilder.h`
- Create: `tests/support/provider/JoernFixtureBuilder.cpp`
- Modify: `tests/support/CMakeLists.txt`
- Create: `tests/fixtures/projects/joern_golden/compile_commands.json`
- Create: `tests/fixtures/projects/joern_golden/program.c`
- Create: `tests/fixtures/projects/joern_golden/program.cpp`
- Create: `tests/fixtures/provider/joern/FIXTURE-MANIFEST.md`
- Create: `tests/fixtures/provider/joern/SHA256SUMS`

**Interfaces:**
- Consumes: complete M12B public behavior.
- Produces: CTest labels `m12b-joern-importer`, `m12b-security`, and `m12b-scale`.

```cpp
class JoernFixtureBuilder {
 public:
  JoernFixtureBuilder& AddNode(TypedProviderId id, std::string label,
                               PropertyObject properties);
  JoernFixtureBuilder& AddEdge(TypedProviderId source, TypedProviderId target,
                               std::string label,
                               PropertyObject properties);
  std::string BuildGraphson(RecordOrder order) const;
  std::string BuildGraphml(RecordOrder order) const;
};
```

- [ ] **Step 1: Add deterministic generated fixture support**

`JoernFixtureBuilder` emits bounded GraphSON and GraphML records in chosen order, with controlled properties, duplicates, dangling endpoints, nesting, and byte/count sizes. It also supplies a file-rewrite hook for `SEC-008` and a process/network probe for `CLI-007`/`SEC-001`.

Record Joern `v4.0.592` / `cddc360962169d02bfd2aa9a67730cd1017be549`, the emitted CPG schema version, frontend, exact `joern-export --repr=all` commands, source fixture digest, export digest, and regeneration command in `FIXTURE-MANIFEST.md`; verify every checked-in upstream-derived fixture against `SHA256SUMS` before running semantic assertions.

- [ ] **Step 2: Implement all adversarial cases**

Exercise DTD/entity, deep nesting, oversized scalar/list, node/edge/input limits, dangling endpoint, conflicting ID, path traversal/absolute-path values, input mutation, and budget cleanup. Assert typed status codes, no external retrieval/read, no staging residue, and unchanged current binding before checking diagnostic strings.

- [ ] **Step 3: Implement declared peak-memory and determinism tests**

Generate a large fixture sized for the CI profile, run with a declared `max_peak_memory_bytes`, sample process peak RSS through the existing platform test helper, and require it to stay within the declared budget. Import twice with shuffled order and require equal raw/projection/component IDs and byte-identical canonical graph/fact output. Require bounded diagnostic sample count.

- [ ] **Step 4: Run the complete M12B gate**

Run:

```bash
cmake --build --preset default
ctest --test-dir build -L m12b-joern-importer --output-on-failure
ctest --test-dir build -L m12b-security --output-on-failure
ctest --test-dir build -L m12b-scale --output-on-failure
```

Expected: no required case is missing, disabled, skipped, failed, or errored; `SEC-001`–`SEC-008` and `PER-001`–`PER-004` pass.

- [ ] **Step 5: Run the native and M12A non-regression suites**

Run:

```bash
ctest --test-dir build -L m12a-provider-substrate --output-on-failure
ctest --test-dir build -R "VeritasBuildAnalyze|CpgRepositoryTest|Wpa" --output-on-failure
```

Expected: all pass with native analysis unchanged.

- [ ] **Step 6: Commit the qualification gate**

```bash
git add tests/support tests/integration/provider tests/fixtures/projects/joern_golden
git commit -m "test: qualify Joern graph ingestion"
```

---

## M12B Registered Acceptance Ownership

Each stable case has one registered owning test; reader/normalizer unit tests
may repeat invariants as supporting coverage without re-registering the ID.

| Owning test | Stable acceptance cases |
| --- | --- |
| `GraphsonReaderTest` | `FMT-001`, `FMT-008` |
| `GraphmlReaderTest` | `FMT-002` |
| `CrossFormatParityTest` | `FMT-003`, `FMT-004`, `FMT-005`, `FMT-009`, `FMT-010` |
| `JoernFormatDetectorTest` | `FMT-006`, `FMT-007` |
| `JoernContextValidatorTest` | `SCH-001`, `SCH-002`, `SCH-003`, `SCH-004`, `SCH-005`, `SCH-006`, `SCH-007`, `SCH-008`, `SCH-009`, `SCH-010`, `SCH-011` |
| `JoernIdentityResolverTest` | `ID-002`, `ID-003`, `ID-004`, `ID-005`, `ID-006` |
| `JoernNormalizerTest` | `NRM-001`, `NRM-002`, `NRM-003`, `NRM-004`, `NRM-005`, `NRM-006`, `NRM-007`, `NRM-008`, `NRM-009`, `NRM-010` |
| `JoernMemoryNormalizerTest` | `MEM-001`, `MEM-002`, `MEM-003`, `MEM-004`, `MEM-005` |
| `JoernImporterTest` | `PUB-001`, `PUB-002` |
| `VeritasBuildJoernImportCliTest` | `CLI-001`, `CLI-002`, `CLI-003`, `CLI-004`, `CLI-005`, `CLI-007` |
| `JoernSecurityTest` | `SEC-001`, `SEC-002`, `SEC-003`, `SEC-004`, `SEC-005`, `SEC-006`, `SEC-007`, `SEC-008` |
| `JoernScaleTest` | `PER-001`, `PER-002`, `PER-003`, `PER-004` |
| `M12bConformanceTest` | Cross-format/importer/native non-regression gate; no duplicate stable ID registration |

M12B is complete only when supported GraphSON and GraphML imports are semantically identical where the specification requires, distinct provenance is retained, every unsafe or incomplete input fails atomically, and the importer can be removed without changing native analysis behavior.
