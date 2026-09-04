# WPA and SummaryDB End-to-End Qualification Corpus Design Specification

**Status:** Approved design; implementation pending

**Scope:** M8R.4 orchestration completion and M8R.5 qualification

**Primary inputs:** Mixed C11/C++20 projects with `compile_commands.json`

**Production WPA domains:** `ReachableCall` and `MayWrite`

## 1. Purpose

This specification defines a source-driven qualification corpus for the full
VERITAS native analysis path:

```text
C/C++ source + compilation database
  -> Clang/LLVM module acquisition
  -> SVF AndersenWaveDiff and SVFG mapping
  -> Function Summary IR v2
  -> atomic SummaryDB publication
  -> call graph and SCC construction
  -> relations.v2 component materialization
  -> compiled Souffle execution
  -> canonical facts and rooted witnesses
  -> persisted WPA run and component state
```

The corpus must expose incorrect memory abstraction, identity collisions,
alias-state loss, incomplete indirect-call resolution, virtual-dispatch loss,
incorrect SCC scheduling, mismatched Souffle semantics, non-determinism, and
failure-atomicity violations. Passing compilation or producing non-empty
output is insufficient; every case has an explicit semantic oracle.

This work also completes the missing production orchestration needed to make
the standard `veritas-build analyze --project <dir>` workflow execute WPA after
publishing `summary.v2` and the thin CPG.

## 2. Current baseline and boundary

At the design baseline, `main` at `8faf463` provides:

- required in-process SVF analysis and normalized V2 facts;
- native `summary.v2` construction and version-aware SummaryDB storage;
- stable value and structured abstract-memory identity;
- version-neutral call and SCC graphs;
- typed `relations.v2` and engine-neutral `WpaLogicalComponentInput`;
- compiled Souffle executors for `ReachableCall` and `MayWrite`;
- a C++ conformance executor consuming the same logical input;
- generic semantic-key encoding and rooted-witness canonicalization.

The baseline does not yet connect standard `ProjectAnalyzer` execution to
durable WPA run orchestration. Run/component lifecycle persistence, complete
reverse-topological orchestration, and standard analyzer engine selection are
therefore prerequisites of the end-to-end qualification path.

The implemented Souffle bundles recursively derive only `ReachableCall` and
`MayWrite`. `DirectRead`, structured memory, dispatch kinds, models, and
explicit unknowns remain typed `relations.v2` values where their current
component materializer admits them. Alias values are qualified through SVF,
`summary.v2`, the relation schema, and relation-I/O round trips; they do not
enter a logical component merely to exercise an unused row. This work does not
add new recursive read, alias, or value-flow domains. Those remain M10A scope.

## 3. Goals

The implementation must:

1. Exercise C and C++ translation units in one linked `ProgramIr`.
2. Cover global, stack, heap, argument, function, external, subobject, array,
   overlapping, and unknown memory abstractions.
3. Preserve structural access paths and distinguish known zero ranges from
   unknown ranges.
4. Exercise `MustAlias`, `MayAlias`, `NoAlias`, and explicit unknown alias
   outcomes without conflating alias kind and epistemic state.
5. Exercise direct, indirect, callback, virtual, recursive, cross-TU, modeled,
   and unknown-external calls.
6. Exercise single inheritance, multiple inheritance, and multiple admissible
   virtual targets.
7. Prove collision-free stable identities for unnamed values and distinct
   allocation sites.
8. Prove SummaryDB immutability, current/historical binding behavior, and
   atomic publication.
9. Prove typed stable/dense mapping and component ownership for every relation
   emitted into current WPA logical inputs.
10. Prove canonical Souffle/C++ agreement on the overlapping production
    domains using byte-identical logical input.
11. Prove deterministic finite rooted witnesses for every published result.
12. Prove run determinism, process cleanup, exact-engine reuse, failure
    atomicity, and bounded resource use.
13. Make every qualification test mandatory and machine-detect skipped,
    disabled, missing, extra, failed, errored, or duplicate aggregate tests.

## 4. Non-goals

This specification does not:

- add M10A `MayRead`, `GlobalFlow`, `UnknownEffect`, or soundness-coverage
  bundles;
- introduce a Souffle-native points-to analysis;
- replace pinned SVF as the V1 owner of Andersen points-to, SVFG, aliases, or
  indirect-call candidates;
- claim path, flow, context, or object sensitivity beyond the configured L1
  SVF analysis;
- add exception, RTTI, standard-library, threading, or operating-system
  dependencies to fixture sources;
