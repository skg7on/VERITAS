# VERITAS Backbone Milestone Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Break the VERITAS engineering backbone into small, testable milestones, each with a design spec and implementation plan.

**Architecture:** VERITAS should own semantic identity, summaries, persistence, incremental invalidation, thin CPG projection, provenance, and Evidence Builder APIs. Mature program-analysis projects should do traditional compiler and analysis work behind stable adapters: Clang/LLVM for C/C++ frontend and local IR facts, SVF for pointer/value-flow analysis, Souffle for recursive fact derivation, and Joern/CPG concepts as a schema reference while keeping the persistent VERITAS CPG thin.

**Tech Stack:** C++20, CMake, Clang LibTooling, LLVM analysis APIs, SVF, Protobuf, RocksDB, SQLite, Souffle, GoogleTest, Python helper scripts for golden fixture checks.

**Spec:** `docs/specs/veritas-engineering-backbone-design-specification.md`

## Global Constraints

- Function identity is semantic, not file-line based.
- Summary objects are immutable and content-addressed.
- Summary components have independent semantic and evidence hashes.
- Reverse dependencies are indexed by producer component, not only by caller.
- Recursive call regions are propagated as SCCs.
- Every derived fact has provenance.
- Epistemic state and confidence are separate.
- LLM or heuristic output is not a verified fact.
- The persistent VERITAS CPG is function- and object-centric; instruction-level detail is generated or cached on demand.
- The first language target is C/C++ through Clang and LLVM.
- The first storage stack is RocksDB for immutable objects and SQLite for metadata.
- The first WPA stack is a C++ worklist engine, with Souffle introduced when recursive fact export is stable.

---

# 1. Source-Backed Reuse Strategy

The backbone should not reimplement mature static-analysis infrastructure unless VERITAS needs a semantic contract those tools do not expose.

| Area | Reuse | VERITAS Owns | Rationale |
| --- | --- | --- | --- |
| C/C++ parsing, AST, macros, source locations, compilation database | Clang LibTooling | Stable extraction adapters and source anchors | Clang tooling already runs `FrontendAction`s over source files using compilation databases. |
| LLVM IR, SSA, MemorySSA, alias hooks, dominators, range-ish local facts | LLVM analysis APIs | Normalization into Summary IR and EIR-compatible facts | LLVM already exposes canonical IR and analysis infrastructure; VERITAS should not build a compiler frontend. |
| Pointer/value-flow, VFG, Andersen-style and demand-driven refinements | SVF | Adapter boundary, stable value IDs, component hashes, uncertainty mapping | SVF is a mature static value-flow framework over LLVM IR; use it for heavy VFG/pointer jobs before inventing custom analyses. |
| Recursive relations and transitive WPA facts | Souffle | Fact schemas, tuple IDs, provenance capture, publication | Souffle is designed as a Datalog tool for static analysis; VERITAS should add provenance and epistemic policy around it. |
| CPG concepts and schema vocabulary | Joern CPG/spec | Thin persistent VERITAS CPG projection | The architecture wants CPG benefits without storing a full instruction-level universal graph for every repo. |
| Security query idioms and data-flow test comparisons | CodeQL documentation and optional baseline runs | VERITAS-native summaries, dependencies, and evidence | CodeQL is useful as a conceptual and fixture baseline, but VERITAS should not make CodeQL DBs the core SummaryDB. |

The source architecture says "Build a VERITAS CPG, but keep it thin." This document interprets the user's "GPG" wording as "CPG" because the existing docs consistently discuss Code Property Graphs.

---

# 2. Milestone Map

| Milestone | Name | Primary Deliverable | Depends On |
| --- | --- | --- | --- |
| M0 | Project skeleton and toolchain harness | Buildable repo with dependency detection and fixture tests | none |
| M1 | Build intelligence and program context | Revision, build variant, translation unit manifest | M0 |
| M2 | Identity, canonical hashing, and metadata store | Stable IDs and SQLite metadata schema | M1 |
| M3 | Summary IR and CAS object store | Protobuf summaries, component hashes, RocksDB CAS | M2 |
| M4 | Clang/LLVM local extraction adapter | AST/IR function facts, calls, CFG, source anchors | M3 |
| M5 | SVF value-flow and pointer adapter | VFG, alias, memory/value-flow facts normalized into summaries | M4 |
| M6 | Thin VERITAS CPG projection | Function/object-centric graph projection and query API | M4, M5 |
| M7 | Reverse dependency index and incremental scheduler | Component deltas and precise consumer scheduling | M3, M4, M5 |
| M8 | SCC-aware WPA and Souffle fact engine | Recursive facts with fixpoint state and provenance hooks | M7 |
| M9 | Provenance-aware fact store and explain API | Fact/provenance tables and `veritas-explain` | M8 |
| M10 | Evidence Builder input APIs and first demo | EIR-ready slices for buffer-overflow style cases | M6, M9 |

Each milestone should merge independently. Every milestone has tests and a small user-visible CLI behavior.

Detailed milestone design specs live under `docs/specs/milestones/`:

| Milestone | Detailed Spec |
| --- | --- |
| M1 | `docs/specs/milestones/m1-build-intelligence-program-context-design-spec.md` |
| M2 | `docs/specs/milestones/m2-identity-canonical-hashing-metadata-store-design-spec.md` |
| M3 | `docs/specs/milestones/m3-summary-ir-cas-object-store-design-spec.md` |
| M4 | `docs/specs/milestones/m4-clang-llvm-local-extraction-design-spec.md` |
| M5 | `docs/specs/milestones/m5-svf-value-flow-pointer-adapter-design-spec.md` |
| M6 | `docs/specs/milestones/m6-thin-veritas-cpg-projection-design-spec.md` |
| M7 | `docs/specs/milestones/m7-reverse-dependency-incremental-scheduler-design-spec.md` |
| M8 | `docs/specs/milestones/m8-scc-wpa-souffle-fact-engine-design-spec.md` |
| M9 | `docs/specs/milestones/m9-provenance-fact-store-explain-api-design-spec.md` |
| M10 | `docs/specs/milestones/m10-evidence-builder-input-apis-demo-design-spec.md` |

