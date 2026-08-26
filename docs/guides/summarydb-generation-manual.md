# Generating and Inspecting a VERITAS SummaryDB

This manual explains how program inputs become Function Summary IR, a native
thin CPG, and persistent SummaryDB state. It covers the executable source-code
workflow first, then the approved LLVM IR/bitcode and Joern workflows, the
future PhASAR adapter, and the contract for additional providers.

## 1. Know which path exists

VERITAS has three architectural ingest tiers. Only Tier 1 has a public command
in the current tree.

| Tier | Input | What VERITAS owns after ingestion | Current status |
| --- | --- | --- | --- |
| 1 | Project directory containing `compile_commands.json` | Clang CodeGen, linked `ProgramIr`, local extraction, required in-process SVF, Summary IR v2, native thin CPG, atomic publication | **Available now** |
| 2 | One `.bc`/`.ll` file or a directory of them | Module loading/linking followed by the same native analysis path as Tier 1 | **Approved M11 target** |
| 3 | Joern GraphSON/GraphML or another provider result | Provider-neutral graph/facts, capabilities, assumptions, provenance, and an independent provider binding | **Approved M12 target**; PhASAR still needs M12D design |

The difference between Tier 2 and Tier 3 is authority. Bitcode is only an
alternate way to acquire the LLVM module; VERITAS still computes the native
summaries. Joern and PhASAR supply external observations, so their results stay
in provider projections and enter with an epistemic floor of `INFERRED` or
`ASSUMED`.

## 2. Build VERITAS

Prerequisites and exact version contracts are documented in
[LLVM](../third_party/LLVM.md) and [SVF](../third_party/SVF.md). The canonical
build uses Ninja and the repository presets:

```bash
cmake --preset default \
  -DLLVM_PROJECT_BUILD_DIR=/absolute/path/to/llvm-project/build
cmake --build --preset default
ctest --preset default
```

The relevant executables are produced under `build/bin/`:

```text
veritas-build    ingest and native analysis
veritas-query    native CPG queries available today
veritas-diff     summary-component diff and dependency impact
veritas-explain  version-only skeleton; the planned M9 fact API is not present
```

Confirm that the executable and checkout agree before generating persistent
state:

```bash
build/bin/veritas-build --version
```

## 3. Tier 1: generate SummaryDB from source code

### 3.1 Prepare the project

The path passed to `--project` must be a directory and must contain a non-empty
`compile_commands.json` at its root. VERITAS intentionally does not accept a
direct `--compile-db` path.

For a CMake project:

```bash
cmake -S /absolute/path/to/project \
  -B /absolute/path/to/project/build \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cp /absolute/path/to/project/build/compile_commands.json \
   /absolute/path/to/project/compile_commands.json
```

Other build systems may use their normal compilation-database generator. The
database must describe every translation unit intended for analysis. Missing
files, empty databases, invalid commands, or partial ingestion are hard
failures; VERITAS never silently analyzes a subset.

Prefer absolute `directory` values in each compilation-database entry (the
normal CMake output). A relative `directory` is resolved by the compilation
database loader from the analyzer process's working directory, not guaranteed
from `--project`; this can make an existing source appear missing.

Before a large run, check the input quickly:

```bash
test -s /absolute/path/to/project/compile_commands.json
python3 -m json.tool \
  /absolute/path/to/project/compile_commands.json >/dev/null
```

The second command checks JSON syntax only. Clang's compilation-database
loader and VERITAS command normalization perform the authoritative validation.

### 3.2 Run the native pipeline

Use an explicit output directory when the database should live outside the
analyzed tree:

```bash
build/bin/veritas-build analyze \
  --project /absolute/path/to/project \
  --output /absolute/path/to/summarydb
```

If `--output` is omitted, VERITAS writes to
`/absolute/path/to/project/.veritas`.

The current command performs this deterministic flow:

```text
project directory
  -> resolve compile_commands.json
  -> normalize commands and build AnalysisManifest
  -> Clang CodeGen for every translation unit
  -> link all modules into one private ProgramIr
  -> extract local summary.v2 drafts
  -> run required in-process SVF
  -> merge typed calls, memory effects, value flows, and aliases
  -> build the native thin CPG
  -> write immutable summary objects
  -> atomically bind summaries and CPG as current
```

`veritas-build analyze` does **not** currently run the M8 SCC/fixpoint library,
publish an M9 durable fact store, build M10B evidence inputs, or serialize M10C
Evidence IR. Those stages have separate milestone gates.

### 3.3 Read the command result

A successful invocation prints the stable program context and publication
result:

