# VERITAS Backbone Milestone Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Break the VERITAS engineering backbone into small, testable milestones, each with a design spec and implementation plan.

**Architecture:** VERITAS owns the complete analysis workflow. The current pre-M11 Tier-1 route starts at one project directory and performs compilation-database ingestion plus Clang AST/LLVM IR generation. M11 adds mutually exclusive Tier-2 `.bc`/`.ll` module acquisition. Both routes continue through required in-process SVF, durable Function Summary IR publication, persistence, incremental invalidation, run-local WPA projection, provenance, and Evidence Builder APIs. Pinned SVF owns V1 points-to/alias/SVFG and indirect-call truth; compiled Souffle owns normal production recursive WPA; C++ is a conformance oracle or explicitly selected emergency engine. Third-party types remain behind private stages and never bypass VERITAS analysis.

**Tech Stack:** C++20, CMake 3.23+, LLVM/Clang 22+, Clang LibTooling and CodeGen, SVF commit `18fb5650600530a54f0afc22f4df1a10b03d3c02`, Z3, Protobuf, RocksDB, SQLite, compiled Souffle 2.5 source revision `5682a9f12e2668ecdd26348fe63cc508bc0fcf47` with verified executable/toolchain provenance, GoogleTest, and Python helper scripts for golden fixture checks.

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
- `veritas-build analyze --project <directory>` is the only public source input and the current pre-M11 contract; `<directory>/compile_commands.json` must exist.
- M11 adds mutually exclusive `veritas-build analyze --bitcode <.bc|.ll|directory>` as a Tier-2 module-acquisition input. It skips only Clang CodeGen and then uses the same VERITAS-owned local extraction, required SVF, Summary IR, WPA, and provenance pipeline; it never accepts an SVF artifact or analysis result.
- M12 external facts are non-authoritative terminal observations and never become Summary IR or recursive-WPA inputs.
- SVF is required, lives at `third_party/SVF`, and is pinned to `18fb5650600530a54f0afc22f4df1a10b03d3c02`; no SVF-disabled standard build exists.
- The first storage stack is RocksDB for immutable objects and SQLite for metadata.
- Function Summary IR is the durable WPA contract; typed `relations.v2` rows and dense IDs are run-local execution projections.
- Compiled Souffle is the required normal production recursive-WPA engine after M8R.4; C++ consumes the same byte-identical `WpaLogicalComponentInput` only for conformance or explicit `cpp-emergency` use, under a distinct valid envelope and `RunId`.
- Every engine toolchain record has a required canonical engine-specific provenance payload/hash. Before production Souffle execution, VERITAS parses the configured install-provenance manifest, requires version 2.5 at source revision `5682a9f12e2668ecdd26348fe63cc508bc0fcf47`, hashes the configured executable, and verifies it against the manifest; its payload includes manifest, executable, generated-bundle, and generator/compiler/link provenance. C++ conformance/`cpp-emergency` instead records exact C++ build identity and never reuses or impersonates Souffle provenance.
- Automatic engine fallback is forbidden. Failed components publish no replacement and retain the last successful result only as stale history.
- Component reuse is content-addressed by engine-neutral logical input plus exact executor/toolchain identity.
- Canonical `FactID` hashes only `relations.v2`, relation name, typed stable semantic cells, and epistemic value; revision/build/run/engine/dense/tuple/rule/witness/provenance context is stored separately and never re-identifies an incoming Fact Bus fact.
- Every derived fact has a generic deterministic finite witness rooted in stable input fact IDs.
- `AnalysisFactBatch` is the only M9 input and must prove expected/completed component equality, rooted-input closure, and idempotent Fact Bus delivery.
- M9 starts only after all ten M8R executable gates pass without missing, extra, disabled, skipped, failed, or errored tests.

---

# 1. Source-Backed Reuse Strategy

The backbone should not reimplement mature static-analysis infrastructure unless VERITAS needs a semantic contract those tools do not expose.