Executable per-milestone implementation plans live under `docs/plan/`.

---

# 3. Proposed Repository Structure

```text
veritas/
  CMakeLists.txt
  cmake/
    Dependencies.cmake
    FindSVF.cmake
    VeritasWarnings.cmake

  proto/
    veritas/core/v1/ids.proto
    veritas/summary/v1/summary.proto
    veritas/fact/v1/fact.proto
    veritas/cpg/v1/cpg.proto

  include/veritas/
    core/
    build/
    summary/
    summarydb/
    frontend/clang/
    analysis/llvm/
    analysis/svf/
    cpg/
    wpa/
    facts/
    evidence/

  src/
    core/
    build/
    summary/
    summarydb/
    frontend/clang/
    analysis/llvm/
    analysis/svf/
    cpg/
    wpa/
    facts/
    evidence/
    tools/

  tests/
    unit/
    integration/
    fixtures/cpp/
    golden/

  tools/
    veritas-build
    veritas-query
    veritas-diff
    veritas-explain
```

The layout keeps adapters separate from VERITAS-owned semantic models. If SVF or Souffle is replaced, Summary IR, SummaryDB, CPG projection, and Evidence APIs should remain stable.

---

# 4. M0: Project Skeleton and Toolchain Harness

## Design Spec

M0 creates a buildable C++20 repository with dependency probes but no production analysis behavior. It establishes test conventions, fixture layout, and CLI entry points so later milestones have a stable place to land.

VERITAS should use CMake because Clang, LLVM, SVF, RocksDB, Protobuf, SQLite, and GoogleTest all have practical CMake integration paths. The first build should support two modes:

```text
VERITAS_USE_SYSTEM_LLVM=ON
VERITAS_USE_BUNDLED_THIRD_PARTY=OFF
```

V1 should prefer system or developer-installed dependencies. Vendoring large tools should be a separate repository decision.

## Files

- Create: `CMakeLists.txt`
- Create: `cmake/Dependencies.cmake`
- Create: `cmake/FindSVF.cmake`
- Create: `cmake/VeritasWarnings.cmake`
- Create: `include/veritas/core/Status.h`
- Create: `include/veritas/core/Version.h`
- Create: `src/core/Status.cpp`
- Create: `src/core/Version.cpp`
- Create: `src/tools/veritas-build.cpp`
- Create: `src/tools/veritas-query.cpp`
- Create: `src/tools/veritas-diff.cpp`
- Create: `src/tools/veritas-explain.cpp`
- Create: `tests/unit/core/VersionTest.cpp`
- Create: `tests/unit/core/StatusTest.cpp`
- Create: `tests/fixtures/cpp/smoke/compile_commands.json`
- Create: `tests/fixtures/cpp/smoke/smoke.cpp`

## Interfaces

```cpp
namespace veritas {
enum class StatusCode {
  Ok,
  InvalidArgument,
  NotFound,
  FailedPrecondition,
  Internal
};

class Status {
 public:
  static Status Ok();
  static Status InvalidArgument(std::string message);
  static Status NotFound(std::string message);
  static Status FailedPrecondition(std::string message);
  static Status Internal(std::string message);

  bool ok() const;
  StatusCode code() const;
  std::string_view message() const;
};

template <typename T>
class StatusOr {
 public:
  StatusOr(T value);
  StatusOr(Status status);
  bool ok() const;
  const Status& status() const;
  const T& value() const;
  T& value();
};

struct Version {
  int major;
  int minor;
  int patch;
  std::string git_revision;
};

Version GetVersion();
std::string FormatVersion(const Version& version);
}
```

CLI contracts:

```text
veritas-build --version
veritas-query --version
veritas-diff --version
veritas-explain --version
```

Each command prints the same version string and exits zero.

## Implementation Plan

- [ ] Create the CMake skeleton with targets `veritas_core`, `veritas-build`, `veritas-query`, `veritas-diff`, and `veritas-explain`.
- [ ] Add dependency discovery for LLVM, Clang, Protobuf, RocksDB, SQLite, GoogleTest, SVF, and Souffle, but make SVF and Souffle optional in M0.
- [ ] Write `StatusTest.cpp` before implementing `Status` and `StatusOr`.
- [ ] Implement the minimal `Status` and `StatusOr` API used by later milestones.
- [ ] Write `VersionTest.cpp` before implementing `FormatVersion`.
- [ ] Implement `GetVersion` and `FormatVersion`.
- [ ] Wire each CLI to `--version`.
- [ ] Add the smoke fixture with a minimal `compile_commands.json`.
- [ ] Run `cmake -S . -B build -DVERITAS_BUILD_TESTS=ON`.
- [ ] Run `cmake --build build`.
- [ ] Run `ctest --test-dir build --output-on-failure`.
- [ ] Commit with message `build: add VERITAS C++ skeleton`.

## Tests

```cpp
TEST(VersionTest, FormatsSemanticVersion) {
  veritas::Version version{0, 1, 0, "dev"};
  EXPECT_EQ(veritas::FormatVersion(version), "VERITAS 0.1.0 (dev)");
}
```

## Exit Criteria

```text
All four CLI binaries build.
All four CLI binaries print a version.
ctest passes.
No production code depends on SVF or Souffle yet.
```

---

# 5. M1: Build Intelligence and Program Context

## Design Spec

M1 ingests a compilation database and creates a deterministic analysis manifest. The manifest defines repository, revision, build variant, and translation unit identities before any program facts are emitted.

Clang LibTooling should be used for compilation database parsing because it already supports CMake-generated `compile_commands.json` and the same command semantics Clang tools use. VERITAS should normalize the result into its own manifest so downstream systems never depend directly on Clang's transient command objects.

