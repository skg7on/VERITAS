# Documentation Information Architecture Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move VERITAS architecture, milestone design-specification, and implementation-plan documents into the approved ordered hierarchy, migrate every current repository reference, and retain canonical immutable GitHub links only in the eligible open issue descriptions.

**Architecture:** Preserve the top-level `docs/specs/` and `docs/plans/` taxonomy, add parallel milestone subtrees, and use one explicit old-to-new mapping for filesystem moves and reference rewriting. Commit the complete local migration before updating GitHub so remote links can use one permanent commit SHA.

**Tech Stack:** Git, Markdown, POSIX shell, Perl-compatible path replacement, ripgrep, CMake/Ninja, CTest, GitHub CLI

**Spec:** `docs/specs/veritas-documentation-information-architecture-design-specification.md`

## Global Constraints

- Architecture, specifications, and plans remain distinct document classes.
- Design specifications never live under `docs/superpowers/`.
- Architecture documents use the numeric reading order 01 through 04.
- Milestone identifiers below 10 are zero-padded; refinements preserve their suffix, such as `m08r` and `m10b`.
- Milestone specs and plans use parallel `docs/specs/milestones/` and `docs/plans/milestones/` subtrees.
- The migration changes paths and navigation only; technical requirements and historical decisions do not change.
- No compatibility stubs remain at old paths.
- The binding final GitHub scope supersedes the original broader selection: only open issues `#12`, `#13`, `#20`, `#21`, and `#61` receive or retain canonical immutable links; closed issues and merged PRs remain historical records.
- Eligible GitHub document links use the final migration commit SHA, never `main` or the task branch; historical records retain their original revisions.
- The primary checkout stays clean on `main`; every write, build, test, commit, push, and PR operation occurs in the linked task worktree.

---

### Task 1: Relocate architecture and specification documents

**Files:**

- Create: `docs/architecture/01-platform-architecture.md`
- Create: `docs/architecture/02-whole-program-analysis-architecture.md`
- Create: `docs/architecture/03-summarydb-storage-architecture.md`
- Create: `docs/architecture/04-evidence-ir-architecture.md`
- Create: `docs/specs/github-actions-ci-build-design-spec.md`
- Create: `docs/specs/milestones/m01-project-ingestion-program-context-design-spec.md`
- Create: `docs/specs/milestones/m02-identity-canonical-hashing-metadata-store-design-spec.md`
- Create: `docs/specs/milestones/m03-summary-ir-cas-object-store-design-spec.md`
- Create: `docs/specs/milestones/m04-clang-llvm-project-analysis-design-spec.md`
- Create: `docs/specs/milestones/m05-required-svf-analysis-design-spec.md`
- Create: `docs/specs/milestones/m06-thin-cpg-projection-design-spec.md`
- Create: `docs/specs/milestones/m07-reverse-dependency-incremental-scheduler-design-spec.md`
- Create: `docs/specs/milestones/m08-scc-wpa-souffle-fact-engine-design-spec.md`
- Create: `docs/specs/milestones/m08r-souffle-wpa-remediation-design-spec.md`
- Create: `docs/specs/milestones/m08r-souffle-wpa-architecture-refinement-design-spec.md`
- Create: `docs/specs/milestones/m09-provenance-fact-store-explain-api-design-spec.md`
- Create: `docs/specs/milestones/m10b-evidence-builder-input-apis-demo-design-spec.md`
- Create: `docs/specs/milestones/m11-m12-summarydb-ingest-adapters-design-spec.md`
- Remove: the corresponding current paths listed in specification Sections 4 and 5

**Interfaces:**

- Consumes: the approved path tables in specification Sections 4 and 5
- Produces: canonical architecture/specification paths for repository reference rewriting

- [ ] **Step 1: Verify the move precondition**

Run:

```bash
test -f docs/architecture/veritas-platform-architecture-design.md
test -f docs/specs/milestones/m1-build-intelligence-program-context-design-spec.md
test -f docs/superpowers/specs/2026-08-22-souffle-wpa-architecture-refinement-design.md
test ! -e docs/architecture/01-platform-architecture.md
test ! -e docs/specs/milestones/m01-project-ingestion-program-context-design-spec.md
```

Expected: exit 0; every source exists and representative targets do not.

- [ ] **Step 2: Move the architecture documents**

Run:

```bash
git mv docs/architecture/veritas-platform-architecture-design.md docs/architecture/01-platform-architecture.md
git mv docs/architecture/veritas-whole-program-analysis-design.md docs/architecture/02-whole-program-analysis-architecture.md
git mv docs/architecture/veritas-thin-summarydb-backends-design.md docs/architecture/03-summarydb-storage-architecture.md
git mv docs/architecture/veritas-evidence-ir-design.md docs/architecture/04-evidence-ir-architecture.md
```

Expected: four staged renames with unchanged contents.

- [ ] **Step 3: Move cross-cutting and milestone specifications**

Run the exact `git mv` pairs from specification Section 5, including:

```bash
git mv docs/specs/github-actions-ci-build-design.md docs/specs/github-actions-ci-build-design-spec.md
git mv docs/specs/veritas-summarydb-ingest-adapter-design.md docs/specs/milestones/m11-m12-summarydb-ingest-adapters-design-spec.md
git mv docs/specs/milestones/m1-build-intelligence-program-context-design-spec.md docs/specs/milestones/m01-project-ingestion-program-context-design-spec.md
git mv docs/specs/milestones/m2-identity-canonical-hashing-metadata-store-design-spec.md docs/specs/milestones/m02-identity-canonical-hashing-metadata-store-design-spec.md
git mv docs/specs/milestones/m3-summary-ir-cas-object-store-design-spec.md docs/specs/milestones/m03-summary-ir-cas-object-store-design-spec.md
git mv docs/specs/milestones/m4-clang-llvm-local-extraction-design-spec.md docs/specs/milestones/m04-clang-llvm-project-analysis-design-spec.md
git mv docs/specs/milestones/m5-svf-value-flow-pointer-adapter-design-spec.md docs/specs/milestones/m05-required-svf-analysis-design-spec.md
git mv docs/specs/milestones/m6-thin-veritas-cpg-projection-design-spec.md docs/specs/milestones/m06-thin-cpg-projection-design-spec.md
git mv docs/specs/milestones/m7-reverse-dependency-incremental-scheduler-design-spec.md docs/specs/milestones/m07-reverse-dependency-incremental-scheduler-design-spec.md
git mv docs/specs/milestones/m8-scc-wpa-souffle-fact-engine-design-spec.md docs/specs/milestones/m08-scc-wpa-souffle-fact-engine-design-spec.md
git mv docs/specs/milestones/m8r-souffle-wpa-remediation-design-spec.md docs/specs/milestones/m08r-souffle-wpa-remediation-design-spec.md
git mv docs/superpowers/specs/2026-08-22-souffle-wpa-architecture-refinement-design.md docs/specs/milestones/m08r-souffle-wpa-architecture-refinement-design-spec.md
git mv docs/specs/milestones/m9-provenance-fact-store-explain-api-design-spec.md docs/specs/milestones/m09-provenance-fact-store-explain-api-design-spec.md
git mv docs/specs/milestones/m10-evidence-builder-input-apis-demo-design-spec.md docs/specs/milestones/m10b-evidence-builder-input-apis-demo-design-spec.md
```

Expected: every Section 5 source path is absent and every target exists.

- [ ] **Step 4: Verify specification relocation**

Run:

```bash
test -f docs/architecture/01-platform-architecture.md
test -f docs/architecture/04-evidence-ir-architecture.md
test -f docs/specs/milestones/m01-project-ingestion-program-context-design-spec.md
test -f docs/specs/milestones/m08r-souffle-wpa-architecture-refinement-design-spec.md
test -f docs/specs/milestones/m11-m12-summarydb-ingest-adapters-design-spec.md
```