- use source line numbers, raw LLVM/SVF identifiers, run-local dense IDs, or
  tuple order as test oracles;
- promote expected analysis imprecision into a false `MUST` fact.

## 5. Architecture

The implementation has three independently testable units.

### 5.1 Production WPA orchestration

`WpaRunRepository` persists immutable run manifests and separately mutable
lifecycle state. `WpaOrchestrator` owns call/SCC topology, component ordering,
logical-input materialization, exact-engine reuse, execution, validation, and
atomic result publication. `ProjectAnalyzer` invokes the orchestrator only
after summary/CPG publication succeeds.

For each analysis run the orchestrator must:

1. Load the just-published V2 artifacts for the exact revision and build
   variant.
2. Construct the call graph and SCC condensation graph.
3. Enumerate every expected `(SccId, WpaComponentKind)` pair for reachability
   and memory effects.
4. Process SCCs in reverse topological order so successor support is available
   to predecessors.
5. Materialize one immutable logical input for each component.
6. Reuse a successful result only by
   `(LogicalInputHash, EngineToolchainIdentity)` after revalidation.
7. Execute the selected engine, canonicalize results, validate schema,
   stable/dense mappings, duplicates, ownership, and witness closure, then
   atomically publish the component.
8. Mark the run complete only if the completed component set equals the
   expected component set.

### 5.2 Hybrid source corpus

Focused projects provide narrow semantic oracles. One integrated mixed-language
project, `semantic_zoo`, combines the same features across translation-unit and
language boundaries. The narrow fixtures localize failures; the integrated
fixture detects feature interactions.

### 5.3 Layered qualification harness

Tests inspect each semantic boundary independently and then follow selected
facts end to end. An extraction failure must not be misreported as a Souffle
failure, and an executor equality test must not pass because both engines saw
an empty or incomplete input.

## 6. Integrated `semantic_zoo` project

The project lives under:

```text
tests/fixtures/projects/semantic_zoo/
  compile_commands.json
  semantic_zoo.h
  c_memory.c
  c_callbacks.c
  cpp_memory.cpp
  cpp_dispatch.cpp
  recursion.cpp
  driver.cpp
```

The compilation database uses `@PROJECT_ROOT@`, `clang -std=c11` for C, and
`clang++ -std=c++20` for C++. Every command includes `-O0`, debug information,
`-fno-inline`, and the applicable `-fno-rtti` and `-fno-exceptions` flags.
Fixture files include no system headers. Required external declarations use
portable fixture-owned types and explicit `extern` or `extern "C"` signatures.

### 6.1 Shared C ABI

`semantic_zoo.h` defines the C-compatible records and entry points shared by
the C and C++ translation units:

- `ZooPayload`, containing a byte array and length;
- `ZooEnvelope`, containing nested header/payload fields;
- `ZooBuffer`, containing data, capacity, and current length;
- `ZooCallback`, a function-pointer type receiving a buffer and integer;
- declarations for callback dispatch, modeled copy, recursive entry, and the
  integrated driver.

C++ class definitions remain private to `cpp_dispatch.cpp`. C-callable wrapper
functions expose virtual-dispatch scenarios without placing C++ types in the C
header.

### 6.2 `c_memory.c`

Named functions isolate these cases:

- write a global and a translation-unit static object;
- copy a pointer and access the same object through both names;
- allocate two disjoint stack objects and access both;
- accept two unconstrained pointer parameters that may alias;
- access nested structure fields;
- access a constant array element;
- access an array through a variable index;
- load and store through `int **`;
- access overlapping union fields;
- retain a known zero offset and distinguish it from an unknown range.

The resulting facts must expose global, stack, argument, subobject, array, and
overlap/unknown cases without relying on display names as identity.

### 6.3 `c_callbacks.c`

This translation unit defines two concrete callbacks with different writes, a
global callback pointer, a callback table, and wrappers for:

- one direct call;
- a call through the global function pointer;
- a call through a callback parameter;
- selection between two callback-table entries.

The selection case must produce multiple stable `MAY` targets at one call site.
Unknown calls remain scoped and never cause whole-program fanout.

### 6.4 `cpp_memory.cpp`

This translation unit declares `malloc`, `free`, and `memcpy` directly and
defines:

- two allocation helpers with different allocation call sites;
- one helper called repeatedly from different callers but containing one
  allocation site;
- field writes to allocated objects;
- a modeled payload copy followed by deallocation;
- an indirect path from a caller to the modeled write.