## Files

- Create: `include/veritas/build/ProgramContext.h`
- Create: `include/veritas/build/CompilationDatabaseLoader.h`
- Create: `src/build/ProgramContext.cpp`
- Create: `src/build/CompilationDatabaseLoader.cpp`
- Create: `tests/unit/build/ProgramContextTest.cpp`
- Create: `tests/integration/build/CompilationDatabaseLoaderTest.cpp`
- Modify: `src/tools/veritas-build.cpp`

## Interfaces

```cpp
namespace veritas::build {
struct ProgramContext {
  std::string repository_id;
  std::string revision_id;
  std::string build_variant_id;
  std::string root_path;
  std::string target_triple;
  std::string compiler_id;
  std::string compiler_version;
};

struct TranslationUnitCommand {
  std::string translation_unit_id;
  std::string source_path;
  std::vector<std::string> arguments;
  std::string command_hash;
};

struct AnalysisManifest {
  ProgramContext context;
  std::vector<TranslationUnitCommand> translation_units;
};

AnalysisManifest LoadCompilationDatabase(
    const std::filesystem::path& compile_database_path,
    const ProgramContext& base_context);
}
```

CLI contract:

```text
veritas-build configure --compile-db <path> --output <manifest.json>
```

## Implementation Plan

- [ ] Write unit tests for deterministic `ProgramContext` serialization.
- [ ] Write an integration test that loads `tests/fixtures/cpp/smoke/compile_commands.json`.
- [ ] Implement a thin wrapper around `clang::tooling::JSONCompilationDatabase`.
- [ ] Normalize all source paths relative to repository root.
- [ ] Hash compile command arguments in canonical order.
- [ ] Emit `AnalysisManifest` as diagnostic JSON in M1.
- [ ] Add `veritas-build configure`.
- [ ] Run unit and integration tests.
- [ ] Commit with message `feat: add build manifest ingestion`.

## Tests

The integration test should assert:

```text
one translation unit is discovered
source path is repository-relative
command hash is stable across repeated loads
manifest JSON is byte-identical across repeated writes
```

## Exit Criteria

```text
veritas-build configure reads compile_commands.json.
The emitted manifest is deterministic.
No function identity or summary storage is introduced in this milestone.
```

---

# 6. M2: Identity, Canonical Hashing, and Metadata Store

## Design Spec

M2 implements the ID model from the backbone spec: repository, revision, build variant, function symbol, function variant, function body, analyzer run, and fact identities. It also creates the SQLite metadata schema.

VERITAS owns this layer. Reused tools can provide names, USRs, source locations, target triples, and type-layout data, but VERITAS must define the canonical bytes that become IDs.

## Files

- Create: `include/veritas/core/Hash.h`
- Create: `include/veritas/core/Ids.h`
- Create: `include/veritas/core/CanonicalJson.h`
- Create: `include/veritas/summarydb/MetadataStore.h`
- Create: `src/core/Hash.cpp`
- Create: `src/core/Ids.cpp`
- Create: `src/core/CanonicalJson.cpp`
- Create: `src/summarydb/MetadataStore.cpp`
- Create: `src/summarydb/schema/v1.sql`
- Create: `tests/unit/core/HashTest.cpp`
- Create: `tests/unit/core/IdsTest.cpp`
- Create: `tests/unit/summarydb/MetadataStoreTest.cpp`

## Interfaces

```cpp
namespace veritas::core {
enum class IdKind {
  Repository,
  Revision,
  BuildVariant,
  FunctionSymbol,
  FunctionVariant,
  FunctionBody,
  FunctionSummary,
  AnalyzerRun,
  Fact
};

struct StableId {
  IdKind kind;
  std::string algorithm;
  std::string digest_hex;
};

StableId MakeStableId(IdKind kind, std::span<const std::byte> canonical_bytes);
std::string ToString(const StableId& id);
StableId ParseStableId(std::string_view text);
}
```

```cpp
namespace veritas::summarydb {
class MetadataStore {
 public:
  static veritas::StatusOr<MetadataStore> Open(const std::filesystem::path& db_path);
  veritas::Status ApplySchema();
  veritas::Status PutRepository(const RepositoryRow& row);
  veritas::Status PutRevision(const RevisionRow& row);
  veritas::Status PutBuildVariant(const BuildVariantRow& row);
};
}
```

## Implementation Plan

- [ ] Write hash tests for stable SHA-256 output and ID prefixes.
- [ ] Write canonical JSON tests for map sorting and timestamp exclusion.
- [ ] Implement `MakeStableId`, `ToString`, and `ParseStableId`.
- [ ] Add SQLite schema matching the identity tables in the backbone spec.
- [ ] Implement `MetadataStore::ApplySchema`.
- [ ] Add transaction tests that insert repository, revision, and build rows.
- [ ] Add duplicate insert tests that prove idempotency.
- [ ] Run unit tests.
- [ ] Commit with message `feat: add stable identity metadata`.

## Tests

Required cases:

```text
same canonical input -> same ID
different kind prefix -> different string, same digest allowed
unordered map fields -> same canonical bytes
absolute local path field -> rejected from semantic hash input
duplicate metadata insert -> no corruption
```

## Exit Criteria

```text
SQLite metadata schema is applied from a fresh DB.
Stable IDs are parseable and round-trip.
The code can store repository/revision/build context from M1.
```

---

# 7. M3: Summary IR and Immutable CAS Object Store

## Design Spec

M3 defines the first Protobuf Summary IR and stores immutable summary objects by content hash. The summary may contain synthetic facts at first; real extraction arrives in M4 and M5.

The important contract is not analysis precision yet. It is immutability, component hashes, semantic/evidence hash split, and atomic metadata publication.

## Files