| Area | Reuse | VERITAS Owns | Rationale |
| --- | --- | --- | --- |
| C/C++ parsing, AST, macros, source locations, compilation database | Clang 22 LibTooling | Project-level orchestration, stable extraction adapters, and source anchors | VERITAS calls `FrontendAction`s itself from the project compilation database. |
| LLVM IR, SSA, MemorySSA, alias hooks, dominators, range-ish local facts | LLVM 22 libraries | Tier-1 in-process IR generation/linking, M11 Tier-2 module acquisition, private `ProgramIr`, and normalization into Summary IR | Both input tiers remain behind VERITAS-owned module verification, lifetime, extraction, and identity boundaries. |
| Pointer/value-flow, VFG, and AndersenWaveDiff analysis | Required pinned SVF Git submodule | Direct library invocation, stable value IDs, component hashes, lifecycle cleanup, and uncertainty mapping | SVF supplies mature analysis while VERITAS owns its input module, execution, and outputs. |
| Recursive relations and transitive WPA facts | Souffle | Fact schemas, tuple IDs, provenance capture, publication | Souffle is designed as a Datalog tool for static analysis; VERITAS should add provenance and epistemic policy around it. |
| CPG concepts and schema vocabulary | Joern CPG/spec | Thin persistent VERITAS CPG projection | The architecture wants CPG benefits without storing a full instruction-level universal graph for every repo. |
| Security query idioms and data-flow test comparisons | CodeQL documentation and optional baseline runs | VERITAS-native summaries, dependencies, and evidence | CodeQL is useful as a conceptual and fixture baseline, but VERITAS should not make CodeQL DBs the core SummaryDB. |

The source architecture says "Build a VERITAS CPG, but keep it thin." This document interprets the user's "GPG" wording as "CPG" because the existing docs consistently discuss Code Property Graphs.

---

# 2. Milestone Map

| Milestone | Name | Primary Deliverable | Depends On |
| --- | --- | --- | --- |
| M0 | Project skeleton and required toolchain harness | Buildable repo with pinned SVF submodule and LLVM/Clang/SVF compatibility checks | none |
| M1 | Project ingestion and program context | Project-level request, revision/build context, typed translation-unit manifest | M0 |
| M2 | Identity, canonical hashing, and metadata store | Stable IDs and SQLite metadata schema | M1 |
| M3 | Summary IR and CAS object store | Protobuf summaries, component hashes, RocksDB CAS | M2 |
| M4 | VERITAS-owned Clang/LLVM project analysis | AST facts, linked private `ProgramIr`, and local summary drafts | M3 |
| M5 | Required in-process SVF analysis | Required SVF facts merged and published from the project pipeline | M4 |
| M6 | Thin VERITAS CPG projection | Function/object-centric graph projection and query API | M4, M5 |
| M7 | Reverse dependency index and incremental scheduler | Component deltas and precise consumer scheduling | M3, M4, M5 |
| M8 | SCC-aware WPA and Souffle fact engine (implemented history) | C++ recursive facts plus optional Souffle comparison | M7 |
| M8R.1 | Semantic Fact Contract | Typed semantic values, run manifests, relation registry, stable/dense maps | M8 |
| M8R.2 | SVF and Memory Refinement | Native `summary.v2`, indirect calls, collision-free abstract memory | M8R.1 |
| M8R.3 | Relational WPA Projection | Engine-neutral SCC input, `relations.v2`, rooted witnesses | M8R.2 |
| M8R.4 | Production Souffle WPA | Required compiled engine, exact provenance, atomic failure/reuse | M8R.3 |
| M8R.5 | Qualification and M9 Handoff | Conformance corpus, `AnalysisFactBatch`, Fact Bus, ten-criterion entry gate with exact expected test-name membership per criterion | M8R.4 |
| M9 | Provenance-aware fact store and explain API | Run/fact/witness/diagnostic persistence and `veritas-explain` | M8R.5 gate |
| M10A | Recursive domain expansion | `MayRead`, `GlobalFlow`, `UnknownEffect`, `SoundnessCoverage` | M9 |
| M10B | Evidence Builder input APIs and first demo | EIR-ready slices over M9 facts and M10A relations | M6, M9, M10A |
| M11 | External IR adapter | External bitcode/textual IR through the same summary boundary | M5 |
| M12 | External facts importer | Provenance-tagged non-authoritative imported facts | M9 |
| M13 | Benchmark-gated PTA research | Independent Souffle-native PTA comparison against pinned SVF | independent of M9-M12 |

Each milestone should merge independently. Every milestone has tests and a small user-visible CLI behavior.

M11 and M12 remain chronologically placed after M10B, but chronology is not a
functional dependency: M11 reuses the M5 local/SVF pipeline, while M12 requires
the M9 fact and provenance stores.

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
| M8R.1-M8R.5 | `docs/specs/milestones/m8r-souffle-wpa-remediation-design-spec.md` |
| M9 | `docs/specs/milestones/m9-provenance-fact-store-explain-api-design-spec.md` |
| M10A | Detailed recursive-domain-expansion spec required before implementation |
| M10B | `docs/specs/milestones/m10-evidence-builder-input-apis-demo-design-spec.md` |