Expected: exit 0.

### Task 2: Relocate the roadmap and implementation plans

**Files:**

- Create: `docs/plans/veritas-backbone-milestone-roadmap.md`
- Create: `docs/plans/milestones/m00-project-skeleton-required-svf-toolchain-implementation-plan.md`
- Create: `docs/plans/milestones/m01-project-ingestion-program-context-implementation-plan.md`
- Create: `docs/plans/milestones/m02-identity-canonical-hashing-metadata-store-implementation-plan.md`
- Create: `docs/plans/milestones/m03-summary-ir-cas-object-store-implementation-plan.md`
- Create: `docs/plans/milestones/m04-clang-llvm-project-analysis-implementation-plan.md`
- Create: `docs/plans/milestones/m05-required-svf-analysis-implementation-plan.md`
- Create: `docs/plans/milestones/m06-thin-cpg-projection-implementation-plan.md`
- Create: `docs/plans/milestones/m07-reverse-dependency-incremental-scheduler-implementation-plan.md`
- Create: `docs/plans/milestones/m08-scc-wpa-souffle-fact-engine-implementation-plan.md`
- Create: `docs/plans/milestones/m08r-souffle-wpa-remediation-implementation-plan.md`
- Create: `docs/plans/milestones/m09-provenance-fact-store-explain-api-implementation-plan.md`
- Create: `docs/plans/milestones/m10b-evidence-builder-input-apis-demo-implementation-plan.md`
- Create: `docs/plans/milestones/m11-external-ir-adapter-implementation-plan.md`
- Create: `docs/plans/milestones/m12-external-facts-importer-implementation-plan.md`
- Remove: the corresponding current paths listed in specification Section 6

**Interfaces:**

- Consumes: the approved path table in specification Section 6
- Produces: canonical roadmap and plan paths for repository reference rewriting

- [ ] **Step 1: Create the milestone-plan directory and move the roadmap**

Run:

```bash
mkdir -p docs/plans/milestones
git mv docs/specs/veritas-backbone-milestones-and-implementation-plan.md docs/plans/veritas-backbone-milestone-roadmap.md
```

Expected: the roadmap is staged as a rename across document classes.

- [ ] **Step 2: Move every milestone implementation plan**

Run:

```bash
git mv docs/plans/m0-project-skeleton-required-svf-toolchain-implementation-plan.md docs/plans/milestones/m00-project-skeleton-required-svf-toolchain-implementation-plan.md
git mv docs/plans/m1-build-intelligence-program-context-implementation-plan.md docs/plans/milestones/m01-project-ingestion-program-context-implementation-plan.md
git mv docs/plans/m2-identity-canonical-hashing-metadata-store-implementation-plan.md docs/plans/milestones/m02-identity-canonical-hashing-metadata-store-implementation-plan.md
git mv docs/plans/m3-summary-ir-cas-object-store-implementation-plan.md docs/plans/milestones/m03-summary-ir-cas-object-store-implementation-plan.md
git mv docs/plans/m4-clang-llvm-local-extraction-implementation-plan.md docs/plans/milestones/m04-clang-llvm-project-analysis-implementation-plan.md
git mv docs/plans/m5-svf-value-flow-pointer-adapter-implementation-plan.md docs/plans/milestones/m05-required-svf-analysis-implementation-plan.md
git mv docs/plans/m6-thin-veritas-cpg-projection-implementation-plan.md docs/plans/milestones/m06-thin-cpg-projection-implementation-plan.md
git mv docs/plans/m7-reverse-dependency-incremental-scheduler-implementation-plan.md docs/plans/milestones/m07-reverse-dependency-incremental-scheduler-implementation-plan.md
git mv docs/plans/m8-scc-wpa-souffle-fact-engine-implementation-plan.md docs/plans/milestones/m08-scc-wpa-souffle-fact-engine-implementation-plan.md
git mv docs/superpowers/plans/2026-08-22-souffle-wpa-remediation-bridge-implementation-plan.md docs/plans/milestones/m08r-souffle-wpa-remediation-implementation-plan.md
git mv docs/plans/m9-provenance-fact-store-explain-api-implementation-plan.md docs/plans/milestones/m09-provenance-fact-store-explain-api-implementation-plan.md
git mv docs/plans/m10-evidence-builder-input-apis-demo-implementation-plan.md docs/plans/milestones/m10b-evidence-builder-input-apis-demo-implementation-plan.md
git mv docs/plans/m11-external-ir-adapter-implementation-plan.md docs/plans/milestones/m11-external-ir-adapter-implementation-plan.md
git mv docs/plans/m12-external-facts-importer-implementation-plan.md docs/plans/milestones/m12-external-facts-importer-implementation-plan.md
```