- Create: `proto/veritas/summary/v1/summary.proto`
- Create: `include/veritas/summary/FunctionSummary.h`
- Create: `include/veritas/summary/ComponentHash.h`
- Create: `include/veritas/summarydb/ObjectStore.h`
- Create: `include/veritas/summarydb/SummaryRepository.h`
- Create: `src/summary/FunctionSummary.cpp`
- Create: `src/summary/ComponentHash.cpp`
- Create: `src/summarydb/ObjectStoreRocksDb.cpp`
- Create: `src/summarydb/SummaryRepository.cpp`
- Create: `tests/unit/summary/ComponentHashTest.cpp`
- Create: `tests/unit/summarydb/ObjectStoreTest.cpp`
- Create: `tests/unit/summarydb/SummaryRepositoryTest.cpp`

## Interfaces

```cpp
namespace veritas::summary {
enum class ComponentKind {
  Calls,
  MemoryEffects,
  ValueFlow,
  RangeFacts,
  AliasFacts,
  Taint,
  Ownership,
  Locks,
  State,
  Unknowns,
  Assumptions,
  Dependencies,
  Provenance
};

struct ComponentDigest {
  ComponentKind kind;
  core::StableId semantic_hash;
  core::StableId evidence_hash;
  int item_count;
};

std::vector<ComponentDigest> ComputeComponentDigests(
    const veritas::summary::v1::FunctionSummary& summary);
}
```

```cpp
namespace veritas::summarydb {
class ObjectStore {
 public:
  virtual veritas::Status PutIfAbsent(std::string_view key, std::span<const std::byte> bytes) = 0;
  virtual veritas::StatusOr<std::vector<std::byte>> Get(std::string_view key) const = 0;
};
}
```

## Implementation Plan

- [ ] Write `summary.proto` with header, identity, component hash, calls, memory effects, value flows, ranges, aliases, unknowns, dependencies, and provenance refs.
- [ ] Generate Protobuf C++ bindings through CMake.
- [ ] Write tests for component hash stability.
- [ ] Implement canonical summary serialization for hashing.
- [ ] Implement RocksDB `ObjectStore`.
- [ ] Implement `SummaryRepository::PublishSummary` with SQLite transaction boundaries.
- [ ] Add tests for CAS deduplication.
- [ ] Add crash-simulation tests using an injected failing object store.
- [ ] Run unit tests.
- [ ] Commit with message `feat: add immutable summary store`.

## Tests

Required cases:

```text
same summary bytes -> same FunctionSummaryID
range-only change -> only range component semantic hash changes
provenance-only change -> semantic hash stable, evidence hash changes
object written before failed metadata transaction -> no current binding
duplicate object put -> one stored object
```

## Exit Criteria

```text
Synthetic FunctionSummary objects can be published and retrieved.
Component digests are persisted.
Publication is atomic at current bindings.
```

---

# 8. M4: Clang/LLVM Local Extraction Adapter

## Design Spec

M4 extracts function symbols, source anchors, direct calls, CFG/dominator facts, local memory operations, and LLVM value references from real C/C++ code.

Reuse strategy:

- Use Clang LibTooling for AST, declarations, USRs, templates, macros, and source mapping.
- Use LLVM IR and analysis APIs for SSA-level instructions, CFG, dominators, MemorySSA, and alias-query integration points.
- Do not make Clang AST nodes or LLVM `Value*` pointers persistent IDs. Convert them into VERITAS stable references within a translation unit and function variant.

## Files

- Create: `include/veritas/frontend/clang/ClangExtractor.h`
- Create: `include/veritas/frontend/clang/SourceAnchorBuilder.h`
- Create: `include/veritas/analysis/llvm/LlvmExtractor.h`
- Create: `include/veritas/analysis/llvm/ValueRef.h`
- Create: `src/frontend/clang/ClangExtractor.cpp`
- Create: `src/frontend/clang/SourceAnchorBuilder.cpp`
- Create: `src/analysis/llvm/LlvmExtractor.cpp`
- Create: `src/analysis/llvm/ValueRef.cpp`
- Create: `tests/integration/frontend/ClangExtractorTest.cpp`
- Create: `tests/integration/analysis/LlvmExtractorTest.cpp`
- Modify: `src/tools/veritas-build.cpp`

## Interfaces

```cpp
namespace veritas::frontend::clang {
struct ExtractedFunctionDecl {
  core::StableId function_symbol_id;
  std::string qualified_name;
  std::string mangled_name;
  std::string canonical_signature;
  std::string linkage_kind;
  core::StableId source_anchor_id;
};

class ClangExtractor {
 public:
  veritas::StatusOr<std::vector<ExtractedFunctionDecl>> ExtractDeclarations(
      const build::TranslationUnitCommand& command);
};
}
```

```cpp
namespace veritas::analysis::llvm {
struct LocalIrFacts {
  std::vector<summary::CallFact> direct_calls;
  std::vector<summary::MemoryEffectFact> memory_effects;
  std::vector<summary::ValueFlowFact> local_value_flows;
  std::vector<summary::RangeFact> range_facts;
  std::vector<summary::UnknownFact> unknowns;
};

class LlvmExtractor {
 public:
  veritas::StatusOr<LocalIrFacts> ExtractLocalFacts(
      const build::TranslationUnitCommand& command,
      const core::StableId& function_variant_id);
};
}
```

## Implementation Plan

- [ ] Add fixture functions covering direct calls, static functions, overloads, templates, macros, and a simple `memcpy`.
- [ ] Write Clang extractor tests for qualified names, signatures, linkage, and source anchors.
- [ ] Implement declaration extraction through LibTooling `FrontendAction`.
- [ ] Write LLVM extractor tests for direct call edges and basic memory effects.
- [ ] Implement IR module loading or generation from the compile command.
- [ ] Implement local CFG and dominator fact extraction.
- [ ] Implement local memory read/write detection using LLVM memory instructions and MemorySSA where available.
- [ ] Normalize extracted facts into M3 `FunctionSummary`.
- [ ] Add `veritas-build index --local-only`.
- [ ] Run integration tests on fixtures.
- [ ] Commit with message `feat: extract local Clang LLVM summaries`.