Historical per-milestone implementation plans live under `docs/plans/`. The
M8R executable plan is
`docs/superpowers/plans/2026-08-22-souffle-wpa-remediation-bridge-implementation-plan.md`;
M10A still requires its detailed plan, and M13's independently approved
research plan is outside the M9-M12 critical path.

---

# 3. Proposed Repository Structure

```text
veritas/
  .gitmodules
  CMakeLists.txt
  cmake/
    Dependencies.cmake
    VerifySvfSubmodule.cmake
    VeritasWarnings.cmake

  third_party/
    SVF/                       pinned Git submodule

  docs/third_party/
    SVF.md

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
    analysis/                  project-level public API only
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
    analysis/pipeline/
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
    fixtures/projects/
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

M0 creates a buildable C++20 repository and the required compiler-analysis dependency contract, but no production analysis behavior. LLVM/Clang 22+ comes from one configured installation. SVF is source-pinned at `third_party/SVF` and is always part of the standard build; Souffle remains optional until M8.

The exact SVF contract is:

```text
upstream: https://github.com/SVF-tools/SVF.git
revision: 18fb5650600530a54f0afc22f4df1a10b03d3c02
path: third_party/SVF
minimum CMake: 3.23
LLVM/Clang: 22+, shared with VERITAS
```

## Files

- Create: `CMakeLists.txt`
- Create: `.gitmodules`
- Create: `cmake/Dependencies.cmake`
- Create: `cmake/VerifySvfSubmodule.cmake`
- Create: `cmake/VeritasWarnings.cmake`
- Add: `third_party/SVF` Git submodule at `18fb5650600530a54f0afc22f4df1a10b03d3c02`
- Create: `docs/third_party/SVF.md`
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
- Create: `tests/fixtures/projects/smoke/compile_commands.json`
- Create: `tests/fixtures/projects/smoke/smoke.cpp`

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

- [ ] Add and pin the required dependency exactly:

```bash
git submodule add https://github.com/SVF-tools/SVF.git third_party/SVF
git -C third_party/SVF checkout 18fb5650600530a54f0afc22f4df1a10b03d3c02
git add .gitmodules third_party/SVF
```

- [ ] Create the CMake skeleton with targets `veritas_core`, `veritas-build`, `veritas-query`, `veritas-diff`, and `veritas-explain`.
- [ ] Require CMake 3.23+, LLVM/Clang 22+, Protobuf, RocksDB, SQLite, GoogleTest, and Z3; keep only Souffle optional in M0.
- [ ] Fail configuration when the SVF submodule is absent with `git submodule update --init --recursive third_party/SVF`.
- [ ] Add SVF with `add_subdirectory(third_party/SVF EXCLUDE_FROM_ALL)` and create private interface target `veritas_third_party_svf` linking `SvfCore` and `SvfLLVM`.
- [ ] Verify VERITAS and SVF use the same LLVM version, RTTI, exception, target, and ABI settings.
- [ ] Write `StatusTest.cpp` before implementing `Status` and `StatusOr`.
- [ ] Implement the minimal `Status` and `StatusOr` API used by later milestones.
- [ ] Write `VersionTest.cpp` before implementing `FormatVersion`.
- [ ] Implement `GetVersion` and `FormatVersion`.
- [ ] Wire each CLI to `--version`.
- [ ] Add the smoke fixture with a minimal `compile_commands.json`.
- [ ] Run `cmake -S . -B build -DVERITAS_BUILD_TESTS=ON`.
- [ ] Run `cmake --build build`.
- [ ] Run `ctest --test-dir build --output-on-failure`.
- [ ] Preserve `third_party/SVF/LICENSE.TXT` and record upstream, revision, AGPL-3.0-or-later license, toolchain, and initialization command in `docs/third_party/SVF.md`.
- [ ] Commit with message `build: add required SVF toolchain`.

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
The standard build always provides SvfCore, SvfLLVM, and veritas_third_party_svf from the pinned submodule.
No production analysis invokes SVF until M5; Souffle remains optional until M8.
```

---

# 5. M1: Project Ingestion and Program Context

## Design Spec

M1 accepts one project directory, resolves only `<project>/compile_commands.json`, and creates the typed deterministic analysis manifest used directly by M2 and M4. Diagnostic JSON is optional output; it is never a public input to another stage.

## Files

- Create: `include/veritas/analysis/ProjectAnalysisRequest.h`
- Create: `include/veritas/build/ProjectInput.h`
- Create: `include/veritas/build/AnalysisManifest.h`
- Create: `include/veritas/build/ProjectManifestLoader.h`
- Create: `src/build/ProjectInput.cpp`
- Create: `src/build/AnalysisManifest.cpp`
- Create: `src/build/ProjectManifestLoader.cpp`
- Create: `tests/unit/build/ProjectInputTest.cpp`
- Create: `tests/unit/build/AnalysisManifestTest.cpp`
- Create: `tests/integration/build/ProjectManifestLoaderTest.cpp`
- Create: `tests/integration/build/VeritasBuildAnalyzeCliTest.cpp`
- Modify: `src/tools/veritas-build.cpp`