Expected: all milestone plans are staged below `docs/plans/milestones/`.

- [ ] **Step 3: Verify the plan taxonomy and remove empty legacy directories**

Run:

```bash
test -f docs/plans/veritas-backbone-milestone-roadmap.md
test -f docs/plans/milestones/m00-project-skeleton-required-svf-toolchain-implementation-plan.md
test -f docs/plans/milestones/m08r-souffle-wpa-remediation-implementation-plan.md
test -f docs/plans/milestones/m12-external-facts-importer-implementation-plan.md
test -z "$(git ls-files docs/superpowers)"
```

Expected: exit 0; Git tracks no file below `docs/superpowers/`.

### Task 3: Rewrite repository references and navigation

**Files:**

- Create: `docs/README.md`
- Create: `docs/architecture/README.md`
- Create: `docs/specs/README.md`
- Modify: `docs/specs/milestones/README.md`
- Modify: `docs/plans/README.md`
- Modify: `.claude/rules/docs-layout.md`
- Modify: `CLAUDE.md`
- Modify: `README.md`
- Modify: every tracked Markdown file that cites a migrated current path
- Exclude from mechanical rewriting: the migration specification and this plan, which intentionally preserve old-to-new mappings

**Interfaces:**

- Consumes: all canonical paths produced by Tasks 1 and 2
- Produces: navigable documentation indexes, normative layout policy, and zero stale current-path references

- [ ] **Step 1: Mechanically replace migrated paths**

Apply every old-to-new mapping from specification Sections 4–6 to tracked
Markdown files except these migration records:

```text
docs/specs/veritas-documentation-information-architecture-design-specification.md
docs/plans/documentation-information-architecture-migration-implementation-plan.md
```

Use literal whole-path replacement, then inspect every changed line with:

```bash
git diff -- '*.md'
```

Expected: all path-only edits preserve surrounding prose and Markdown labels.

- [ ] **Step 2: Add the document indexes with exact responsibilities**

Write:

```text
docs/README.md                  document classes, primary reading order, milestone navigation
docs/architecture/README.md     ordered architecture list 01–04 with one-sentence scope summaries
docs/specs/README.md            cross-cutting specs plus milestone-spec entrypoint
docs/specs/milestones/README.md milestone/status/spec/plan/issue matrix
docs/plans/README.md            roadmap, tooling plan, and milestone-plan index
```

Every index link must be relative to the index file and use the canonical path
from the specification. The milestone matrices must identify the implemented
state of M0–M8, the approved/pending state of M8R, and the planned state of
M9–M12 without changing milestone requirements.

- [ ] **Step 3: Make the canonical layout normative**

Update `.claude/rules/docs-layout.md` to state:

```text
Architecture documents: docs/architecture/NN-topic-slug-architecture.md
Cross-cutting design specs: docs/specs/topic-slug-design-spec.md
Milestone design specs: docs/specs/milestones/mNN-or-mNNsuffix-topic-slug-design-spec.md
Project/tooling plans: docs/plans/topic-slug-implementation-plan.md
Milestone plans: docs/plans/milestones/mNN-or-mNNsuffix-topic-slug-implementation-plan.md
Forbidden: docs/superpowers/
Required indexes: docs/README.md plus README.md in architecture, specs, specs/milestones, and plans
```