## Tests

Required fixture assertions:

```text
overloaded functions have distinct FunctionSymbolIDs
static internal-linkage functions in different files do not collide
macro-expanded call has spelling and expansion source anchors
direct call edge has MUST_CALL
unresolved function pointer call emits UNKNOWN_CALL
memcpy callsite is represented as a callsite fact
```

## Exit Criteria

```text
veritas-build index --local-only emits real local summaries.
No SVF dependency is required for this milestone.
Instruction-level graph data is not persisted globally.
```

---

# 9. M5: SVF Value-Flow and Pointer Adapter

## Design Spec

M5 introduces SVF behind an adapter to produce higher-quality pointer, alias, and value-flow facts. SVF should be treated as an analysis provider, not as VERITAS's internal data model.

VERITAS should map SVF nodes and edges into:

```text
ValueRef
MemoryRef
ValueFlowFact
AliasFact
MemoryEffectFact
UnknownFact
```

The adapter must preserve uncertainty:

```text
MustAlias
MayAlias
NoAlias
UnknownAlias
```

No consumer outside `analysis/svf` should include SVF headers.

## Files

- Create: `include/veritas/analysis/svf/SvfAdapter.h`
- Create: `include/veritas/analysis/svf/SvfConfig.h`
- Create: `src/analysis/svf/SvfAdapter.cpp`
- Create: `src/analysis/svf/SvfConfig.cpp`
- Create: `tests/integration/analysis/SvfAdapterTest.cpp`
- Modify: `cmake/FindSVF.cmake`
- Modify: `src/tools/veritas-build.cpp`

## Interfaces

```cpp
namespace veritas::analysis::svf {
struct SvfConfig {
  std::string pointer_analysis_level;  // basic, andersen, demand
  int max_analysis_seconds;
  bool emit_unknowns_on_timeout;
};

struct SvfFacts {
  std::vector<summary::ValueFlowFact> value_flows;
  std::vector<summary::AliasFact> aliases;
  std::vector<summary::MemoryEffectFact> refined_memory_effects;
  std::vector<summary::UnknownFact> unknowns;
};

class SvfAdapter {
 public:
  veritas::StatusOr<SvfFacts> AnalyzeModule(
      const LlvmModuleInput& module,
      const SvfConfig& config);
};
}
```

## Implementation Plan

- [ ] Add CMake detection for SVF include directories and libraries.
- [ ] Add `VERITAS_ENABLE_SVF` build option.
- [ ] Write tests that are skipped with a clear message when SVF is unavailable.
- [ ] Build a fixture with pointer assignment, field load/store, and parameter-to-return flow.
- [ ] Implement SVF module ingestion from LLVM bitcode.
- [ ] Map SVF value-flow nodes to VERITAS `ValueRef`.
- [ ] Map SVF points-to/alias answers to VERITAS alias facts.
- [ ] Merge SVF facts with M4 local summaries without replacing Clang source anchors.
- [ ] Emit `UnknownFact` on timeout or unsupported construct.
- [ ] Run SVF integration tests.
- [ ] Commit with message `feat: add SVF value-flow adapter`.

## Tests

Required fixture assertions:

```text
arg0 -> return flow is emitted as ValueFlowFact
store then load through pointer emits may-flow with alias provenance
field-sensitive fact preserves field path when SVF provides enough detail
timeout emits UnknownFact instead of dropping facts silently
SVF disabled build still compiles all non-SVF targets
```

## Exit Criteria

```text
SVF is optional at build time.
When enabled, SVF improves value-flow and alias components.
No public VERITAS API exposes SVF-native node IDs.
```

---

# 10. M6: Thin VERITAS CPG Projection

## Design Spec

M6 builds the VERITAS CPG as a projection over SummaryDB facts, not as the primary source of truth. It should be thin:

Persistent nodes:

```text
TranslationUnit
Namespace
Type
Function
Parameter
Global
CallSite
MemoryObject
Field
BasicBlockSummary
```

Persistent edges:

```text
CONTAINS
DECLARES
CALLS
MAY_CALL
READS
WRITES
FLOWS_TO
MAY_ALIAS
DOMINATES_SUMMARY
SUMMARIZED_BY
```

Instruction-level nodes are generated on demand from Clang/LLVM or cached for a specific Evidence Case. This avoids turning the graph store into a giant duplicated compiler IR database.

Joern's CPG and the CPG specification are schema inspiration. VERITAS should not copy Joern storage or require Joern as a runtime dependency for V1.

## Files

- Create: `proto/veritas/cpg/v1/cpg.proto`
- Create: `include/veritas/cpg/ThinCpg.h`
- Create: `include/veritas/cpg/CpgBuilder.h`
- Create: `include/veritas/cpg/CpgQuery.h`
- Create: `src/cpg/ThinCpg.cpp`
- Create: `src/cpg/CpgBuilder.cpp`
- Create: `src/cpg/CpgQuery.cpp`
- Create: `tests/unit/cpg/ThinCpgTest.cpp`
- Create: `tests/integration/cpg/CpgBuilderTest.cpp`
- Modify: `src/tools/veritas-query.cpp`

## Interfaces

```cpp
namespace veritas::cpg {
enum class NodeKind {
  TranslationUnit,
  Type,
  Function,
  Parameter,
  Global,
  CallSite,
  MemoryObject,
  Field,
  BasicBlockSummary
};

enum class EdgeKind {
  Contains,
  Declares,
  Calls,
  MayCall,
  Reads,
  Writes,
  FlowsTo,
  MayAlias,
  DominatesSummary,
  SummarizedBy
};

class CpgQuery {
 public:
  std::vector<CpgNode> GetCallees(core::StableId function_variant_id) const;
  std::vector<CpgPath> GetValueFlow(core::StableId src, core::StableId dst, int max_depth) const;
  std::vector<CpgNode> GetWriters(core::StableId memory_object_id) const;
};
}
```

