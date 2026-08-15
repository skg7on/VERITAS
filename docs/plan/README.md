# VERITAS Milestone Implementation Plans

This directory contains executable implementation plans for VERITAS backbone milestones M1 through M10.

Each plan is paired with a detailed design spec in `docs/specs/milestones/` and assumes M0 has already produced the C++ project skeleton, test harness, command-line targets, and `veritas::Status` / `veritas::StatusOr` primitives.

## Plans

| Milestone | Plan | Design Spec |
| --- | --- | --- |
| M1 | `m1-build-intelligence-program-context-implementation-plan.md` | `docs/specs/milestones/m1-build-intelligence-program-context-design-spec.md` |
| M2 | `m2-identity-canonical-hashing-metadata-store-implementation-plan.md` | `docs/specs/milestones/m2-identity-canonical-hashing-metadata-store-design-spec.md` |
| M3 | `m3-summary-ir-cas-object-store-implementation-plan.md` | `docs/specs/milestones/m3-summary-ir-cas-object-store-design-spec.md` |
| M4 | `m4-clang-llvm-local-extraction-implementation-plan.md` | `docs/specs/milestones/m4-clang-llvm-local-extraction-design-spec.md` |
| M5 | `m5-svf-value-flow-pointer-adapter-implementation-plan.md` | `docs/specs/milestones/m5-svf-value-flow-pointer-adapter-design-spec.md` |
| M6 | `m6-thin-veritas-cpg-projection-implementation-plan.md` | `docs/specs/milestones/m6-thin-veritas-cpg-projection-design-spec.md` |
| M7 | `m7-reverse-dependency-incremental-scheduler-implementation-plan.md` | `docs/specs/milestones/m7-reverse-dependency-incremental-scheduler-design-spec.md` |
| M8 | `m8-scc-wpa-souffle-fact-engine-implementation-plan.md` | `docs/specs/milestones/m8-scc-wpa-souffle-fact-engine-design-spec.md` |
| M9 | `m9-provenance-fact-store-explain-api-implementation-plan.md` | `docs/specs/milestones/m9-provenance-fact-store-explain-api-design-spec.md` |
| M10 | `m10-evidence-builder-input-apis-demo-implementation-plan.md` | `docs/specs/milestones/m10-evidence-builder-input-apis-demo-design-spec.md` |

## Execution Rule

Implement one milestone per branch or PR. Do not start the next milestone until the previous milestone's tests and CLI acceptance checks pass.