Different allocation sites must have different abstract-object and memory
identities. Repeated use of one allocation site must retain its declared
allocation-site abstraction. The model bundle must contribute explicit
allocation, read, write, and deallocation observations without changing
summary identity.

### 6.5 `cpp_dispatch.cpp`

The private hierarchy contains:

- abstract base `ZooWriter` with virtual `write`;
- two concrete overriding writers with different memory effects;
- an independent tagged base;
- one multiply inherited writer;
- a nonvirtual helper method.

C-callable wrappers exercise a direct nonvirtual call, a virtual call through a
base pointer, and a selector returning one of two derived objects before a
virtual call. SVF candidates must be normalized as stable `MAY` targets with
virtual dispatch, while the nonvirtual call remains direct.

The fixture exercises SVF's internal class-hierarchy graph through the
resulting target set. It does not introduce a new durable class-hierarchy IR or
require the thin CPG to persist inheritance edges.

### 6.6 `recursion.cpp`

This translation unit provides:

- a self-recursive function with a terminating branch;
- two mutually recursive functions;
- a recursive path that reaches a memory-writing leaf;
- a call from the recursive region into another translation unit.

The self-recursive function forms a recursive single-member SCC. The mutual
pair forms one two-member SCC. Both domains must converge, and predecessor
components must consume successor support rather than claim successor facts as
locally owned.

### 6.7 `driver.cpp`

The driver creates cross-TU paths that combine:

- C callback dispatch;
- C++ virtual dispatch;
- modeled allocation/copy/deallocation;
- recursive reachability;
- direct and indirect memory writes.

It is the end-to-end qualification root. No `main` function is required because
VERITAS analyzes linked translation units rather than executing the fixture.

## 7. Focused projects

The hybrid corpus also contains these independent projects:

| Fixture | Required coverage |
| --- | --- |
| `recursive_calls` | self-recursion, mutual recursion, SCC convergence |
| `callback_dispatch` | callback parameter and global/table function pointers |
| `virtual_dispatch` | single/multiple inheritance and two admissible overrides |
| `pointer_alias` | copied pointer, disjoint stack objects, may-alias parameters, truncation unknown |
| `abstract_memory` | object kinds, nested fields, arrays, overlaps, known/unknown ranges |
| `unknown_external` | unresolved external call and explicit unknown policy |

Existing `multiple_tus`, `function_pointer`, `modeled_calls`, `field_access`,
`parameter_return`, and `store_load` fixtures remain part of the parameterized
matrix where they already provide a smaller sufficient case.

## 8. Semantic oracle matrix

### 8.1 SVF normalization

The source-boundary tests must prove:

- a copied pointer produces `MustAlias`;
- two disjoint stack allocations produce `NoAlias` with `MUST` epistemic;
- unconstrained pointer parameters retain a possible alias result;
- configured fact-budget truncation produces explicit scoped unknowns;
- direct, indirect, callback, and virtual calls retain distinct dispatch;
- indirect, callback, and virtual candidates use stable
  `FunctionVariantID` values and `MAY` epistemic;
- distinct unnamed LLVM values and allocation sites have distinct stable IDs;
- every mapped memory effect names structured memory rather than a raw pointer
  or placeholder string.

`UnknownAlias` itself is qualified with a synthetic normalized-fact and
summary/relation round-trip because no portable source pattern can require SVF
to return its unexpected/unknown alias result. The source fixture owns
`MustAlias`, `MayAlias`, and `NoAlias`; the truncation case separately owns the
explicit analysis-unknown contract.

A call through a function-pointer formal parameter is classified as callback
dispatch; a call through a global or locally selected function pointer remains
indirect dispatch. Classification follows the stable call/value origin and
must not depend on display-name heuristics.

If upstream SVF classifies a soundly possible parameter pair more precisely
than `MayAlias`, the oracle may accept the stronger semantic value only when
the fixture contract proves that strengthening sound. The test must never
rewrite an observed unknown into a positive result.

### 8.2 SummaryDB and `summary.v2`

After standard analysis, tests reopen SummaryDB and prove:

- every current native artifact is `summary.v2`;
- dispatch, alias kind, epistemic state, memory location, byte range, unknown,
  dependency, and provenance inputs survive serialization;
- distinct object/path identities remain distinct;
- byte range is excluded from memory identity while structural offsets remain
  part of the access path;
- known zero values remain known and unknown range payloads are canonical;
- summary and component IDs are insertion-order independent;
- CAS bytes are immutable and repeated publication is idempotent;
- reanalysis swaps current bindings without mutating historical bytes;
- failed WPA does not invalidate successfully published summaries.