## Implementation Plan

- [ ] Write thin CPG schema with stable VERITAS IDs as node IDs.
- [ ] Write graph unit tests for node/edge insertion and deduplication.
- [ ] Implement `CpgBuilder` from current summary bindings.
- [ ] Add query indexes for outgoing call edges, incoming memory writers, and value-flow adjacency.
- [ ] Add `veritas-query callees <function>`.
- [ ] Add `veritas-query flow <src> <dst> --max-depth N`.
- [ ] Add tests proving instruction-level LLVM values are not persisted as global CPG nodes.
- [ ] Run CPG tests.
- [ ] Commit with message `feat: add thin CPG projection`.

## Tests

Required cases:

```text
two summaries with same function node do not duplicate node
CALLS edge can cite source callsite anchor
FLOWS_TO path can traverse summary edges
unknown call creates MAY_CALL or unknown node, not full graph fanout
instruction detail request returns unsupported until Evidence expansion milestone
```

## Exit Criteria

```text
Thin CPG can answer caller/callee and value-flow adjacency queries.
Persistent graph size is proportional to functions, callsites, objects, and summary edges, not every instruction.
```

---

# 11. M7: Reverse Dependency Index and Incremental Scheduler

## Design Spec

M7 implements semantic incremental recomputation. It consumes summary component deltas and schedules only consumers that depend on changed producer components.

The dependency index must support:

```text
producer_kind
producer_id
producer_component
consumer_kind
consumer_id
consumer_component
sensitivity
```

This milestone is the backbone's scale hinge. A range-only change should not invalidate call graph consumers. A provenance-only change should not invalidate semantic WPA consumers.

## Files

- Create: `include/veritas/summarydb/SummaryDelta.h`
- Create: `include/veritas/summarydb/DependencyIndex.h`
- Create: `include/veritas/runtime/WorklistScheduler.h`
- Create: `src/summarydb/SummaryDelta.cpp`
- Create: `src/summarydb/DependencyIndex.cpp`
- Create: `src/runtime/WorklistScheduler.cpp`
- Create: `tests/unit/summarydb/SummaryDeltaTest.cpp`
- Create: `tests/unit/summarydb/DependencyIndexTest.cpp`
- Create: `tests/unit/runtime/WorklistSchedulerTest.cpp`
- Modify: `src/tools/veritas-diff.cpp`

## Interfaces

```cpp
namespace veritas::summarydb {
struct ComponentDelta {
  summary::ComponentKind component_kind;
  core::StableId old_semantic_hash;
  core::StableId new_semantic_hash;
  core::StableId old_evidence_hash;
  core::StableId new_evidence_hash;
};

struct SummaryDelta {
  core::StableId function_variant_id;
  core::StableId old_summary_id;
  core::StableId new_summary_id;
  std::vector<ComponentDelta> changed_components;
};

class DependencyIndex {
 public:
  veritas::Status ReplaceCurrentDependencies(
      core::StableId consumer_summary_id,
      std::vector<DependencyEdge> edges);
  veritas::StatusOr<std::vector<ConsumerRef>> UsersOf(
      core::StableId producer_id,
      summary::ComponentKind producer_component) const;
};
}
```

```cpp
namespace veritas::runtime {
class WorklistScheduler {
 public:
  void Enqueue(WorkItem item);
  std::optional<WorkItem> PopNext();
  bool Empty() const;
};
}
```

## Implementation Plan

- [ ] Write component diff tests using synthetic summaries.
- [ ] Implement `DiffSummaries(old, fresh)`.
- [ ] Add SQLite tables for `summary_deltas`, `component_deltas`, and `reverse_dependency_index`.
- [ ] Write reverse index tests for producer-component lookup.
- [ ] Implement stale edge reconciliation when a current summary changes.
- [ ] Implement worklist deduplication by kind, target, revision, build variant, and consumer component.
- [ ] Add `veritas-diff <old> <new>` diagnostic output for changed components and scheduled consumers.
- [ ] Add fixture mutation tests for comment-only, range-only, call-only, and provenance-only changes.
- [ ] Run unit and integration tests.
- [ ] Commit with message `feat: add incremental dependency scheduler`.

## Tests

Required cases:

```text
range-only delta schedules range_wpa and taint_wpa, not scc
call-only delta schedules scc and call_graph consumers
provenance-only delta schedules evidence_slice only
old reverse index rows are removed from current hot index
historical dependency rows remain available for old summary explanation
```

## Exit Criteria

```text
veritas-diff reports semantic component deltas.
Consumer scheduling is narrower than all-callers invalidation.
```

---

# 12. M8: SCC-Aware WPA and Souffle Fact Engine

## Design Spec

M8 computes whole-program facts over current summaries. It starts with a C++ SCC/fixpoint engine for transitive calls and may-read/may-write, then exports base relations to Souffle for recursive fact derivation once the schemas are stable.

SCCs should include `MUST_CALL` and `MAY_CALL`. `UNKNOWN_CALL` must not connect to every function. It should produce unknown external summary facts and conservative evidence.

Souffle should be used for recursive relations such as reachability and transitive effects. VERITAS owns tuple IDs, epistemic joins, provenance nodes, and publication.

## Files

