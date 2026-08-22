# VERITAS Milestone Design Specs

This directory contains detailed design specifications for VERITAS backbone
milestones M1-M9 and M10B, plus the approved M8R remediation bridge. M10A
recursive domain expansion requires its own detailed spec before implementation.

The implementation checklist for these milestones lives in:

`docs/specs/veritas-backbone-milestones-and-implementation-plan.md`

The executable per-milestone implementation plans live in:

`docs/plans/`

The backbone data model and invariants live in:

`docs/specs/veritas-engineering-backbone-design-specification.md`

## Milestone Specs

| Milestone | Spec |
| --- | --- |
| M1 | `m1-build-intelligence-program-context-design-spec.md` |
| M2 | `m2-identity-canonical-hashing-metadata-store-design-spec.md` |
| M3 | `m3-summary-ir-cas-object-store-design-spec.md` |
| M4 | `m4-clang-llvm-local-extraction-design-spec.md` |
| M5 | `m5-svf-value-flow-pointer-adapter-design-spec.md` |
| M6 | `m6-thin-veritas-cpg-projection-design-spec.md` |
| M7 | `m7-reverse-dependency-incremental-scheduler-design-spec.md` |
| M8 | `m8-scc-wpa-souffle-fact-engine-design-spec.md` |
| M8R.1-M8R.5 | `m8r-souffle-wpa-remediation-design-spec.md` |
| M9 | `m9-provenance-fact-store-explain-api-design-spec.md` |
| M10A | Detailed recursive-domain-expansion spec required before implementation |
| M10B | `m10-evidence-builder-input-apis-demo-design-spec.md` |
| M13 | Separately approved benchmark-gated PTA research; independent of M9-M12 |

M0 is the project skeleton and toolchain harness. It remains in the implementation plan because it is bootstrapping infrastructure rather than a semantic backbone subsystem.