Update `CLAUDE.md` and `README.md` to point readers to `docs/README.md`, the
renamed architecture documents, milestone specs, and milestone plans. Correct
the stale README statement that describes the repository as design-only.

- [ ] **Step 4: Verify current repository references**

Run a stale-path scan over tracked Markdown excluding the two migration
records. Search for every current-path value in the left column of
specification Sections 4–6.

Expected: zero matches outside the specification and this plan.

- [ ] **Step 5: Verify index coverage and link targets**

Run:

```bash
rg --files docs/architecture docs/specs/milestones docs/plans/milestones | sort
rg -n '\]\([^)]*\.md(?:#[^)]*)?\)' docs/README.md docs/architecture/README.md docs/specs/README.md docs/specs/milestones/README.md docs/plans/README.md
git diff --check
```

Expected: every canonical document appears in exactly one appropriate index;
all displayed local targets exist; `git diff --check` emits no output.

### Task 4: Commit and verify the local migration

**Files:**

- Stage: every intended documentation rename, index, policy edit, and reference update
- Do not stage: `build/` or unrelated files

**Interfaces:**

- Consumes: complete repository migration from Tasks 1–3
- Produces: one migration commit whose SHA is the authority for GitHub links

- [ ] **Step 1: Review the complete migration diff**

Run:

```bash
git status --short
git diff --stat HEAD
git diff --summary HEAD
git diff --check
```

Expected: only intended docs, `CLAUDE.md`, `README.md`, and
`.claude/rules/docs-layout.md` appear; old/new pairs are recognized as renames
where contents were not otherwise edited.

- [ ] **Step 2: Commit the migration**

Run:

```bash
git add .claude/rules/docs-layout.md CLAUDE.md README.md docs
git diff --cached --check
git commit -m "docs: reorganize documentation hierarchy"
```

Expected: commit succeeds and `git rev-parse HEAD` returns the migration SHA.

- [ ] **Step 3: Verify the committed tree**

Run:

```bash
git status --porcelain
git ls-tree -r --name-only HEAD docs | sort
```

Expected: clean worktree; canonical paths present; no `docs/superpowers/`
entry.

### Task 5: Run required pre-push verification and publish the branch

**Files:**

- Rebuild: task-worktree `build/` directory only
- Push: `claude/docs-information-architecture`

**Interfaces:**

- Consumes: committed local migration
- Produces: verified remote branch and migration pull request

- [ ] **Step 1: Perform the repository's clean build**

Run from the task worktree after removing only its absolute `build/` directory:

```bash
rm -rf /Users/skg7on/Workspace/Projects/VERITAS/.claude/worktrees/docs-information-architecture/build
cmake --preset default -DLLVM_PROJECT_BUILD_DIR=/Users/skg7on/Workspace/Projects/llvm-project/build
cmake --build --preset default -- -j1
```

Expected: configure and all build targets complete with zero errors. `-j1`
avoids the confirmed baseline-only five-second GoogleTest discovery timeout
caused by concurrent cold starts on this host.

- [ ] **Step 2: Run the complete test suite and formatting checks**

Run:

```bash
ctest --preset default --output-on-failure
git diff --check origin/main..HEAD
git status --porcelain
```

Expected: CTest exits 0 with 212 passing tests and the existing explicit
environment-dependent Soufflé comparison skip; no whitespace errors; clean
worktree.

- [ ] **Step 3: Push and create the migration PR**

Run:

```bash
git push -u origin claude/docs-information-architecture
gh pr create \
  --base main \
  --head claude/docs-information-architecture \
  --title "docs: reorganize documentation hierarchy" \
  --body $'## Summary\n\n- order architecture documents by reading sequence\n- preserve separate specification and plan taxonomies with parallel milestone subtrees\n- move M8R artifacts out of the forbidden docs/superpowers tree\n- add canonical navigation indexes and update every repository reference\n\n## Verification\n\n- clean serial CMake/Ninja build completed\n- CTest: 212 passed, 1 documented Soufflé environment skip\n- git diff --check origin/main..HEAD'
```