```text
Project: /absolute/path/to/project
Repository: repo:sha256:<digest>
Revision: rev:sha256:<digest>
Build Variant: bv:sha256:<digest>
Translation Units: <count>
Diagnostic Manifest: /absolute/path/to/summarydb/manifest.json
Analysis complete
Published summaries: <count>
CPG projection: cpgproj:sha256:<digest>
CPG nodes: <count>
CPG edges: <count>
Unknowns: <count>
```

Save the revision, build-variant, projection, and relevant function/value IDs.
The query tools operate on stable IDs, not source names or file-line identity.

### 3.4 Understand the output directory

The implemented output is:

```text
<summarydb-root>/
  manifest.json    diagnostic, deterministic AnalysisManifest view
  metadata.db      SQLite metadata, bindings, native CPG, dependencies, M8 state
  objects/         RocksDB content-addressed Function Summary objects
```

`manifest.json` is diagnostic JSON, not a canonical persistent input. Summary
objects are canonical Protobuf payloads keyed by `FunctionSummaryID`; do not
edit RocksDB files or treat their physical key layout as an API.

The current SQLite schema contains these main groups:

| Group | Tables |
| --- | --- |
| Program context | `repositories`, `revisions`, `build_variants`, `translation_units` |
| Function identity schema | `function_symbols`, `function_variants`, `function_bodies` |
| Summary publication | `summary_objects`, `summary_components`, `summary_bindings` |
| Native CPG | `cpg_projections`, `cpg_nodes`, `cpg_edges`, `cpg_edge_support`, `current_cpg_projections` |
| Incrementality | `summary_dependencies`, `reverse_dependency_index`, `summary_deltas`, `component_deltas` |
| M8 state | `wpa_sccs`, `wpa_scc_members`, `wpa_scc_edges`, `wpa_component_states` |

Product code must use semantic C++ APIs rather than SQL. Direct SQL is useful
only for development diagnostics and schema troubleshooting.

### 3.5 Inspect a generated database

Check the physical files:

```bash
find /absolute/path/to/summarydb -maxdepth 2 -print
```

Inspect diagnostic counts without changing the database:

```bash
sqlite3 -readonly /absolute/path/to/summarydb/metadata.db \
  "SELECT 'summaries', count(*) FROM summary_objects
   UNION ALL
   SELECT 'bindings', count(*) FROM summary_bindings
   UNION ALL
   SELECT 'cpg_nodes', count(*) FROM cpg_nodes
   UNION ALL
   SELECT 'cpg_edges', count(*) FROM cpg_edges;"
```

List the current context and projection:

```bash
sqlite3 -readonly /absolute/path/to/summarydb/metadata.db \
  "SELECT revision_id, build_variant_id, projection_id
     FROM current_cpg_projections
    ORDER BY revision_id, build_variant_id;"
```

List stable function IDs from the current native projection. `node_kind = 0`
is the current on-disk enum value for native function nodes; this query is a
diagnostic aid, not a stable application interface:

```bash
sqlite3 -readonly /absolute/path/to/summarydb/metadata.db \
  "SELECT node_id, node_label
     FROM cpg_nodes
    WHERE projection_id = '<projection-id>' AND node_kind = 0
    ORDER BY node_id;"
```

Use the supported CLI for a direct callee query:

```bash
build/bin/veritas-query callees <function-variant-id> \
  --revision <revision-id> \
  --build <build-variant-id> \
  --db /absolute/path/to/summarydb
```

Use the budgeted native value-flow traversal when the endpoint `ValueRef` IDs
are known:

```bash
build/bin/veritas-query flow <source-value-id> <destination-value-id> \
  --projection <projection-id> \
  --db /absolute/path/to/summarydb \
  --max-depth 10 \
  --max-nodes 1000 \
  --max-paths 100
```

`Truncated by: none` means the traversal was complete within its budget. A
truncated empty result is not proof that no path exists.

### 3.6 Rerun and compare

Rerunning the same semantic input is safe. Summary objects are written with
put-if-absent semantics, while one SQLite transaction advances current summary
and CPG bindings. Equivalent content reuses the same IDs.

After a source or build change, generate a new SummaryDB snapshot in the same
output root and retain the old and new summary IDs. The current analyzer emits
v2 summaries, and there is **no current v2 diff command**. Do not pass those
IDs to `veritas-diff`: that CLI still reads v1 objects through `GetSummary` and
will reject analyzer-produced v2 objects.

For two historical v1 summary objects, the implemented command is:

