# VERITAS Design Specifications

Cross-cutting specifications establish repository-wide design requirements.
Milestone-specific specifications live in the parallel `milestones/` subtree.

## Current Product Refinement

[Review-driven analysis](veritas-review-driven-analysis-design-spec.md) proposes
the LLM/static-analysis reviewer goal, preserves authority and data contracts,
and maps open issues to delivery priorities. Existing capability specifications
remain binding for runtime/schema/acceptance until explicitly amended.

## Cross-Cutting Specifications

- [GitHub Actions CI build](github-actions-ci-build-design-spec.md)
- [Documentation information architecture](veritas-documentation-information-architecture-design-specification.md)
- [Engineering backbone](veritas-engineering-backbone-design-specification.md)
- [Evidence IR formal specification](veritas-evidence-ir-formal-specification.md)
- [Evidence IR Agent security use cases](veritas-evidence-ir-agent-security-use-cases-design-spec.md)
- [Claude Code Evidence IR review plugin](veritas-claude-code-evidence-review-plugin-design-spec.md)
- [Link-unit / program boundary](link-unit-program-boundary-design-spec.md)
- [`veritas-build analyze` performance](veritas-build-analyze-performance-design-spec.md)
- [`veritas-build analyze` round 3 performance](veritas-build-analyze-round3-performance-design-spec.md)
- [`veritas-build analyze` round 4 performance](veritas-build-analyze-round4-performance-design-spec.md)
- [`veritas-build analyze` phase observability](veritas-build-analyze-phase-observability-design-spec.md)
- [WPA and SummaryDB end-to-end qualification corpus](wpa-summarydb-qualification-corpus-design-spec.md)
- [WPA in-process Soufflé execution](wpa-in-process-souffle-execution-design-spec.md)
- [WPA witness derivation identity](wpa-witness-derivation-identity-design-spec.md)

## Milestone Specifications

The [milestone specification matrix](milestones/README.md) provides each
milestone's approved design specification, related implementation plan, and
tracking issue.
