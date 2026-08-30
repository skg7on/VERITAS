# VERITAS Guides and Tutorials

These guides are the task-oriented companion to the architecture,
specifications, and implementation plans. Start here when you want to run
VERITAS, inspect a SummaryDB, add an analysis, or build a consumer over
SummaryDB and Evidence IR.

## Capability status

The repository contains implemented code, approved target designs, and future
work. The distinction matters because several architecture documents describe
the complete target platform.

| Capability | Status in the current tree | Entry point |
| --- | --- | --- |
| C/C++ project directory with `compile_commands.json` | **Available now** | `veritas-build analyze --project <dir>` |
| Function Summary IR v2 and native thin CPG publication | **Available now** | `ProjectAnalyzer` and `ProjectPublicationCoordinator` |
| RocksDB summary CAS and SQLite metadata/CPG/dependency schema | **Available now** | `<output>/objects` and `<output>/metadata.db` |
| Native CPG `callees` and budgeted `flow` queries | **Available now** | `veritas-query` |
| Summary component diff and dependency impact for v1 artifacts | **Available now** | `veritas-diff` and C++ APIs; add a version-neutral overload for v2 |
| SCC graph, C++ fixed point, Souffle export/runner, and persisted M8 state | **Available as library/tested M8 components** | `veritas::wpa` and `veritas::facts`; not invoked by `veritas-build analyze` |
| Durable Fact/Provenance Store and explain API | **Approved target (M9)** | Design and plan only |
| Evidence Builder semantic queries and typed handoff | **Approved target (M10B)** | Design and plan only |
| Validated Evidence IR model and serialization | **Approved target (M10C)** | Design and plan only |
| LLVM `.bc` / `.ll` ingestion | **Approved target (M11)** | `--bitcode` is currently rejected |
| Joern GraphSON/GraphML import | **Approved target (M12A-M12C)** | `veritas-build import` is not implemented |
| PhASAR result import | **Future design (M12D)** | No detailed schema or CLI contract yet |

“Available now” means the command or API exists in this repository and is
covered by the current test suite. “Approved target” means the governing design
is approved but the public command/API must not be treated as runnable yet.

## Manuals

1. [Generating and inspecting SummaryDB](summarydb-generation-manual.md)
   explains every input tier, the native end-to-end workflow that works now,
   output layout, inspection, reruns, and adapter readiness.
2. [Extending SummaryDB and Evidence IR](summarydb-evidence-ir-developer-guide.md)
   maps the current extension points and the contracts new native analyses,
   WPA relations, external providers, Evidence producers, and Agent tools must
   preserve.

## Tutorials

1. [Build a SummaryDB analysis tool](tutorial-build-summarydb-analysis-tool.md)
   develops a small version-neutral summary reader, adds graph queries, and
   explains component-level impact analysis using APIs available today.
2. [Build an Agent-based code-review tool](tutorial-agent-code-review.md)
   designs the safe post-M10B/M10C integration: bounded semantic tools,
   Evidence IR, hypotheses, proof obligations, and deterministic verification.

## Normative references

Guides summarize but do not replace the project contracts:

- [Platform architecture](../architecture/01-platform-architecture.md)
- [Whole-program analysis architecture](../architecture/02-whole-program-analysis-architecture.md)
- [SummaryDB storage architecture](../architecture/03-summarydb-storage-architecture.md)
- [Evidence IR architecture](../architecture/04-evidence-ir-architecture.md)
- [M10B Evidence Builder design](../specs/milestones/m10b-evidence-builder-input-apis-demo-design-spec.md)
- [M10C Evidence IR design](../specs/milestones/m10c-evidence-ir-semantic-model-serialization-design-spec.md)
- [M11/M12 adapter boundary](../specs/milestones/m11-m12-summarydb-ingest-adapters-design-spec.md)
- [M12 Joern importer design](../specs/milestones/m12-joern-cpg-summarydb-importer-design-spec.md)

When a guide and a normative specification disagree, follow the normative
specification and update the guide in the same change.
