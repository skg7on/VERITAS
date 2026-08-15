# CLAUDE.md

## Mandatory Git Worktree Policy

This policy applies to every Claude Code session in this repository. It is non-negotiable.

The primary checkout exists to keep `main` clean and to integrate completed work. Claude must never perform repository-changing work directly in the primary checkout or directly on `main`.

## Scope

Use a dedicated Git worktree for every task that may change repository state, including:

- architecture and design specifications;
- implementation plans and milestone documents;
- features, experiments, refactors, and performance work;
- bug fixes and regression tests;
- documentation, examples, and diagrams;
- build files, dependencies, generated schemas, and project settings;
- CI/CD workflows, scripts, tooling, and developer configuration;
- test fixtures, snapshots, golden files, and benchmarks;
- repository policy files, including this file.

Read-only investigation may happen in the primary checkout only when it produces no files, build artifacts, caches, formatting changes, or other repository mutations.

## Invariants

Claude must preserve all of these invariants:

1. The primary checkout remains on branch `main`.
2. The primary checkout remains clean: `git status --porcelain` returns no output.
3. No task commit is created directly on `main`.
4. Every repository-changing task uses its own branch and its own linked worktree.
5. All edits, builds, tests, task commits, task-branch pushes, and pull-request updates happen in the task worktree.
6. `main` receives work only by merging a completed task branch, normally through a pull request.
7. Merge conflicts are resolved on the task branch in its worktree, never by editing files on `main`.
8. A failure to create or use a worktree is a blocker. It is not permission to work on `main`.

## Allowed Operations on Main

In the primary checkout, Claude may only:

- inspect repository state and history;
- run read-only searches and file reads;
- run `git fetch`;
- fast-forward `main` with `git pull --ff-only`;
- list, add, lock, unlock, prune, or remove worktrees safely;
- merge a verified task branch when the user explicitly requests local integration;
- push `main` only after such an explicitly requested and verified local merge;
- verify the final merged state without producing tracked or untracked files.

Claude must not switch the primary checkout away from `main`, edit files, generate files, run mutating formatters, install dependencies, build, test, create direct task commits, amend, rebase, or resolve conflicts there.

## Required Preflight

Before the first repository write for any task:

1. Locate the primary checkout and inspect its state:

   ```bash
   git branch --show-current
   git status --porcelain
   git worktree list
   ```

2. Confirm the primary checkout is on `main` and clean. If it is dirty, stop and report the existing changes. Do not stash, reset, overwrite, or delete work that may belong to the user.

3. Synchronize safely when needed:

   ```bash
   git fetch origin
   git pull --ff-only
   ```

   If `main` cannot fast-forward, stop and report the divergence.

4. Choose a unique, descriptive branch named `claude/<task-slug>`.

5. Create a sibling worktree without switching the primary checkout away from `main`:

   ```bash
   PRIMARY_ROOT="$(git rev-parse --show-toplevel)"
   REPOSITORY_NAME="$(basename "$PRIMARY_ROOT")"
   WORKTREE_PARENT="$(dirname "$PRIMARY_ROOT")/${REPOSITORY_NAME}-worktrees"
   TASK_SLUG="<task-slug>"
   TASK_BRANCH="claude/${TASK_SLUG}"
   TASK_WORKTREE="${WORKTREE_PARENT}/${TASK_SLUG}"

   mkdir -p "$WORKTREE_PARENT"
   git worktree add -b "$TASK_BRANCH" "$TASK_WORKTREE" main
   ```

6. Change into the task worktree and verify isolation:

   ```bash
   cd "$TASK_WORKTREE"
   git branch --show-current
   git status --porcelain
   git rev-parse --git-dir
   git rev-parse --git-common-dir
   ```

   The branch must not be `main`, the task worktree must be clean, and its Git directory must differ from the common Git directory.

## Worktree Execution Rules

Inside the task worktree, Claude must:

- keep the branch limited to one coherent task;
- preserve unrelated user changes and avoid unrelated refactors;
- perform all file creation, editing, deletion, generation, formatting, setup, builds, and tests there;
- follow the repository's design, implementation, and verification requirements;
- commit only after task-appropriate verification succeeds;
- push the task branch and use it for pull-request creation and updates;
- remain in the same worktree for review feedback and follow-up fixes.

Claude must not reuse another task's worktree or branch merely because it already exists.

## Verification Before Integration

Before proposing or performing a merge:

1. Run the full task-appropriate test, build, lint, and documentation checks in the task worktree.
2. Run `git diff --check`.
3. Confirm `git status --porcelain` is empty after committing.
4. Review the branch diff against `main` and confirm it contains only intended changes.
5. Push the branch and open or update its pull request.
6. Report any unavailable checks explicitly; do not describe unrun checks as passing.

## Integration Rules

Pull-request integration is the default. Direct pushes of task commits to `main` are forbidden.

When the user explicitly requests a local merge, Claude must:

1. Verify the task worktree is clean and all required checks pass.
2. Return to the primary checkout and verify it is still on clean `main`.
3. Run `git fetch origin` and `git pull --ff-only`.
4. If the task branch conflicts with updated `main`, return to the task worktree, integrate `main` there, resolve conflicts there, and rerun verification.
5. Merge the verified task branch into `main`.
6. Verify the merged result before pushing `main`.

Do not create ad hoc fix commits on `main` before, during, or after integration. Any required correction belongs on the task branch and must be merged from there.

## Cleanup Rules

Keep the task worktree while its pull request is open. Remove it only after the branch is merged or the user explicitly abandons the task.

Before removal:

```bash
git -C "$TASK_WORKTREE" status --porcelain
```

If the command reports changes, stop. Never use forced worktree removal when uncommitted files may be lost.

After successful integration and a clean check:

```bash
git worktree remove "$TASK_WORKTREE"
git worktree prune
git branch -d "$TASK_BRANCH"
```

Delete the remote branch only when repository policy or the user calls for it.

## Failure Handling

Stop and report the blocker instead of working on `main` when:

- the primary checkout is dirty;
- `main` is missing, detached, or diverged;
- the branch name or worktree path already exists unexpectedly;
- worktree creation fails;
- the task worktree cannot be made clean;
- baseline tests fail before task changes;
- integration would require resolving conflicts on `main`;
- cleanup would discard uncommitted work.

If Claude accidentally changes the primary checkout, it must stop immediately, make no destructive recovery attempt, report the affected paths, and move the work safely only after the user approves the recovery approach.

## Final Check

At the end of every repository-changing task, Claude must verify and report both states:

```bash
git -C "<primary-checkout>" branch --show-current
git -C "<primary-checkout>" status --porcelain
git -C "<task-worktree>" branch --show-current
git -C "<task-worktree>" status --porcelain
```

The primary checkout must be clean on `main`. The task worktree must contain the task branch and be clean after its changes are committed.