The PR body must summarize the architecture/spec/plan hierarchy, the removal
of `docs/superpowers/`, repository reference verification, and the exact build
and CTest results.

Expected: branch push and PR creation succeed.

### Task 6: Migrate and verify GitHub issue and PR references

**Files:**

- External update: open issue descriptions `#12`, `#13`, `#20`, `#21`, and `#61`
- Preserve unchanged: closed issue descriptions `#3`–`#11` and merged PR descriptions `#1`, `#2`, `#18`, `#19`, `#24`, `#27`, `#36`, and `#42`, including their original paths and revisions
- Preserve unchanged: open PR `#63`, which has no migrated-document path
- Preserve: comments, titles, labels, states, checklist state, surrounding prose, and PR #19's intentionally historical monolith wording

**Interfaces:**

- Consumes: the pushed migration commit SHA and the old-to-new tables in specification Sections 4–6
- Produces: only the five eligible open-issue descriptions with current-document references resolving to immutable migrated files

- [ ] **Step 1: Capture the immutable link revision**

Run:

```bash
MIGRATION_COMMIT="$(git rev-parse HEAD)"
REMOTE_MIGRATION_COMMIT="$(git ls-remote origin refs/heads/claude/docs-information-architecture | cut -f1)"
test "$REMOTE_MIGRATION_COMMIT" = "$MIGRATION_COMMIT"
```

Expected: the commit is reachable from the pushed remote branch.

- [ ] **Step 2: Update only the eligible open-issue descriptions**

The binding final scope supersedes the original broader issue/PR selection.
For each eligible open issue body (`#12`, `#13`, `#20`, `#21`, and `#61`) only:

1. Replace every current old path using the exact mapping in specification
   Sections 4–6.
2. For Markdown links to a migrated document, replace the existing
   `blob/main/` or prior commit revision with
   `blob/$MIGRATION_COMMIT/`.
3. Preserve link labels, checklist state, surrounding prose, and all other
   body content.
4. PATCH only when the resulting body differs.

Do not fetch, PATCH, or otherwise target closed issues `#3`–`#11`, merged PRs
`#1`, `#2`, `#18`, `#19`, `#24`, `#27`, `#36`, and `#42`, or open PR `#63`.
They are outside this rerunnable migration operation and retain their original
paths and revisions.

Use this exact zsh update, which applies the specification mapping to only the
five eligible open-issue descriptions:

```zsh
MIGRATION_COMMIT="$(git rev-parse HEAD)"
typeset -A DOC_PATH_MOVES
DOC_PATH_MOVES=(
  docs/architecture/veritas-platform-architecture-design.md docs/architecture/01-platform-architecture.md
  docs/architecture/veritas-whole-program-analysis-design.md docs/architecture/02-whole-program-analysis-architecture.md
  docs/architecture/veritas-thin-summarydb-backends-design.md docs/architecture/03-summarydb-storage-architecture.md
  docs/architecture/veritas-evidence-ir-design.md docs/architecture/04-evidence-ir-architecture.md
  docs/specs/github-actions-ci-build-design.md docs/specs/github-actions-ci-build-design-spec.md
  docs/specs/veritas-summarydb-ingest-adapter-design.md docs/specs/milestones/m11-m12-summarydb-ingest-adapters-design-spec.md
  docs/specs/milestones/m1-build-intelligence-program-context-design-spec.md docs/specs/milestones/m01-project-ingestion-program-context-design-spec.md
  docs/specs/milestones/m2-identity-canonical-hashing-metadata-store-design-spec.md docs/specs/milestones/m02-identity-canonical-hashing-metadata-store-design-spec.md
  docs/specs/milestones/m3-summary-ir-cas-object-store-design-spec.md docs/specs/milestones/m03-summary-ir-cas-object-store-design-spec.md
  docs/specs/milestones/m4-clang-llvm-local-extraction-design-spec.md docs/specs/milestones/m04-clang-llvm-project-analysis-design-spec.md
  docs/specs/milestones/m5-svf-value-flow-pointer-adapter-design-spec.md docs/specs/milestones/m05-required-svf-analysis-design-spec.md
  docs/specs/milestones/m6-thin-veritas-cpg-projection-design-spec.md docs/specs/milestones/m06-thin-cpg-projection-design-spec.md
  docs/specs/milestones/m7-reverse-dependency-incremental-scheduler-design-spec.md docs/specs/milestones/m07-reverse-dependency-incremental-scheduler-design-spec.md
  docs/specs/milestones/m8-scc-wpa-souffle-fact-engine-design-spec.md docs/specs/milestones/m08-scc-wpa-souffle-fact-engine-design-spec.md
  docs/specs/milestones/m8r-souffle-wpa-remediation-design-spec.md docs/specs/milestones/m08r-souffle-wpa-remediation-design-spec.md
  docs/superpowers/specs/2026-08-22-souffle-wpa-architecture-refinement-design.md docs/specs/milestones/m08r-souffle-wpa-architecture-refinement-design-spec.md
  docs/specs/milestones/m9-provenance-fact-store-explain-api-design-spec.md docs/specs/milestones/m09-provenance-fact-store-explain-api-design-spec.md
  docs/specs/milestones/m10-evidence-builder-input-apis-demo-design-spec.md docs/specs/milestones/m10b-evidence-builder-input-apis-demo-design-spec.md
  docs/specs/veritas-backbone-milestones-and-implementation-plan.md docs/plans/veritas-backbone-milestone-roadmap.md
  docs/plans/m0-project-skeleton-required-svf-toolchain-implementation-plan.md docs/plans/milestones/m00-project-skeleton-required-svf-toolchain-implementation-plan.md
  docs/plans/m1-build-intelligence-program-context-implementation-plan.md docs/plans/milestones/m01-project-ingestion-program-context-implementation-plan.md
  docs/plans/m2-identity-canonical-hashing-metadata-store-implementation-plan.md docs/plans/milestones/m02-identity-canonical-hashing-metadata-store-implementation-plan.md
  docs/plans/m3-summary-ir-cas-object-store-implementation-plan.md docs/plans/milestones/m03-summary-ir-cas-object-store-implementation-plan.md
  docs/plans/m4-clang-llvm-local-extraction-implementation-plan.md docs/plans/milestones/m04-clang-llvm-project-analysis-implementation-plan.md
  docs/plans/m5-svf-value-flow-pointer-adapter-implementation-plan.md docs/plans/milestones/m05-required-svf-analysis-implementation-plan.md
  docs/plans/m6-thin-veritas-cpg-projection-implementation-plan.md docs/plans/milestones/m06-thin-cpg-projection-implementation-plan.md
  docs/plans/m7-reverse-dependency-incremental-scheduler-implementation-plan.md docs/plans/milestones/m07-reverse-dependency-incremental-scheduler-implementation-plan.md
  docs/plans/m8-scc-wpa-souffle-fact-engine-implementation-plan.md docs/plans/milestones/m08-scc-wpa-souffle-fact-engine-implementation-plan.md
  docs/superpowers/plans/2026-08-22-souffle-wpa-remediation-bridge-implementation-plan.md docs/plans/milestones/m08r-souffle-wpa-remediation-implementation-plan.md
  docs/plans/m9-provenance-fact-store-explain-api-implementation-plan.md docs/plans/milestones/m09-provenance-fact-store-explain-api-implementation-plan.md
  docs/plans/m10-evidence-builder-input-apis-demo-implementation-plan.md docs/plans/milestones/m10b-evidence-builder-input-apis-demo-implementation-plan.md
  docs/plans/m11-external-ir-adapter-implementation-plan.md docs/plans/milestones/m11-external-ir-adapter-implementation-plan.md
  docs/plans/m12-external-facts-importer-implementation-plan.md docs/plans/milestones/m12-external-facts-importer-implementation-plan.md
)
REMOTE_BODY_NUMBERS=(12 13 20 21 61)
for REMOTE_BODY_NUMBER in $REMOTE_BODY_NUMBERS; do
  CURRENT_REMOTE_BODY="$(gh api "repos/skg7on/VERITAS/issues/$REMOTE_BODY_NUMBER" --jq .body)"
  UPDATED_REMOTE_BODY="$CURRENT_REMOTE_BODY"
  for OLD_DOC_PATH in ${(k)DOC_PATH_MOVES}; do
    UPDATED_REMOTE_BODY="${UPDATED_REMOTE_BODY//$OLD_DOC_PATH/${DOC_PATH_MOVES[$OLD_DOC_PATH]}}"
  done
  UPDATED_REMOTE_BODY="$(printf '%s' "$UPDATED_REMOTE_BODY" | perl -0pe 's{https://github\.com/skg7on/VERITAS/blob/[^/]+/(?=docs/(?:architecture/0[1-4]-|specs/(?:github-actions-ci-build-design-spec\.md|milestones/)|plans/(?:veritas-backbone-milestone-roadmap\.md|milestones/)))}{https://github.com/skg7on/VERITAS/blob/'"$MIGRATION_COMMIT"'/}g')"
  if [[ "$UPDATED_REMOTE_BODY" != "$CURRENT_REMOTE_BODY" ]]; then
    gh api --method PATCH "repos/skg7on/VERITAS/issues/$REMOTE_BODY_NUMBER" -f body="$UPDATED_REMOTE_BODY" --silent
  fi
done
```

