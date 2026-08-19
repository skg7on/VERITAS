# Pre-Push Verification Policy

This policy applies to every push to the remote repository, whether direct or via pull request. It is mandatory and non-negotiable.

## Scope

Every push must pass full local verification before reaching the remote. This includes:

- feature branches and bug fixes;
- documentation, specs, and implementation plans;
- build configuration, CMake files, and dependencies;
- test code, fixtures, and CI/CD workflows;
- refactors, performance improvements, and experimental work.

Read-only changes (typo fixes in markdown comments, README edits with no code impact) may skip the build verification but must still pass the test suite if tests exist.

## Required Verification Steps

Before pushing any branch to the remote, Claude must complete all applicable verification steps in the task worktree and confirm they pass.

### 1. Clean Build

Run a full clean build from scratch:

```bash
rm -rf build
cmake --preset default -DLLVM_PROJECT_BUILD_DIR=/path/to/llvm-project/build
cmake --build --preset default
```

The build must complete with **zero errors**. Warnings are acceptable but should be minimized.

**Pass criteria:**
- `cmake --build` exits with code 0
- All targets build successfully
- All expected binaries appear in `build/bin/`

**When to skip:** Never. Even documentation-only changes must verify the build still works.

### 2. Test Suite

Run the complete test suite:

```bash
cd build
ctest --output-on-failure
```

Every test must pass. Flaky tests must be fixed or disabled before pushing.

**Pass criteria:**
- `ctest` exits with code 0
- 100% of tests pass
- No crashes, timeouts, or assertion failures

**When to skip:** Only when `VERITAS_BUILD_TESTS=OFF` was explicitly set, or when adding the very first test to an untested area. In the latter case, verify the new test passes.

### 3. Formatting Check

Verify code formatting meets project standards:

```bash
git diff --check
```

This catches trailing whitespace, mixed line endings, and other formatting issues.

**Pass criteria:**
- `git diff --check` exits with code 0
- No whitespace errors reported

**When to skip:** Never. This is a free check.

### 4. License Header Check

Verify all modified source files have the required Apache-2.0 header:

```bash
for file in $(git diff --name-only origin/main); do
  case "$file" in
    *.h|*.hpp|*.hh|*.c|*.cc|*.cpp|*.cxx|CMakeLists.txt|*.cmake)
      if ! head -20 "$file" | grep -q 'Licensed under the Apache License, Version 2.0'; then
        echo "Missing license header: $file"
      fi
      ;;
  esac
done
```

**Pass criteria:**
- No missing headers reported
- Headers use correct year and copyright holder

**When to skip:** When modifying only third-party code under `third_party/` or generated files under `build/`.

### 5. Clean Working Tree

Verify the task worktree is clean after all changes are committed:

```bash
git status --porcelain
```

**Pass criteria:**
- Command returns no output
- No untracked files that should be committed
- No uncommitted changes

**When to skip:** Never. Uncommitted changes indicate incomplete work.

## Execution Checklist

Before every push, Claude must:

1. ✅ Run clean build and verify exit code 0
2. ✅ Run full test suite and verify 100% pass rate
3. ✅ Run `git diff --check` and verify no whitespace errors
4. ✅ Verify license headers on all modified in-scope files
5. ✅ Verify working tree is clean
6. ✅ Review the diff against `main` and confirm only intended changes
7. ✅ Push the branch

If any step fails, Claude must:
- Stop immediately and report the failure
- Fix the root cause
- Restart verification from step 1

Do not skip failed steps, ignore flaky tests, or push partial fixes with the intent to "fix forward."

## Reporting Results

After completing verification, Claude must report the results explicitly:

```
Pre-push verification completed:
✅ Clean build passed (0 errors, N warnings)
✅ Test suite passed (X/X tests, 0 failures)
✅ Formatting check passed
✅ License headers verified
✅ Working tree clean
✅ Ready to push
```

If any step failed, report the failure and the remediation:

```
Pre-push verification failed:
❌ Test suite: 2/47 tests failed
   - MetadataStoreTest::InsertDuplicate (assertion failure)
   - SummaryRepositoryTest::IncrementalUpdate (segfault)

Remediation: Fix both test failures and re-run full verification.
```

## CI/CD Integration

This policy is enforced locally before push. It does not replace CI/CD verification, but it ensures the CI pipeline starts from a known-good state.

When the CI pipeline fails after a push that passed local verification, the difference indicates an environment-specific issue (missing dependency, platform difference, or non-deterministic test). Investigate and fix the CI failure, then update local verification to catch the same issue next time.

## Exemptions

There are **no exemptions** to this policy. Every push must pass verification.

"I'll fix it in the next commit" is not an exemption. Fix it before pushing.

"The test is flaky" is not an exemption. Fix or disable the test before pushing.

"It's just a doc change" is not an exemption. Verify the build and tests still pass.

## Failure Recovery

If Claude accidentally pushes without completing verification:

1. Stop all further pushes immediately
2. Report the violation to the user
3. Run verification in the pushed branch
4. If verification fails, either:
   - Push a fix commit if the failure is trivial
   - Revert the push and fix locally if the failure is non-trivial

Never force-push to erase a verification failure without user approval.

## Related Policies

- `.claude/rules/git-worktree-policy.md` — Worktree creation and integration rules
- `.claude/rules/cpp-compilation-policy.md` — RTTI and exception constraints that affect build
- `.claude/rules/license-header-policy.md` — Required header format and verification