### 8.3 `relations.v2` materialization

Materialization tests must inspect:

- `DirectCall`, `UnknownCall`, `DirectRead`, `DirectWrite`, and `ModeledEffect`
  rows emitted by the current reachability and memory components;
- `SupportReachableCall` and `SupportMayWrite` ownership boundaries;
- complete function, value, memory, call-site, and fact maps;
- rejection of missing mappings, cross-domain dense IDs, conflicting stable
  IDs, noncanonical unknown ranges, and tuple conflicts;
- logical-input byte equality across Souffle and C++ envelopes;
- logical hashes independent of source-summary and tuple insertion order.

Every dense ID used by an execution row must map back to exactly one stable ID
of the correct domain. Dense IDs never appear in persisted facts or summaries.
The `Alias` schema and its semantic cells are tested separately through
schema-validation and relation-I/O round trips so an unused alias row does not
perturb the logical hash of `ReachableCall` or `MayWrite`.

### 8.4 Recursive engine results

The production-domain oracles require:

- direct and transitive `ReachableCall` results;
- direct and transitive `MayWrite` results;
- propagation across direct, callback, virtual, recursive, and cross-TU paths;
- epistemic weakening from `MUST` to `MAY` across possible call edges;
- equality of canonical facts and external hashes between Souffle and the C++
  conformance executor;
- non-empty expected semantic results for every differential case;
- a finite, acyclic, closed witness for every canonical fact;
- deterministic proof selection independent of engine evaluation order.

### 8.5 Standard analyzer and persistence

An end-to-end analyzer test must prove:

- `AnalysisConfig::Default()` selects Souffle;
- a successful result exposes the production WPA `RunId`, engine, component
  counts, and no degraded-operation diagnostic;
- every expected component is persisted complete before the run becomes
  complete;
- an exact-engine reusable result records a reference in the new run without
  merging run history;
- `cpp-emergency` requires explicit selection, uses distinct run identity, and
  records degraded operation;
- a C++ conformance run is separate from production and cannot replace it;
- no Souffle failure invokes C++ implicitly.

## 9. Failure and publication semantics

These conditions fail the current component and mark the run incomplete:

- missing or incompatible worker/bundle;
- invalid or mismatched Souffle provenance;
- timeout, crash, signal, or resource exhaustion;
- input/output schema mismatch;
- missing, conflicting, or cross-domain identity mapping;
- inconsistent duplicate semantic rows;
- unsupported output epistemic state;
- malformed, orphaned, cyclic, or unclosed witnesses.

A failed component publishes no replacement. The last successful component
remains queryable only as stale history for the new revision or configuration.
Partial output from an incomplete run is never mixed with prior output.
Summaries and the CPG already published before WPA remain valid.

Failure injection must cover every condition above at the narrowest practical
boundary and include an aggregate test that verifies prior-success retention.

## 10. Determinism, lifecycle, and performance

Qualification includes:

- seeds 0 through 64 for input, member, and evaluation-order permutations;
- 20 complete analyses in one process;
- zero live Souffle workers after each run;
- clean SVF session state after each run;
- five warmed performance iterations with identical semantic hashes;
- compilation-database entry permutation;
- historical V1 compatibility projection and native V2 reanalysis.

The checked-in initial performance ceiling is:

```json
{
  "schema_version": "wpa-performance.v1",
  "fixture": "recursive_calls",
  "maximum_wall_time_ms": 30000,
  "maximum_peak_rss_mb": 2048,
  "maximum_output_facts": 1000000
}
```

The performance test reports the median warmed wall time and maximum observed
RSS. It fails only when a checked-in ceiling is exceeded or semantic hashes
differ. Updating a ceiling requires an explicit reviewed change with recorded
measurements; the test must not auto-relax it.

## 11. Test structure and registration

The source-boundary executables are:

- `SemanticZooSvfTest`;
- `SemanticZooSummaryDbTest`;
- `SemanticZooEndToEndTest`.

The qualification executables are:

- `WpaDifferentialQualificationTest`;
- `WpaDeterminismQualificationTest`;
- `WpaFailureQualificationTest`;
- `WpaMigrationQualificationTest`;
- `WpaPerformanceQualificationTest`.

The M9 criterion registry remains authoritative:

| Criterion label | Exact aggregate tests |
| --- | --- |
| `summary-v2` | `FunctionSummaryV2Test`, `SummaryRepositoryVersionTest` |
| `indirect-calls` | `SvfFactMapperV2Test`, `CallGraphTest` |
| `stable-identity` | `StableValueIdentityTest`, `AbstractMemoryBuilderTest`, `DenseIdMapTest` |
| `relations-v2` | `RelationSchemaTest`, `WpaInputMaterializerTest`, `WpaDeterminismQualificationTest` |
| `souffle-production` | `SouffleWpaExecutorTest`, `ProjectAnalyzerWpaTest`, `WpaPerformanceQualificationTest` |
| `engine-conformance` | `WpaExecutorConformanceTest`, `WpaDifferentialQualificationTest` |
| `witness-closure` | `WitnessCanonicalizerTest`, `AnalysisFactBusTest` |
| `failure-atomicity` | `WpaFailureQualificationTest`, `WpaOrchestratorTest` |
| `run-identity` | `AnalysisRunTest`, `WpaMigrationQualificationTest` |
| `documentation-consistency` | `M9DocumentationConsistencyTest` |

Each aggregate is registered with `m9-entry` and its criterion label. The five
qualification executables also carry `wpa-qualification`. The source-boundary
executables retain ordinary integration labels and are dependencies of the
qualification behavior; they do not silently expand the exact M9 registry or
the five-test `wpa-qualification` set.

`check_no_skips.py` parses CTest JUnit XML and fails on missing, extra,
disabled, skipped, failed, errored, or duplicate aggregate names. Individually
discovered GoogleTest cases do not substitute for aggregate membership.

## 12. CI contract

CI must:

1. Use the vendored Souffle 2.5 source revision
   `5682a9f12e2668ecdd26348fe63cc508bc0fcf47`.
2. Produce and verify the install-provenance manifest and executable SHA-256.
3. Configure production engine selection as Souffle with one evaluation
   thread.
4. Build the source-boundary and qualification targets.
5. Run `wpa-qualification` with JUnit output and the no-skip checker.
6. Run every `m9-entry` criterion set with exact membership enforcement.
7. Preserve the full ordinary test suite; qualification does not replace
   existing unit and integration tests.

## 13. Repository structure

The implementation is expected to add or modify these areas:

```text
include/veritas/wpa/                 run/orchestration public contracts
src/wpa/                             orchestration and run repository
src/summarydb/schema/                versioned run/component persistence
include/veritas/analysis/            analyzer engine configuration/results
src/analysis/                        standard analyzer integration
src/tools/                           CLI engine selection
tests/unit/wpa/                       orchestration and persistence tests
tests/integration/analysis/          standard analyzer and source-boundary tests
tests/qualification/wpa/             five qualification executables
tests/qualification/check_no_skips.py
tests/fixtures/projects/             focused fixtures and semantic_zoo
.github/workflows/ci.yml             mandatory qualification execution
```

All new C, C++, CMake, shell, and Python files carry the repository's full
Apache-2.0 header. Test and production C++ remains compatible with
`-fno-rtti` and `-fno-exceptions`.

## 14. Implementation ordering

Implementation must proceed in dependency order:

1. Persist run/component lifecycle and exact-engine reuse state.
2. Add reverse-topological `WpaOrchestrator` execution and atomic failure
   behavior.
3. Integrate explicit engine selection into `ProjectAnalyzer` and the CLI.
4. Add focused fixture projects and their narrow SVF/SummaryDB tests.
5. Add `semantic_zoo` and its layered end-to-end tests.
6. Add differential, determinism, failure, migration, lifecycle, and
   performance qualification.
7. Register exact criterion membership and mandatory CI execution.

Each implementation increment follows test-first development. Tests that name
a missing fixture fail because the fixture is unavailable before source files
are added. Orchestration tests fail on the missing interface before the
interface is implemented. No test is accepted solely because two empty result
sets compare equal.

## 15. Acceptance criteria

The design is implemented only when:

- standard project analysis runs compiled Souffle WPA and reports a durable
  production run;
- every requested memory, alias, call, class-hierarchy, virtual-dispatch, and
  recursion scenario has an explicit boundary oracle;
- selected scenarios survive the complete source-to-persisted-WPA path;
- Souffle and C++ conformance results agree for every overlapping case using
  byte-identical logical inputs;
- every published derived fact has a closed finite witness;
- failure injection cannot replace prior successful state;
- deterministic permutations and repeated-process analysis produce identical
  semantic results and clean engine state;
- checked-in resource ceilings pass;
- exact M9 entry membership passes with zero missing, extra, disabled,
  skipped, failed, errored, or duplicate aggregate tests;
- the full project build and test suite pass from a clean worktree.