Expected: only an eligible body that differs is updated; the operation never
targets a closed issue, merged PR, or PR #63.

- [ ] **Step 3: Verify eligible immutable links**

Fetch only the five eligible open-issue bodies and verify:

```text
- no current old path remains;
- every migrated Markdown URL uses the migration commit SHA;
- every docs path after /blob/$MIGRATION_COMMIT/ exists in that commit;
```

Closed issues `#3`–`#11`, merged PRs `#1`, `#2`, `#18`, `#19`, `#24`, `#27`,
`#36`, and `#42`, and open PR `#63` are deliberately excluded from this
rerunnable verification operation; their original paths, revisions, comments,
titles, labels, states, checklist state, and surrounding prose remain
unchanged. This includes PR #19's historical monolith wording.

Resolve every unique migrated URL target with this self-contained check:

```zsh
MIGRATION_COMMIT="$(git rev-parse HEAD)"
REMOTE_BODY_NUMBERS=(12 13 20 21 61)
for REMOTE_BODY_NUMBER in $REMOTE_BODY_NUMBERS; do
  gh api "repos/skg7on/VERITAS/issues/$REMOTE_BODY_NUMBER" --jq .body
done | perl -nle 'while (m{https://github\.com/skg7on/VERITAS/blob/[0-9a-f]+/(docs/[^)[:space:]]+\.md)}g) { print $1 }' | sort -u | while IFS= read -r CANONICAL_DOC_PATH; do
  gh api "repos/skg7on/VERITAS/contents/$CANONICAL_DOC_PATH?ref=$MIGRATION_COMMIT" --jq .path
done
```

Expected: each request returns the same canonical path and no request returns
404.

- [ ] **Step 4: Report final repository states**

Run:

```bash
git -C /Users/skg7on/Workspace/Projects/VERITAS branch --show-current
git -C /Users/skg7on/Workspace/Projects/VERITAS status --porcelain
git branch --show-current
git status --porcelain
```

Expected: primary checkout is clean on `main`; task worktree is clean on
`claude/docs-information-architecture`.