## Interfaces

```cpp
namespace veritas::build {
enum class PathRootKind {
  kRepository,
  kGenerated,
  kExternal,
  kToolchain,
};

struct TaggedPath {
  PathRootKind root_kind;
  std::string root_id;
  std::filesystem::path relative_path;
};

struct ProjectInput {
  std::filesystem::path project_root;
  std::filesystem::path compile_database_path;
  std::filesystem::path output_root;
};

struct ProgramContext {
  std::string repository_id;
  std::string revision_id;
  std::string build_variant_id;
  std::filesystem::path project_root;
  std::string target_triple;
  std::string compiler_id;
  std::string compiler_version;
};

struct TranslationUnitCommand {
  std::string translation_unit_id;
  TaggedPath source_path;
  TaggedPath working_directory;
  std::vector<std::string> arguments;
  std::string command_hash;
};

struct AnalysisManifest {
  ProgramContext context;
  std::vector<TranslationUnitCommand> translation_units;
};

StatusOr<ProjectInput> ResolveProjectInput(
    const analysis::ProjectAnalysisRequest& request);
StatusOr<AnalysisManifest> LoadProjectManifest(const ProjectInput& input);
}
```

CLI contract:

```text
veritas-build analyze --project <directory> [--output <directory>]
```

## Implementation Plan

- [ ] Resolve and validate the project root, fixed compilation-database path, and default `.veritas` output root.
- [ ] Define tagged paths plus typed context, translation-unit, and manifest records.
- [ ] Load all commands through Clang `JSONCompilationDatabase` and fail the project when any required source is missing.
- [ ] Normalize path roots and ordered semantic arguments, then compute domain-separated SHA-256 hashes.
- [ ] Implement deterministic canonical bytes and one-way diagnostic JSON.
- [ ] Add `veritas-build analyze --project`; reject manifest, bitcode, LLVM-module, SVF-artifact, and alternate compile-database flags.
- [ ] Follow `docs/plans/m1-build-intelligence-program-context-implementation-plan.md` for test-first task details and commits.

## Tests

The integration test should assert:

```text
project root resolves exactly <project>/compile_commands.json
reordered compilation entries produce identical canonical bytes
repository-relative paths remain stable across checkout roots
missing compilation database or source fails without a partial manifest
public command parsing exposes only the project-level source input
```

## Exit Criteria

