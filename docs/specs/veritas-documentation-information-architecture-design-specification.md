# VERITAS Documentation Information Architecture Design Specification

**Status:** Approved migration design

## 1. Purpose

This specification defines a deterministic migration of VERITAS architecture,
design-specification, milestone, and implementation-plan documents into a
systematic hierarchy. It preserves the repository's canonical separation
between `docs/specs/` and `docs/plans/`, removes the policy-violating
`docs/superpowers/` tree, and makes reading order and milestone order explicit.

The migration changes document paths and navigation only. It does not revise
technical architecture, milestone scope, implementation requirements, or
historical decisions.

## 2. Design Principles

1. Architecture, specifications, and plans remain distinct document classes.
2. Design specifications never live under `docs/superpowers/`.
3. Architecture documents use a numeric reading order.
4. Milestone identifiers are zero-padded so lexical and semantic order match.
5. Milestone specs and plans use parallel `milestones/` subtrees without being
   co-located across the `docs/specs/` and `docs/plans/` boundary.
6. Filenames describe the document's subject and role without redundant
   project-name prefixes inside ordered architecture paths.
7. Repository indexes are the primary navigation surface; prose references are
   updated to the canonical paths.
8. Historical GitHub prose remains historically accurate. Only references to
   current documents are migrated.

## 3. Target Hierarchy

```text
docs/
├── README.md
├── architecture/
│   ├── README.md
│   ├── 01-platform-architecture.md
│   ├── 02-whole-program-analysis-architecture.md
│   ├── 03-summarydb-storage-architecture.md
│   └── 04-evidence-ir-architecture.md
├── specs/
│   ├── README.md
│   ├── github-actions-ci-build-design-spec.md
│   ├── veritas-documentation-information-architecture-design-specification.md
│   ├── veritas-engineering-backbone-design-specification.md
│   ├── veritas-evidence-ir-formal-specification.md
│   └── milestones/
│       ├── README.md
│       ├── m01-...-design-spec.md
│       ├── ...
│       ├── m08r-...-design-spec.md
│       ├── m10b-...-design-spec.md
│       └── m11-m12-summarydb-ingest-adapters-design-spec.md
└── plans/
    ├── README.md
    ├── github-actions-ci-build-implementation-plan.md
    ├── veritas-backbone-milestone-roadmap.md
    └── milestones/
        ├── m00-...-implementation-plan.md
        ├── ...
        ├── m08r-...-implementation-plan.md
        ├── m10b-...-implementation-plan.md
        └── m12-...-implementation-plan.md
```

`docs/brainstorm/`, `docs/third_party/`, and `docs/assets/` remain outside this
migration because they already have distinct responsibilities.

## 4. Architecture Path Migration

| Current path | Canonical path |
| --- | --- |
| `docs/architecture/veritas-platform-architecture-design.md` | `docs/architecture/01-platform-architecture.md` |
| `docs/architecture/veritas-whole-program-analysis-design.md` | `docs/architecture/02-whole-program-analysis-architecture.md` |
| `docs/architecture/veritas-thin-summarydb-backends-design.md` | `docs/architecture/03-summarydb-storage-architecture.md` |
| `docs/architecture/veritas-evidence-ir-design.md` | `docs/architecture/04-evidence-ir-architecture.md` |

The numeric prefixes define the canonical reading order: platform context,
analysis, storage, then the evidence language consumed above those layers.

## 5. Specification Path Migration

### 5.1 Cross-cutting specifications

| Current path | Canonical path |
| --- | --- |
| `docs/specs/github-actions-ci-build-design.md` | `docs/specs/github-actions-ci-build-design-spec.md` |
| `docs/specs/veritas-engineering-backbone-design-specification.md` | unchanged |
| `docs/specs/veritas-evidence-ir-formal-specification.md` | unchanged |
| `docs/specs/veritas-summarydb-ingest-adapter-design.md` | `docs/specs/milestones/m11-m12-summarydb-ingest-adapters-design-spec.md` |

The M11/M12 adapter specification is milestone-scoped, so it moves into the
milestone subtree while remaining one shared specification.

### 5.2 Milestone specifications