- Create: `include/veritas/wpa/CallGraph.h`
- Create: `include/veritas/wpa/SccGraph.h`
- Create: `include/veritas/wpa/FixpointEngine.h`
- Create: `include/veritas/facts/FactSchema.h`
- Create: `include/veritas/facts/SouffleExporter.h`
- Create: `src/wpa/CallGraph.cpp`
- Create: `src/wpa/SccGraph.cpp`
- Create: `src/wpa/FixpointEngine.cpp`
- Create: `src/facts/FactSchema.cpp`
- Create: `src/facts/SouffleExporter.cpp`
- Create: `src/facts/rules/reachability.dl`
- Create: `src/facts/rules/memory_effects.dl`
- Create: `tests/unit/wpa/SccGraphTest.cpp`
- Create: `tests/unit/wpa/FixpointEngineTest.cpp`
- Create: `tests/integration/facts/SouffleExporterTest.cpp`

## Interfaces

```cpp
namespace veritas::wpa {
class SccGraph {
 public:
  static SccGraph Build(const CallGraph& call_graph);
  core::StableId SccForFunction(core::StableId function_variant_id) const;
  std::vector<core::StableId> Members(core::StableId scc_id) const;
  std::vector<core::StableId> Predecessors(core::StableId scc_id) const;
};

class FixpointEngine {
 public:
  veritas::StatusOr<SccResult> Compute(
      core::StableId scc_id,
      summary::ComponentKind component_kind);
};
}
```

```cpp
namespace veritas::facts {
struct FactTuple {
  core::StableId tuple_id;
  std::string predicate_kind;
  std::vector<std::string> fields;
  std::string epistemic;
  core::StableId provenance_id;
};

class SouffleExporter {
 public:
  veritas::Status WriteBaseRelations(const std::filesystem::path& directory);
  veritas::StatusOr<std::vector<FactTuple>> ReadDerivedRelations(
      const std::filesystem::path& directory);
};
}
```

## Implementation Plan

- [ ] Write SCC tests for acyclic graph, self-recursion, mutual recursion, and unknown call.
- [ ] Implement call graph loading from summary call components.
- [ ] Implement Tarjan or Kosaraju SCC construction.
- [ ] Implement condensation DAG.
- [ ] Write fixpoint tests for transitive calls and may-write.
- [ ] Implement monotone set-union domains for transitive calls and may-write.
- [ ] Persist SCC state hashes and iteration counts.
- [ ] Write Souffle base relation exporter tests.
- [ ] Add `reachability.dl` and `memory_effects.dl` with tuple ID fields.
- [ ] Capture derived tuple inputs when the rule engine can provide them; otherwise reconstruct immediate derivations for V1 rules.
- [ ] Run WPA and fact tests.
- [ ] Commit with message `feat: add SCC WPA fact engine`.

## Tests

Required cases:

```text
recursive pair converges
unknown call does not create whole-program SCC
internal SCC change with same external hash stops propagation
MayWrite(A, X) derived through A -> B -> C with provenance inputs
Souffle disabled build still supports C++ fixpoint tests
```

## Exit Criteria

```text
WPA can derive transitive call and may-write facts.
Derived facts carry enough provenance hooks for M9.
SCC state hashes participate in incremental propagation.
```

---

# 13. M9: Provenance-Aware Fact Store and Explain API

## Design Spec

M9 publishes current facts and provenance DAGs. Every non-trivial derived fact must answer "why is this true?" within a budgeted explanation.

The fact store separates:

```text
FactID: exact fact in one program context with provenance
semantic_fact_hash: cross-revision semantic equivalence without revision/provenance
provenance_id: derivation node root
```

Epistemic propagation must be conservative. Inferred or assumed inputs cannot silently produce verified `MUST` facts.

## Files

- Create: `proto/veritas/fact/v1/fact.proto`
- Create: `include/veritas/facts/FactStore.h`
- Create: `include/veritas/facts/ProvenanceStore.h`
- Create: `include/veritas/facts/Epistemic.h`
- Create: `src/facts/FactStore.cpp`
- Create: `src/facts/ProvenanceStore.cpp`
- Create: `src/facts/Epistemic.cpp`
- Create: `tests/unit/facts/FactStoreTest.cpp`
- Create: `tests/unit/facts/ProvenanceStoreTest.cpp`
- Create: `tests/unit/facts/EpistemicTest.cpp`
- Modify: `src/tools/veritas-explain.cpp`

## Interfaces

```cpp
namespace veritas::facts {
enum class EpistemicState {
  Must,
  May,
  MustNot,
  Inferred,
  Assumed,
  Unknown
};

EpistemicState JoinEpistemic(
    EpistemicState lhs,
    EpistemicState rhs,
    RuleSoundness soundness);

class FactStore {
 public:
  veritas::Status PublishFacts(std::vector<FactTuple> facts);
  veritas::StatusOr<std::vector<FactTuple>> GetFactsBySubject(core::StableId subject_id) const;
};

class ProvenanceStore {
 public:
  veritas::Status PutNode(ProvenanceNode node);
  veritas::Status PutEdge(ProvenanceEdge edge);
  veritas::StatusOr<ProvenanceGraph> Explain(core::StableId provenance_id, ExplainBudget budget) const;
};
}
```

CLI contract:

```text
veritas-explain fact <fact_id> --max-depth 5 --max-nodes 100
```

## Implementation Plan

- [ ] Write Protobuf fact/provenance messages.
- [ ] Add SQLite fact and provenance tables.
- [ ] Write epistemic join tests for MUST, MAY, UNKNOWN, ASSUMED, and INFERRED inputs.
- [ ] Implement `JoinEpistemic`.
- [ ] Implement fact publication with current fact replacement.
- [ ] Implement provenance node/edge insertion.
- [ ] Implement budgeted recursive explanation.
- [ ] Add `veritas-explain fact`.
- [ ] Add integration test explaining `may_write` through a call chain.
- [ ] Run fact/provenance tests.
- [ ] Commit with message `feat: add provenance fact store`.

## Tests

Required cases:

```text
MUST + sound rule -> MUST
MAY + sound rule -> MAY
INFERRED input remains INFERRED unless verifier result is present
ASSUMED input appears in explanation
explain budget truncates graph with explicit truncation marker
same semantic fact with different provenance has distinct FactID
```