```text
veritas-build analyze --project loads the fixed compilation database into a typed deterministic manifest.
Downstream stages receive that manifest in memory and never read a manifest file.
No AST, LLVM IR, or SVF artifact is accepted from the user.
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
  ControlFlow,
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

- [ ] Write `summary.proto` with header, identity, component hash, calls, memory effects, value flows, versioned `BasicBlockSummaryRef`/dominator control-flow summaries, ranges, aliases, unknowns, dependencies, and provenance refs.
- [ ] Generate Protobuf C++ bindings through CMake.
- [ ] Write tests for component hash stability.
- [ ] Test that control-flow topology changes only the `ControlFlow` semantic hash, source/provenance display changes only its evidence hash, and reordered block-summary records preserve both hashes.
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
control-flow topology change -> only ControlFlow semantic hash changes
control-flow source-display change -> only ControlFlow evidence hash changes
reordered BasicBlockSummaryRef records -> ControlFlow hashes remain stable
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

# 8. M4: VERITAS-Owned Clang/LLVM Project Analysis

## Design Spec

M4 consumes the live M1 manifest, runs Clang AST and CodeGen library stages for every translation unit, links one private in-memory `ProgramIr`, extracts local facts, and returns unpublished summary drafts plus the live module to M5.

Reuse strategy:

- Use Clang 22 LibTooling and CodeGen from normalized M1 commands for AST, declarations, USRs, templates, macros, source mapping, and IR emission.
- Use LLVM 22 libraries for in-process module linking, SSA facts, CFG, dominators, and MemorySSA.
- Do not make Clang AST nodes or LLVM `Value*` pointers persistent IDs. Convert them into VERITAS stable references within a translation unit and function variant.
- Do not accept manifests, bitcode, or LLVM-module paths and do not require user-invoked compiler tools.

## Files

- Create: `src/frontend/clang/ProjectAstExtractor.h`
- Create: `src/frontend/clang/ProjectAstExtractor.cpp`
- Create: `src/frontend/clang/SourceAnchorBuilder.cpp`
- Create: `src/analysis/pipeline/ProgramIr.h`
- Create: `src/analysis/pipeline/ProgramIr.cpp`
- Create: `src/analysis/pipeline/LocalAnalysisStage.h`
- Create: `src/analysis/pipeline/LocalAnalysisStage.cpp`
- Create: `src/analysis/llvm/ProjectIrBuilder.h`
- Create: `src/analysis/llvm/ProjectIrBuilder.cpp`
- Create: `src/analysis/llvm/OriginMap.h`
- Create: `src/analysis/llvm/OriginMap.cpp`
- Create: `src/analysis/llvm/LocalFactExtractor.h`
- Create: `src/analysis/llvm/LocalFactExtractor.cpp`
- Create: `tests/integration/frontend/ProjectAstExtractorTest.cpp`
- Create: `tests/integration/analysis/ProjectIrBuilderTest.cpp`
- Create: `tests/integration/analysis/LocalAnalysisStageTest.cpp`

## Interfaces

```cpp
namespace veritas::analysis::pipeline {
struct LocalAnalysisResult {
  ProgramIr program_ir;
  std::vector<summary::v1::FunctionSummary> summary_drafts;
};

StatusOr<LocalAnalysisResult> RunLocalAnalysis(
    const build::AnalysisManifest& manifest);
}
```

## Implementation Plan

- [ ] Extract project-wide declarations and source anchors through private Clang frontend actions.
- [ ] Generate a module for every normalized command and link one move-only private `ProgramIr`.
- [ ] Build the LLVM-to-VERITAS origin map and deterministic module hash.
- [ ] Extract direct calls, local CFG/dominator facts, memory effects, value flows, ranges, and scoped unknowns.
- [ ] Return local summary drafts and the live `ProgramIr` without publishing before M5.
- [ ] Follow `docs/plans/m4-clang-llvm-local-extraction-implementation-plan.md` for test-first task details and commits.

## Tests

Required fixture assertions:

```text
all manifest translation units are processed or analysis fails
overloaded functions have distinct FunctionSymbolIDs
static internal-linkage functions in different files do not collide
macro-expanded call has spelling and expansion source anchors
direct call edge has MUST_CALL
unresolved function pointer call emits UNKNOWN_CALL
memcpy callsite is represented as a callsite fact
multi-translation-unit input produces one linked in-memory ProgramIr
no public manifest, bitcode, or LLVM-module input exists
```

## Exit Criteria

```text
M4 builds deterministic local summary drafts and a live linked ProgramIr from the M1 manifest.
The standard pipeline does not claim completion or publish drafts until required M5 SVF succeeds.
No instruction-level graph data or native pointer identity is persisted globally.
```

---

# 9. M5: Required In-Process SVF Analysis

## Design Spec

M5 completes the standard project pipeline by running the pinned SVF libraries directly on M4's live `ProgramIr`, mapping results to VERITAS facts, merging them with local drafts, and publishing only after required SVF success.

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

No installed public header includes SVF or LLVM native types. SVF headers and lifecycle calls remain private to `src/analysis/svf`.

## Files

- Create: `include/veritas/analysis/ProjectAnalyzer.h`
- Create: `src/analysis/ProjectAnalyzer.cpp`
- Create: `src/analysis/svf/CMakeLists.txt`
- Create: `src/analysis/svf/SvfSession.h`
- Create: `src/analysis/svf/SvfSession.cpp`
- Create: `src/analysis/svf/SvfFactMapper.h`
- Create: `src/analysis/svf/SvfFactMapper.cpp`
- Create: `src/analysis/svf/SvfAnalysisStage.h`
- Create: `src/analysis/svf/SvfAnalysisStage.cpp`
- Create: `src/analysis/svf/SvfConfig.cpp`
- Create: `tests/integration/analysis/svf/SvfSessionTest.cpp`
- Create: `tests/integration/analysis/svf/SvfFactMapperTest.cpp`
- Create: `tests/integration/analysis/ProjectAnalyzerSvfTest.cpp`
- Modify: `src/tools/veritas-build.cpp`

## Interfaces

```cpp
namespace veritas::analysis::svf {
enum class PointerAnalysisKind { kAndersenWaveDiff };

struct SvfConfig {
  PointerAnalysisKind pointer_analysis;
  std::chrono::seconds soft_analysis_budget;
  std::size_t max_graph_nodes;
  std::size_t max_emitted_facts;
  bool field_sensitive;
};

struct SvfFacts {
  std::vector<summary::ValueFlowFact> value_flows;
  std::vector<summary::AliasFact> aliases;
  std::vector<summary::MemoryEffectFact> refined_memory_effects;
  std::vector<summary::CallFact> refined_calls;
  std::vector<summary::UnknownFact> unknowns;
  std::vector<summary::DependencyEdge> dependencies;
};
}
```

## Implementation Plan

- [ ] Require M0's pinned `SvfCore`/`SvfLLVM` wrapper and expose no enable/disable switch.
- [ ] Build an RAII serialized SVF session with `LLVMModuleSet::buildSVFModule(program_ir.module())`, `SVFIRBuilder`, `AndersenWaveDiff`, and `SVFGBuilder`.
- [ ] Release Andersen, SVFIR, and LLVMModuleSet singleton state on every return path.
- [ ] Resolve SVF values through LLVM values and M4 origins, then map value-flow, alias, memory, indirect-call, dependency, unknown, and provenance facts.
- [ ] Preserve validated partial facts plus explicit truncation unknowns at supported soft-budget checkpoints.
- [ ] Merge conservatively, publish only after required SVF completion, and route `analyze --project` through `ProjectAnalyzer`.
- [ ] Follow `docs/plans/m5-svf-value-flow-pointer-adapter-implementation-plan.md` for test-first task details and commits.

## Tests

Required fixture assertions:

```text
arg0 -> return flow is emitted as ValueFlowFact
store then load through pointer emits may-flow with alias provenance
field-sensitive fact preserves field path when SVF provides enough detail
supported soft-budget truncation emits UnknownFact instead of dropping facts silently
two analyses in one process release singleton state and produce deterministic facts
fatal SVF construction failure publishes no summaries
the standard build has no SVF-disabled configuration
```

## Exit Criteria

```text
The pinned SVF submodule is required by the standard build and full analysis.
One project-directory command owns compilation-database ingestion, AST/IR construction, SVF execution, and publication.
No public VERITAS API exposes prebuilt IR inputs, SVF-native types, or SVF node IDs.
```

---

# 10. M6: LLVM-Native Thin VERITAS CPG Projection

## Design Spec

M6 builds a VERITAS-owned CPG directly from the live linked M4 `ProgramIr` and the completed in-memory summaries produced after required M5 mapping. It runs as a private C++ stage before `ProgramIr` destruction and before current bindings are published. It does not invoke Joern, PhASAR, an external CPG service, a compiler executable, or an artifact-driven analysis path.

Persistent nodes:

```text
Function
Parameter
Global
CallSite
MemoryObject
BasicBlockSummary
Summary
Unknown
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
ALIASES
DOMINATES_SUMMARY
SUMMARIZED_BY
UNKNOWN_AT
```

`ALIASES` carries the exact mapped state `MustAlias`, `MayAlias`, `NoAlias`, or `UnknownAlias` for M5-evaluated candidate pairs. M6 runs no second pointer analysis. A fixture-justified, clean-room PhASAR-inspired refinement belongs in a versioned M5 stage before summary completion and must emit only VERITAS `AliasFact` values.

Instruction-level nodes and native LLVM/SVF identity are never persisted globally. Source anchors, translation-unit IDs, declared type strings, qualified names, and memory field paths remain properties when M4/M5 already mapped them; M6 V1 does not invent source-semantic node identity that the handoff does not provide.

Every persistent node has an explicit mapped identity: Function uses `FunctionVariantID`; Parameter uses `ValueRef`; Global and MemoryObject use `MemoryRef`; CallSite uses `CallSiteID`; BasicBlockSummary uses M4's canonical `BasicBlockSummaryID`; Summary uses `FunctionSummaryID`; and Unknown uses a canonical scoped hash. A missing mapping produces `UNKNOWN_AT`, never an LLVM ordinal or pointer-derived ID.

## Files

- Create: `proto/veritas/cpg/v1/cpg.proto`
- Create: `include/veritas/cpg/CpgTypes.h`
- Create: `include/veritas/cpg/ThinCpg.h`
- Create: `include/veritas/cpg/CpgQuery.h`
- Create: `include/veritas/cpg/CpgRepository.h`
- Create: `src/analysis/cpg/CpgProjectionStage.cpp`
- Create: `src/cpg/ThinCpg.cpp`
- Create: `src/cpg/CpgCanonicalizer.cpp`
- Create: `src/cpg/CpgRepository.cpp`
- Create: `src/cpg/CpgQuery.cpp`
- Create: `include/veritas/summarydb/ProjectPublicationCoordinator.h`
- Create: `src/summarydb/ProjectPublicationCoordinator.cpp`
- Create: `tests/unit/cpg/ThinCpgTest.cpp`
- Create: `tests/integration/analysis/cpg/CpgProjectionStageTest.cpp`
- Create: `tests/integration/summarydb/ProjectPublicationCoordinatorTest.cpp`
- Modify: `src/tools/veritas-query.cpp`

## Interfaces

```cpp
namespace veritas::cpg {
enum class NodeKind {
  Function,
  Parameter,
  Global,
  CallSite,
  MemoryObject,
  BasicBlockSummary,
  Summary,
  Unknown
};

enum class EdgeKind {
  Contains,
  Declares,
  Calls,
  MayCall,
  Reads,
  Writes,
  FlowsTo,
  Aliases,
  DominatesSummary,
  SummarizedBy,
  UnknownAt
};

template <typename T>
struct TraversalResult {
  std::vector<T> items;
  std::vector<TruncationReason> truncation_reasons;
  std::size_t explored_nodes;
  std::size_t explored_paths;
};
}
```

## Implementation Plan

- [ ] Define stable CPG nodes, edges, support records, four-state aliases, canonical bytes, and `ProjectionID`.
- [ ] Build and validate `ThinCpg` from the borrowed live `ProgramIr` plus completed in-memory summaries.
- [ ] Add SQLite projection, node, edge, adjacency, historical, and current-binding rows.
- [ ] Stage summary bindings and the CPG binding in one project-publication transaction.
- [ ] Reject publication before the transaction unless graph and completed-summary revision/build/module identities and exact sorted `FunctionSummaryID` sets match.
- [ ] Bind each `CpgQuery` to one immutable `ProjectionID` and return explicit traversal truncation metadata.
- [ ] Add caller/callee, writer, value-flow, call-path, determinism, failure-injection, and ownership-boundary tests.
- [ ] Follow `docs/plans/m6-thin-veritas-cpg-projection-implementation-plan.md` for test-first tasks and commits.

## Tests

Required cases:

```text
the projector accepts live ProgramIr plus completed summaries and no artifact path
identical inputs produce the same ProjectionID and canonical graph bytes
CALLS/FLOWS_TO/ALIASES edges retain summary and opaque mapped provenance support
all four M5 alias states survive without semantic upgrade or all-pairs fanout
unknown calls terminate at bounded UNKNOWN_AT/MAY_CALL relations
projection failure advances neither summary nor CPG current bindings
query results distinguish no path from each exhausted budget
using a budget exactly does not report truncation unless additional eligible work is rejected
public headers and the standard build contain no native analysis or external CPG-generator boundary
```

## Exit Criteria

```text
One `analyze --project` invocation publishes completed summaries and one matching immutable CPG snapshot.
Thin CPG answers caller/callee, writer, value-flow, alias, and call-path queries with explicit budgets.
Persistent graph size is proportional to functions, callsites, objects, and summary relations, not LLVM instruction count.
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

