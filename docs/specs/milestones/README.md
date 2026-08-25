# VERITAS Milestone Design Specifications

The engineering-backbone invariants are defined in the
[cross-cutting specification](../veritas-engineering-backbone-design-specification.md),
and milestone order is defined by the
[backbone milestone roadmap](../../plans/veritas-backbone-milestone-roadmap.md).

| Milestone | Status | Design specification | Implementation plan | Issue |
| --- | --- | --- | --- | --- |
| M0 | Implemented | — | [plan](../../plans/milestones/m00-project-skeleton-required-svf-toolchain-implementation-plan.md) | [#3](https://github.com/skg7on/VERITAS/issues/3) |
| M1 | Implemented | [spec](m01-project-ingestion-program-context-design-spec.md) | [plan](../../plans/milestones/m01-project-ingestion-program-context-implementation-plan.md) | [#4](https://github.com/skg7on/VERITAS/issues/4) |
| M2 | Implemented | [spec](m02-identity-canonical-hashing-metadata-store-design-spec.md) | [plan](../../plans/milestones/m02-identity-canonical-hashing-metadata-store-implementation-plan.md) | [#5](https://github.com/skg7on/VERITAS/issues/5) |
| M3 | Implemented | [spec](m03-summary-ir-cas-object-store-design-spec.md) | [plan](../../plans/milestones/m03-summary-ir-cas-object-store-implementation-plan.md) | [#6](https://github.com/skg7on/VERITAS/issues/6) |
| M4 | Implemented | [spec](m04-clang-llvm-project-analysis-design-spec.md) | [plan](../../plans/milestones/m04-clang-llvm-project-analysis-implementation-plan.md) | [#7](https://github.com/skg7on/VERITAS/issues/7) |
| M5 | Implemented | [spec](m05-required-svf-analysis-design-spec.md) | [plan](../../plans/milestones/m05-required-svf-analysis-implementation-plan.md) | [#8](https://github.com/skg7on/VERITAS/issues/8) |
| M6 | Implemented | [spec](m06-thin-cpg-projection-design-spec.md) | [plan](../../plans/milestones/m06-thin-cpg-projection-implementation-plan.md) | [#9](https://github.com/skg7on/VERITAS/issues/9) |
| M7 | Implemented | [spec](m07-reverse-dependency-incremental-scheduler-design-spec.md) | [plan](../../plans/milestones/m07-reverse-dependency-incremental-scheduler-implementation-plan.md) | [#10](https://github.com/skg7on/VERITAS/issues/10) |
| M8 | Implemented | [spec](m08-scc-wpa-souffle-fact-engine-design-spec.md) | [plan](../../plans/milestones/m08-scc-wpa-souffle-fact-engine-implementation-plan.md) | [#11](https://github.com/skg7on/VERITAS/issues/11) |
| M8R.1–M8R.2 | Delivered | [remediation spec](m08r-souffle-wpa-remediation-design-spec.md); [architecture refinement](m08r-souffle-wpa-architecture-refinement-design-spec.md) | [plan](../../plans/milestones/m08r-souffle-wpa-remediation-implementation-plan.md) | [#64](https://github.com/skg7on/VERITAS/pull/64), [#76](https://github.com/skg7on/VERITAS/pull/76) |
| M8R.3–M8R.5 | Outstanding | [remediation spec](m08r-souffle-wpa-remediation-design-spec.md); [architecture refinement](m08r-souffle-wpa-architecture-refinement-design-spec.md) | [plan](../../plans/milestones/m08r-souffle-wpa-remediation-implementation-plan.md) | [#61](https://github.com/skg7on/VERITAS/issues/61) (bridge, owns M8R.4–M8R.5); [#77](https://github.com/skg7on/VERITAS/issues/77) (M8R.3) |
| M9 | Planned | [spec](m09-provenance-fact-store-explain-api-design-spec.md) | [plan](../../plans/milestones/m09-provenance-fact-store-explain-api-implementation-plan.md) | [#12](https://github.com/skg7on/VERITAS/issues/12) |
| M10A | Planned; detailed specification required before implementation | — | — | — |
| M10B | Planned | [spec](m10b-evidence-builder-input-apis-demo-design-spec.md) | [plan](../../plans/milestones/m10b-evidence-builder-input-apis-demo-implementation-plan.md) | [#13](https://github.com/skg7on/VERITAS/issues/13) |
| M10C | Planned | [spec](m10c-evidence-ir-semantic-model-serialization-design-spec.md) | [plan](../../plans/milestones/m10c-evidence-ir-semantic-model-serialization-implementation-plan.md) | [#70](https://github.com/skg7on/VERITAS/issues/70) |
| M11 | Planned | [shared ingest-boundary spec](m11-m12-summarydb-ingest-adapters-design-spec.md) | [plan](../../plans/milestones/m11-external-ir-adapter-implementation-plan.md) | [#20](https://github.com/skg7on/VERITAS/issues/20) |
| M12A | Approved / pending implementation | [Joern/SummaryDB spec](m12-joern-cpg-summarydb-importer-design-spec.md); [shared boundary](m11-m12-summarydb-ingest-adapters-design-spec.md) | [SummaryDB provider substrate plan](../../plans/milestones/m12a-summarydb-external-provider-substrate-implementation-plan.md) | [#21](https://github.com/skg7on/VERITAS/issues/21) |
| M12B | Approved / pending implementation | [Joern/SummaryDB spec](m12-joern-cpg-summarydb-importer-design-spec.md) | [Joern GraphSON/GraphML importer plan](../../plans/milestones/m12b-joern-graphson-graphml-importer-implementation-plan.md) | [#21](https://github.com/skg7on/VERITAS/issues/21) |
| M12C | Approved / pending implementation | [Joern/SummaryDB spec](m12-joern-cpg-summarydb-importer-design-spec.md) | [provider fusion/Evidence plan](../../plans/milestones/m12c-provider-fusion-evidence-integration-implementation-plan.md) | [#21](https://github.com/skg7on/VERITAS/issues/21) |
| M12D | Detailed design required | PhASAR result adapter over M12A; intentionally separate from Joern graph ingestion | — | [#21](https://github.com/skg7on/VERITAS/issues/21) |

M13 is separately approved benchmark-gated PTA research and remains independent
of the M9–M12 critical path, including M10C.

M10B and M10C share the approved
[API-to-Evidence-IR executable test contract](m10b-m10c-api-to-evidence-ir-test-design-spec.md),
which assigns stable `AC`, `QRY`, `HND`, `BLD`, `VID`, `REP`, and `DEM` cases to
the two milestones.