```bash
build/bin/veritas-diff \
  --db /absolute/path/to/summarydb \
  --old <old-function-summary-id> \
  --new <new-function-summary-id> \
  --max-consumers 1000 \
  --max-depth 16
```

The command reports semantic versus evidence-only change, changed components,
impacted consumers, and explicit truncation. A v2-capable tool must first add
a version-neutral `SummaryArtifact` overload for summary diffing; the
[analysis-tool tutorial](tutorial-build-summarydb-analysis-tool.md) identifies
that extension boundary.

### 3.7 Troubleshooting the native path

| Symptom | Meaning and next check |
| --- | --- |
| `project root is missing compile_commands.json` | Put the database at the exact project root passed to `--project`. |
| `compile_commands.json is empty` | Regenerate the build database; a zero-byte file is rejected before parsing. |
| `invalid compile_commands.json` | Validate JSON and ensure entries follow Clang's compilation database schema. |
| Missing translation unit or compile failure | Fix the command/file; partial project analysis is intentionally refused. |
| Source exists but is reported missing | Check whether the entry's `directory` is relative or stale; regenerate with absolute working directories. |
| Module link failure | Resolve conflicting/incompatible translation-unit outputs or duplicate definitions. |
| SVF stage failure | Check the pinned SVF/LLVM build contract and preserve the exact diagnostic. |
| Invalid or missing stable identity | Do not synthesize an ID; fix the identity-producing frontend/adapter. |
| Query returns no rows | Confirm revision, build variant, projection, and endpoint ID all belong to the same snapshot. |
| Query reports truncation | Increase the relevant budget or surface the result as incomplete; never reinterpret it as absence. |

## 4. Tier 2: LLVM IR and bitcode

**Status: approved M11 target, not executable today.** The current
`veritas-build analyze` rejects `--bitcode`, `--llvm-module`, and `--svf-input`.

### 4.1 Prepare high-fidelity input

Generate debug-bearing bitcode where possible:

```bash
clang++ -g -emit-llvm -c source.cpp -o source.bc
clang++ -g -S -emit-llvm source.cpp -o source.ll
```

The approved adapter classifies input fidelity:

| Fidelity | Contents | Target behavior |
| --- | --- | --- |
| T0 | Stable symbols plus debug/source information | Full analysis with source anchors |
| T1 | Stable symbols but no debug information | Analysis succeeds; evidence is name-only where anchors are unavailable |
| T2 | Stripped, with no stable symbol identity | Reject with `FailedPrecondition` |

Textual `.ll` and binary `.bc` of the same module must normalize to the same
native semantic result. A directory input is sorted, parsed, verified, and
linked as one private `ProgramIr`; duplicate or incompatible definitions are
not silently merged.

### 4.2 Approved target command

The M11 CLI contract is:

```bash
# Target interface; not runnable in the current tree.
build/bin/veritas-build analyze \
  --bitcode /absolute/path/to/module-or-directory \
  --output /absolute/path/to/summarydb
```

When M11 lands, this path must feed the same `LocalFactExtractor`, in-process
SVF, Summary IR v2 builder, native CPG projection, and atomic publisher as Tier
1. It must not import analysis results embedded by another tool as native
facts. Source-derived and bitcode-derived results remain different build
variants even when they name the same function.

### 4.3 M11 readiness check

Do not advertise the target command until all of these exist and pass:

- `ProgramIrSource`, `CodeGenIrSource`, and `BitcodeIrSource`;
- bounded module discovery plus LLVM parse/verify;
- T0/T1/T2 fidelity detection;
- deterministic linking and `OriginMap` construction;
- analyzer-run provenance containing fidelity and source-anchor availability;
- `.ll`/`.bc` parity, directory-link, stripped-input, version-mismatch, and
  deterministic-ID tests; and
- CLI tests proving `--project` and `--bitcode` are mutually exclusive.

See the [M11 plan](../plans/milestones/m11-external-ir-adapter-implementation-plan.md).

## 5. Tier 3: Joern CPG

**Status: approved M12A-M12C target, not executable today.** The current
`veritas-build` has no `import` subcommand.

### 5.1 Produce the provider artifact

The approved V1 contract accepts whole-graph Joern exports in GraphSON or
GraphML, corresponding conceptually to:

```bash
joern-export --repr=all --format=graphson
joern-export --repr=all --format=graphml
```

Generate the provider graph from the same source/build snapshot already bound
in the target SummaryDB. The importer never runs Joern, a JVM, a script, a
plugin, or a network request; it parses a supplied file as untrusted input.

### 5.2 Approved target command