# 12A. M8R.1-M8R.5: Souffle WPA Remediation Bridge

The historical M8 section above describes what was implemented. The approved
[M8R bridge](milestones/m8r-souffle-wpa-remediation-design-spec.md) is inserted
after M8 and before M9 and does not rewrite that history.

The five gates deliver, in order:

1. typed semantic/run/relation contracts with stable and run-local dense
   identity and complete negative/unknown transport;
2. native `summary.v2` with SVF-owned indirect-call/alias/SVFG results and
   collision-free `AbstractObject + AccessPath + ByteRange` memory identity;
3. canonical engine-neutral per-SCC `WpaLogicalComponentInput`, run-local
   `relations.v2`, successor support, and generic finite rooted witnesses using
   an injective semantic-key codec;
4. compiled Souffle production recursion, pinned to version 2.5 source revision
   `5682a9f12e2668ecdd26348fe63cc508bc0fcf47` with configured install-manifest
   parsing, executable-digest verification, and generated-toolchain provenance,
   plus C++ conformance/explicit emergency only, no automatic fallback,
   content-addressed reuse, and failure atomicity;
5. differential/failure/performance qualification and the complete, rooted,
   idempotently delivered `AnalysisFactBatch`/Fact Bus handoff.

M9 begins only when all ten executable entry criteria pass with exact expected
test membership and no missing, extra, disabled, skipped, failed, or errored
tests. The bridge spec is the canonical delivery record; until implementers
fill reviewed commits and exact test labels, every M8R gate remains a target,
not a shipped claim.