| Current path | Canonical path |
| --- | --- |
| `docs/specs/milestones/m1-build-intelligence-program-context-design-spec.md` | `docs/specs/milestones/m01-project-ingestion-program-context-design-spec.md` |
| `docs/specs/milestones/m2-identity-canonical-hashing-metadata-store-design-spec.md` | `docs/specs/milestones/m02-identity-canonical-hashing-metadata-store-design-spec.md` |
| `docs/specs/milestones/m3-summary-ir-cas-object-store-design-spec.md` | `docs/specs/milestones/m03-summary-ir-cas-object-store-design-spec.md` |
| `docs/specs/milestones/m4-clang-llvm-local-extraction-design-spec.md` | `docs/specs/milestones/m04-clang-llvm-project-analysis-design-spec.md` |
| `docs/specs/milestones/m5-svf-value-flow-pointer-adapter-design-spec.md` | `docs/specs/milestones/m05-required-svf-analysis-design-spec.md` |
| `docs/specs/milestones/m6-thin-veritas-cpg-projection-design-spec.md` | `docs/specs/milestones/m06-thin-cpg-projection-design-spec.md` |
| `docs/specs/milestones/m7-reverse-dependency-incremental-scheduler-design-spec.md` | `docs/specs/milestones/m07-reverse-dependency-incremental-scheduler-design-spec.md` |
| `docs/specs/milestones/m8-scc-wpa-souffle-fact-engine-design-spec.md` | `docs/specs/milestones/m08-scc-wpa-souffle-fact-engine-design-spec.md` |
| `docs/specs/milestones/m8r-souffle-wpa-remediation-design-spec.md` | `docs/specs/milestones/m08r-souffle-wpa-remediation-design-spec.md` |
| `docs/superpowers/specs/2026-08-22-souffle-wpa-architecture-refinement-design.md` | `docs/specs/milestones/m08r-souffle-wpa-architecture-refinement-design-spec.md` |
| `docs/specs/milestones/m9-provenance-fact-store-explain-api-design-spec.md` | `docs/specs/milestones/m09-provenance-fact-store-explain-api-design-spec.md` |
| `docs/specs/milestones/m10-evidence-builder-input-apis-demo-design-spec.md` | `docs/specs/milestones/m10b-evidence-builder-input-apis-demo-design-spec.md` |

`M10B` replaces the ambiguous `M10` filename because the approved architecture
now distinguishes M10A recursive-domain expansion from M10B Evidence Builder
delivery. The migration does not invent an M10A document.

## 6. Plan Path Migration

### 6.1 Project-wide and tooling plans

| Current path | Canonical path |
| --- | --- |
| `docs/specs/veritas-backbone-milestones-and-implementation-plan.md` | `docs/plans/veritas-backbone-milestone-roadmap.md` |
| `docs/plans/github-actions-ci-build-implementation-plan.md` | unchanged |

The backbone milestone document is a roadmap and implementation plan, not a
design specification, so its canonical class is `docs/plans/`.

### 6.2 Milestone implementation plans