## Exit Criteria

```text
Every M8 derived fact can be published.
Every current derived fact can be explained.
Epistemic state is preserved through derivation.
```

---

# 14. M10: Evidence Builder Input APIs and First Demo

## Design Spec

M10 does not implement full Evidence IR. It exposes the semantic slices that Evidence IR needs:

```text
value-flow slice
call path
range facts
memory facts
aliases
dominating checks
unknowns
provenance closure
```

The first demo should be a buffer-overflow style fixture:

```text
packet.len -> decode arg -> copy length -> memcpy size
dst capacity = 2048
range(packet.len) = [0, 65535]
no dominating range check
```

This milestone proves that SummaryDB, thin CPG, WPA facts, and provenance can feed an EIR-L1 case without loading full source text into an agent prompt.

## Files

- Create: `include/veritas/evidence/EvidenceQueryService.h`
- Create: `include/veritas/evidence/SliceTypes.h`
- Create: `src/evidence/EvidenceQueryService.cpp`
- Create: `src/evidence/SliceTypes.cpp`
- Create: `tests/fixtures/cpp/overflow/packet.cpp`
- Create: `tests/fixtures/cpp/overflow/packet.h`
- Create: `tests/integration/evidence/EvidenceQueryServiceTest.cpp`
- Modify: `src/tools/veritas-query.cpp`

## Interfaces

```cpp
namespace veritas::evidence {
struct FlowSlice {
  std::vector<cpg::CpgNode> nodes;
  std::vector<cpg::CpgEdge> edges;
  std::vector<facts::FactTuple> supporting_facts;
  std::vector<facts::FactTuple> unknowns;
};

struct EvidenceQueryBudget {
  int max_depth;
  int max_nodes;
  int max_paths;
};

class EvidenceQueryService {
 public:
  veritas::StatusOr<FlowSlice> GetValueFlow(
      core::StableId src,
      core::StableId dst,
      EvidenceQueryBudget budget) const;

  veritas::StatusOr<std::vector<facts::FactTuple>> GetRanges(core::StableId value_ref) const;
  veritas::StatusOr<std::vector<facts::FactTuple>> GetAliases(core::StableId memory_ref) const;
  veritas::StatusOr<std::vector<facts::FactTuple>> GetUnknowns(core::StableId scope_ref) const;
};
}
```

CLI contract:

```text
veritas-query evidence overflow --sink memcpy --format json
```

## Implementation Plan

- [ ] Add overflow fixture with unsafe and safe variants.
- [ ] Write an integration test that indexes the unsafe fixture.
- [ ] Implement `EvidenceQueryService::GetValueFlow` over thin CPG adjacency plus summary edges.
- [ ] Implement range, alias, and unknown lookup through FactStore.
- [ ] Attach provenance IDs to slice facts.
- [ ] Add JSON diagnostic output for `veritas-query evidence`.
- [ ] Test that unsafe fixture emits a flow to `memcpy.size` and missing dominating check evidence.
- [ ] Test that safe fixture contains a dominating bounds check fact.
- [ ] Run full integration suite.
- [ ] Commit with message `feat: add evidence query slices`.

## Tests

Required cases:

```text
unsafe fixture returns packet.length -> memcpy.size flow
unsafe fixture returns range wider than destination capacity
unsafe fixture returns missing or non-dominating check evidence
safe fixture returns dominating check fact
flow slice includes provenance IDs
slice budget truncates paths explicitly
```

## Exit Criteria

```text
One command produces EIR-ready evidence inputs for a memory-safety demo.
No LLM agent is required.
The slice is semantic and compact.
```

---

# 15. Cross-Milestone Review Gates

Each milestone should pass these gates before moving on:

```text
1. All tests for the milestone pass.
2. New public interfaces have unit tests.
3. Golden fixture output is deterministic.
4. New persistent schemas have migration or fresh-create tests.
5. Any third-party dependency is behind an adapter.
6. No downstream module includes SVF, Clang, or LLVM headers unless it is inside the matching adapter subtree.
7. Any MAY/UNKNOWN/INFERRED fact remains epistemically visible.
8. CLI output includes enough diagnostics to debug fixture failures.
```

The most important architectural review question after each milestone is:

> Did VERITAS preserve its own stable semantic contract, or did it leak a third-party tool's internal model into the backbone?

If the answer is the latter, stop and add an adapter boundary before proceeding.

---

# 16. Recommended Commit Sequence

```text
M0  build: add VERITAS C++ skeleton
M1  feat: add build manifest ingestion
M2  feat: add stable identity metadata
M3  feat: add immutable summary store
M4  feat: extract local Clang LLVM summaries
M5  feat: add SVF value-flow adapter
M6  feat: add thin CPG projection
M7  feat: add incremental dependency scheduler
M8  feat: add SCC WPA fact engine
M9  feat: add provenance fact store
M10 feat: add evidence query slices
```

Each commit should be reviewable alone. A reviewer should be able to checkout any milestone commit and run its documented tests.

---

# 17. Source References

- Clang LibTooling: `https://clang.llvm.org/docs/LibTooling.html`
- LLVM Alias Analysis infrastructure: `https://llvm.org/docs/AliasAnalysis.html`
- LLVM MemorySSA: `https://llvm.org/docs/MemorySSA.html`
- SVF static value-flow framework: `https://github.com/SVF-tools/SVF`
- Souffle Datalog tool for static analysis: `https://souffle-lang.github.io/`
- Joern Code Property Graph docs: `https://docs.joern.io/code-property-graph/`
- Code Property Graph specification: `https://cpg.joern.io/`
- CodeQL C/C++ data-flow guide: `https://codeql.github.com/docs/codeql-language-guides/analyzing-data-flow-in-cpp/`

These references are implementation inputs, not VERITAS API contracts. VERITAS APIs should remain stable if one of these tools is upgraded or replaced.