---

# 13. M9: Provenance-Aware Fact Store and Explain API

## Design Spec

M9 persists complete validated `AnalysisFactBatch` values, current and
historical facts, generic rooted witness DAGs, diagnostics, hashes, and stale
state. Every non-trivial derived fact must answer "why is this true?" within a
budgeted explanation. Raw `FactTuple` vectors are not an M9 input.

The fact store separates:

```text
FactID: hash(relations.v2, relation name, typed stable semantic cells, epistemic)
(RunId, FactID): occurrence/history/current binding
WitnessID: selected or alternative derivation bound to one (RunId, FactID)
```

Revision/build/run/engine identity, dense IDs, tuple order, producer, scope,
rule, witness, provenance, and derivation are excluded from `FactID`. M9
validates and persists incoming Fact Bus IDs rather than re-identifying them.
The same semantic row may have different witnesses in different runs while
retaining one canonical fact/root identity.

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
  veritas::Status PublishBatch(AnalysisFactBatch batch);
  veritas::StatusOr<std::vector<Fact>> GetFactsBySubject(core::StableId subject_id) const;
};

class ProvenanceStore {
 public:
  veritas::Status PutWitness(FactWitness witness);
  veritas::Status PutEdge(FactWitnessEdge edge);
  veritas::StatusOr<ProvenanceGraph> Explain(
      core::StableId run_id,
      core::StableId fact_id,
      ExplainBudget budget) const;
};
}
```

CLI contract:

```text
veritas-explain fact <fact_id> --run <run_id> --max-depth 5 --max-nodes 100
```

## Implementation Plan

- [ ] Write Protobuf fact/provenance messages.
- [ ] Add SQLite canonical-fact, run-fact-binding, and witness tables.
- [ ] Write epistemic join tests for MUST, MAY, UNKNOWN, ASSUMED, and INFERRED inputs.
- [ ] Implement `JoinEpistemic`.
- [ ] Implement fact publication without re-identifying incoming facts; replace only current run bindings.
- [ ] Reject expected/completed component mismatch and unrooted witness leaves.
- [ ] Make `(RunId, BatchId)` delivery idempotent across partial multi-sink retry.
- [ ] Preserve incomplete-run diagnostics and the previous success as stale history.
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
same semantic row with different witness has the same FactID and distinct witness/run bindings
witness-only change may alter FixpointHash but not canonical fact/root IDs or ExternalHash
```