| Current path | Canonical path |
| --- | --- |
| `docs/plans/m0-project-skeleton-required-svf-toolchain-implementation-plan.md` | `docs/plans/milestones/m00-project-skeleton-required-svf-toolchain-implementation-plan.md` |
| `docs/plans/m1-build-intelligence-program-context-implementation-plan.md` | `docs/plans/milestones/m01-project-ingestion-program-context-implementation-plan.md` |
| `docs/plans/m2-identity-canonical-hashing-metadata-store-implementation-plan.md` | `docs/plans/milestones/m02-identity-canonical-hashing-metadata-store-implementation-plan.md` |
| `docs/plans/m3-summary-ir-cas-object-store-implementation-plan.md` | `docs/plans/milestones/m03-summary-ir-cas-object-store-implementation-plan.md` |
| `docs/plans/m4-clang-llvm-local-extraction-implementation-plan.md` | `docs/plans/milestones/m04-clang-llvm-project-analysis-implementation-plan.md` |
| `docs/plans/m5-svf-value-flow-pointer-adapter-implementation-plan.md` | `docs/plans/milestones/m05-required-svf-analysis-implementation-plan.md` |
| `docs/plans/m6-thin-veritas-cpg-projection-implementation-plan.md` | `docs/plans/milestones/m06-thin-cpg-projection-implementation-plan.md` |
| `docs/plans/m7-reverse-dependency-incremental-scheduler-implementation-plan.md` | `docs/plans/milestones/m07-reverse-dependency-incremental-scheduler-implementation-plan.md` |
| `docs/plans/m8-scc-wpa-souffle-fact-engine-implementation-plan.md` | `docs/plans/milestones/m08-scc-wpa-souffle-fact-engine-implementation-plan.md` |
| `docs/superpowers/plans/2026-08-22-souffle-wpa-remediation-bridge-implementation-plan.md` | `docs/plans/milestones/m08r-souffle-wpa-remediation-implementation-plan.md` |
| `docs/plans/m9-provenance-fact-store-explain-api-implementation-plan.md` | `docs/plans/milestones/m09-provenance-fact-store-explain-api-implementation-plan.md` |
| `docs/plans/m10-evidence-builder-input-apis-demo-implementation-plan.md` | `docs/plans/milestones/m10b-evidence-builder-input-apis-demo-implementation-plan.md` |
| `docs/plans/m11-external-ir-adapter-implementation-plan.md` | `docs/plans/milestones/m11-external-ir-adapter-implementation-plan.md` |
| `docs/plans/m12-external-facts-importer-implementation-plan.md` | `docs/plans/milestones/m12-external-facts-importer-implementation-plan.md` |

## 7. Navigation and Policy Updates

The migration adds or rewrites these indexes:

- `docs/README.md` describes document classes and the primary reading order.
- `docs/architecture/README.md` orders and summarizes the four architecture
  documents.
- `docs/specs/README.md` distinguishes cross-cutting and milestone specs.
- `docs/specs/milestones/README.md` pairs each milestone spec with its issue and
  implementation plan when available.
- `docs/plans/README.md` distinguishes the project roadmap, tooling plans, and
  milestone plans.

`.claude/rules/docs-layout.md` is updated to make the target hierarchy,
zero-padding, and index requirements normative. `CLAUDE.md` and the repository
`README.md` are updated to reference the new structure and current project
state.

All tracked references to migrated paths are updated, including references in
plans that describe future file modifications or example `git add` commands.
No compatibility stubs remain at old paths because they would create duplicate
sources of truth.

## 8. GitHub Reference Migration

The following issue descriptions contain current-document references and are
in scope: `#3` through `#13`, `#20`, `#21`, and `#61`.

The following pull-request descriptions contain current-document references
and are in scope: `#1`, `#2`, `#18`, `#19`, `#24`, `#27`, `#36`, and `#42`.

The migration preserves prose labels and edits only path targets or literal
current paths. Historical statements such as PR #19's record of deleting the
old monolithic architecture file remain unchanged because rewriting them would
falsify history.

GitHub links use the migration commit SHA rather than `main` or a task branch.
This makes every migrated link resolvable immediately after the branch is
pushed and keeps it valid if branches are later deleted or documents move
again. No issue-discussion comments or PR review comments currently contain
in-scope paths.

## 9. Verification

The migration is complete only when all of these checks pass:

1. Every canonical path in Sections 4–6 exists.
2. No tracked file exists below `docs/superpowers/`.
3. No tracked Markdown file outside this specification and its implementation
   plan references a migrated old path; those two migration records retain the
   old-to-new tables intentionally.
4. Documentation indexes cover every architecture, milestone-spec, and
   milestone-plan document exactly once.
5. Internal Markdown links resolve to tracked files and local anchors.
6. `git diff --check` reports no whitespace errors.
7. The clean build and complete CTest suite satisfy repository pre-push policy.
8. Each edited GitHub issue and PR body contains the expected migration-commit
   URL, and each URL resolves through the GitHub contents API.
9. The primary checkout remains clean on `main`; the task worktree is clean on
   its dedicated branch after commit.

## 10. Non-goals

- Rewriting technical content or milestone acceptance criteria.
- Creating missing M0, M10A, M11, or M12 standalone design specs.
- Moving brainstorming, third-party, asset, fixture, or repository-policy
  documents outside the changes explicitly required by this hierarchy.
- Rewriting historical GitHub references that intentionally describe paths as
  they existed in an earlier commit.
