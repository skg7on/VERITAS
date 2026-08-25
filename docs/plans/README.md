# VERITAS Implementation Plans

Plans are executable implementation guidance. Read the
[backbone milestone roadmap](veritas-backbone-milestone-roadmap.md) for
milestone sequencing and the [GitHub Actions CI build plan](github-actions-ci-build-implementation-plan.md)
for the project tooling plan. The [documentation information architecture
migration plan](documentation-information-architecture-migration-implementation-plan.md)
records this hierarchy migration.

Additional project/tooling plans:

- [Claude Code Evidence IR review plugin](veritas-claude-code-evidence-review-plugin-implementation-plan.md)

| Milestone | Status | Implementation plan | Design specification | Issue |
| --- | --- | --- | --- | --- |
| M0 | Implemented | [plan](milestones/m00-project-skeleton-required-svf-toolchain-implementation-plan.md) | — | [#3](https://github.com/skg7on/VERITAS/issues/3) |
| M1 | Implemented | [plan](milestones/m01-project-ingestion-program-context-implementation-plan.md) | [spec](../specs/milestones/m01-project-ingestion-program-context-design-spec.md) | [#4](https://github.com/skg7on/VERITAS/issues/4) |
| M2 | Implemented | [plan](milestones/m02-identity-canonical-hashing-metadata-store-implementation-plan.md) | [spec](../specs/milestones/m02-identity-canonical-hashing-metadata-store-design-spec.md) | [#5](https://github.com/skg7on/VERITAS/issues/5) |
| M3 | Implemented | [plan](milestones/m03-summary-ir-cas-object-store-implementation-plan.md) | [spec](../specs/milestones/m03-summary-ir-cas-object-store-design-spec.md) | [#6](https://github.com/skg7on/VERITAS/issues/6) |
| M4 | Implemented | [plan](milestones/m04-clang-llvm-project-analysis-implementation-plan.md) | [spec](../specs/milestones/m04-clang-llvm-project-analysis-design-spec.md) | [#7](https://github.com/skg7on/VERITAS/issues/7) |
| M5 | Implemented | [plan](milestones/m05-required-svf-analysis-implementation-plan.md) | [spec](../specs/milestones/m05-required-svf-analysis-design-spec.md) | [#8](https://github.com/skg7on/VERITAS/issues/8) |
| M6 | Implemented | [plan](milestones/m06-thin-cpg-projection-implementation-plan.md) | [spec](../specs/milestones/m06-thin-cpg-projection-design-spec.md) | [#9](https://github.com/skg7on/VERITAS/issues/9) |
| M7 | Implemented | [plan](milestones/m07-reverse-dependency-incremental-scheduler-implementation-plan.md) | [spec](../specs/milestones/m07-reverse-dependency-incremental-scheduler-design-spec.md) | [#10](https://github.com/skg7on/VERITAS/issues/10) |
| M8 | Implemented | [plan](milestones/m08-scc-wpa-souffle-fact-engine-implementation-plan.md) | [spec](../specs/milestones/m08-scc-wpa-souffle-fact-engine-design-spec.md) | [#11](https://github.com/skg7on/VERITAS/issues/11) |
| M8R.1–M8R.5 | Approved / pending implementation | [plan](milestones/m08r-souffle-wpa-remediation-implementation-plan.md) | [remediation spec](../specs/milestones/m08r-souffle-wpa-remediation-design-spec.md); [architecture refinement](../specs/milestones/m08r-souffle-wpa-architecture-refinement-design-spec.md) | [#61](https://github.com/skg7on/VERITAS/issues/61) |
| M9 | Planned | [plan](milestones/m09-provenance-fact-store-explain-api-implementation-plan.md) | [spec](../specs/milestones/m09-provenance-fact-store-explain-api-design-spec.md) | [#12](https://github.com/skg7on/VERITAS/issues/12) |
| M10A | Planned; detailed specification and plan required before implementation | — | — | — |
| M10B | Planned | [plan](milestones/m10b-evidence-builder-input-apis-demo-implementation-plan.md) | [spec](../specs/milestones/m10b-evidence-builder-input-apis-demo-design-spec.md) | [#13](https://github.com/skg7on/VERITAS/issues/13) |
| M10C | Planned | [plan](milestones/m10c-evidence-ir-semantic-model-serialization-implementation-plan.md) | [spec](../specs/milestones/m10c-evidence-ir-semantic-model-serialization-design-spec.md) | — |
| M11 | Planned | [plan](milestones/m11-external-ir-adapter-implementation-plan.md) | [shared ingest-boundary spec](../specs/milestones/m11-m12-summarydb-ingest-adapters-design-spec.md) | [#20](https://github.com/skg7on/VERITAS/issues/20) |
| M12A | Approved / pending implementation | [SummaryDB provider substrate plan](milestones/m12a-summarydb-external-provider-substrate-implementation-plan.md) | [Joern/SummaryDB spec](../specs/milestones/m12-joern-cpg-summarydb-importer-design-spec.md) | [#21](https://github.com/skg7on/VERITAS/issues/21) |
| M12B | Approved / pending implementation | [Joern GraphSON/GraphML importer plan](milestones/m12b-joern-graphson-graphml-importer-implementation-plan.md) | [Joern/SummaryDB spec](../specs/milestones/m12-joern-cpg-summarydb-importer-design-spec.md) | [#21](https://github.com/skg7on/VERITAS/issues/21) |
| M12C | Approved / pending implementation | [provider fusion/Evidence plan](milestones/m12c-provider-fusion-evidence-integration-implementation-plan.md) | [Joern/SummaryDB spec](../specs/milestones/m12-joern-cpg-summarydb-importer-design-spec.md) | [#21](https://github.com/skg7on/VERITAS/issues/21) |
| M12D | Detailed design and plan required | — | PhASAR adapter is intentionally separate from the Joern graph importer | [#21](https://github.com/skg7on/VERITAS/issues/21) |

## Execution Rule

Implement one milestone per branch or PR. Do not start the next milestone until
the previous milestone's tests and CLI acceptance checks pass.