```bash
# Target interface; not runnable in the current tree.
build/bin/veritas-build import \
  --joern /absolute/path/to/graph.graphson \
  --project /absolute/path/to/project \
  --output /absolute/path/to/summarydb \
  --format auto
```

An import requires an existing compatible native repository, revision, and
build-variant binding. If the export cannot prove source or build/frontend
correspondence, the target defaults to failure. The operator may explicitly
accept missing correspondence evidence:

```bash
# Target interface; creates durable context assumptions.
build/bin/veritas-build import \
  --joern /absolute/path/to/graph.graphml \
  --project /absolute/path/to/project \
  --output /absolute/path/to/summarydb \
  --accept-unverified-context
```

A verified mismatch is never overridable.

### 5.3 What the importer must publish

```text
GraphSON / GraphML
  -> bounded reader
  -> RawProviderGraph
  -> schema, capability, and context validation
  -> identity resolution and provider-neutral normalization
  -> ProviderProgramGraph + ExternalFactBatch
  -> one atomic provider publication
```

The result is an overlay, not a replacement for native state:

- native M6 `ThinCpg`, summaries, SCCs, and WPA inputs do not change;
- Joern ordinals stay in provider-record provenance and never become native
  semantic IDs;
- graph topology and registered semantic facts are separate products;
- facts enter as `INFERRED` or `ASSUMED`, never `MUST`;
- missing Joern edges never establish negative evidence;
- unknown vocabulary is retained as inert typed extensions; and
- graph, facts, assumptions, witnesses, history, and the current provider
  binding become visible together or not at all.

Equivalent GraphSON and GraphML exports may have different artifact/run IDs
but must have the same normalized `ProviderProjectionID`.

See the [M12 Joern design](../specs/milestones/m12-joern-cpg-summarydb-importer-design-spec.md)
and [M12B plan](../plans/milestones/m12b-joern-graphson-graphml-importer-implementation-plan.md).

## 6. Tier 3: PhASAR analysis results

**Status: future M12D design.** There is no approved PhASAR schema, reader,
normalizer, command, or implementation plan yet.

PhASAR should reuse the M12A provider substrate but not the Joern graph reader.
The expected shape is fact-oriented:

```text
PhASAR result artifact
  -> PhASAR-specific bounded reader
  -> typed result records and capability declaration
  -> exact identity/context bridge
  -> canonical relations.v2 facts + assumptions + rooted witnesses
  -> ExternalFactBatch
  -> atomic provider publication
```

Before implementation, M12D must fix:

1. supported PhASAR analysis/result versions and serialization formats;
2. context fingerprints and LLVM-version compatibility;
3. mappings from each result relation to VERITAS stable-ID domains and
   `relations.v2` schemas;
4. provider capability, completeness, truncation, and model semantics;
5. provenance roots down to immutable artifact records;
6. ambiguity and unresolved-identity behavior;
7. resource budgets and untrusted-input controls;
8. atomic publication and idempotency; and
9. acceptance fixtures proving no provider fact is promoted to `MUST`.

Do not route PhASAR result tuples through the Joern `RawProviderGraph` API
merely to reuse code. Reuse the provider publication substrate and common fact
validator; keep the provider-specific reader and normalizer independent.

## 7. Adding another input kind

Choose the integration tier by what the artifact contains:

```text
Can it reconstruct a stable LLVM module?
  yes -> Tier 2 ProgramIrSource; run VERITAS native analysis
  no  -> Does it contain precomputed observations/results?
          yes -> Tier 3 provider adapter; preserve external authority
          no  -> reject or design a new acquisition path
```

Never convert external facts into a fake `FunctionSummary`, fake native CPG
edge, or fabricated `MUST` fact. New module sources share the native pipeline;
new analysis providers share the M12A provider substrate.

## 8. Reproducibility checklist

For any ingestion path, record and verify:

- exact repository, revision, and build-variant IDs;
- analyzer/importer version and configuration digest;
- schema and model/rule bundle versions;
- input content digest and fidelity/capabilities;
- stable-ID validation for every cross-boundary reference;
- explicit assumptions, unknowns, and truncation;
- canonical ordering independent of filesystem, map, or provider iteration;
- immutable object writes followed by atomic current-binding publication; and
- a second identical run that reproduces semantic IDs and query results.

Continue with the [developer guide](summarydb-evidence-ir-developer-guide.md)
to add analyses or provider adapters, or the
[analysis-tool tutorial](tutorial-build-summarydb-analysis-tool.md) to consume
the SummaryDB that the current Tier 1 workflow produces.