## Exit Criteria

```text
Every accepted AnalysisFactBatch can be published atomically and idempotently.
Every current derived fact can be explained.
Epistemic state is preserved through derivation.
```

---

# 14A. M10A: Recursive Domain Expansion

M10A extends the compiled-Souffle production WPA with independently versioned
`MayRead`, `GlobalFlow`, `UnknownEffect`, and `SoundnessCoverage` rule bundles,
models, golden cases, C++/Souffle conformance coverage, rooted witnesses, and
incremental `ExternalHash` behavior. It preserves the M8R logical-input,
toolchain-identity, failure-atomicity, and Fact Bus contracts. M10A requires a
separate detailed design and implementation plan before work begins.

---

# 14B. M10B: Evidence Builder Input APIs and First Demo

## Design Spec

M10B does not implement full Evidence IR. It exposes semantic slices over M9
facts and M10A relations:

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
  std::vector<facts::Fact> supporting_facts;
  std::vector<facts::Fact> unknowns;
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

  veritas::StatusOr<std::vector<facts::Fact>> GetRanges(core::StableId value_ref) const;
  veritas::StatusOr<std::vector<facts::Fact>> GetAliases(core::StableId memory_ref) const;
  veritas::StatusOr<std::vector<facts::Fact>> GetUnknowns(core::StableId scope_ref) const;
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
6. Installed public headers expose no SVF, Clang, or LLVM native types; those headers remain in private `src/frontend/clang`, `src/analysis/llvm`, and `src/analysis/svf` stages.
7. Any MAY/UNKNOWN/INFERRED fact remains epistemically visible.
8. CLI output includes enough diagnostics to debug fixture failures.
```

The most important architectural review question after each milestone is:

> Did VERITAS preserve its own stable semantic contract, or did it leak a third-party tool's internal model into the backbone?

If the answer is the latter, stop and add an adapter boundary before proceeding.

---

# 16. Recommended Commit Sequence

```text
M0  build: add required SVF toolchain
M1  feat: accept project directory for analysis
M2  feat: add stable identity metadata
M3  feat: add immutable summary store
M4  feat: prepare local analysis for required SVF
M5  feat: require SVF in project analysis
M6  feat: add thin CPG projection
M7  feat: add incremental dependency scheduler
M8  feat: add SCC WPA fact engine
M8R remediation commits: semantic contract, SVF/memory refinement,
     relational projection, production Souffle, qualification/M9 handoff
M9  feat: add provenance fact store
M10A feat: expand recursive WPA domains
M10B feat: add evidence query slices
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
