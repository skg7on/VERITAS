# M13 Scale Profile and Store Equivalence Instrument Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `--scale-profile=baseline|scaled` to `veritas-build analyze` with
`baseline` as the default and byte-stable identity, and build the store
equivalence instrument that proves a `baseline` run reproduces today's published
content.

**Architecture:** Two independent additions. The scale profile is one enum field
on `AnalysisConfig`, one parse branch in the hand-rolled `analyze` argument
parser, and one run-time rejection of `scaled` — the field is deliberately
**inert**: it is appended to no canonical config encoding, so every identity
(`AnalyzerRunID`, `WpaConfigurationHash`, `BatchId`, `LogicalInputHash`,
`FixpointHash`, `ExternalHash`) is byte-identical before and after this
milestone. The instrument is new: a `StoreEquivalence` library that dumps every
table of a `metadata.db` in a determined canonical form and digests it, plus a
`veritas-store-diff` CLI. Round 3 performed this comparison with ad-hoc
`sqlite3 … | shasum` shell and recorded the projection only in prose; M13 makes
it code with tests, so later milestones can be judged against it mechanically.

**Tech Stack:** C++20; CMake 3.23+ with Ninja; SQLite through the existing
`MetadataStore` wrapper; GoogleTest. No RTTI and no exceptions in VERITAS code.

**Spec:** `docs/plans/veritas-scaling-milestone-roadmap.md` §6 (Stage 0 / M13),
§13 (acceptance envelope), §14 (verification strategy). The instrument's
determined projection is recorded in
`docs/specs/veritas-build-analyze-round3-performance-design-spec.md` §9.1 — read
that section before Task 5, because this plan reproduces its determinations and
Task 6 re-determines them by measurement.

## Global Constraints

Copied from the spec. Every task's requirements implicitly include this section.

- **No identity may move.** Every canonical identity
  (`AnalyzerRunID`, `WpaConfigurationHash`, `BatchId`, `LogicalInputHash`,
  `FixpointHash`, `ExternalHash`) is byte-identical for unchanged input before
  and after this milestone. `src/analysis/ProjectAnalyzer.cpp:107-123`
  enumerates the fields that participate in `WpaConfigurationHash` explicitly, so
  the new `scale_profile` field is inert **only if it is never appended there**.
  Do not append it in this milestone.
- **`baseline` preserves today's semantics exactly.** No behaviour change of any
  kind on the default path.
- **`scaled` is parsed and then rejected at run time** with
  `Status::FailedPrecondition`. It must never be silently accepted, because a
  profile that claims to opt into structural changes and then performs none is
  worse than no profile at all. `Status` has no `Unimplemented` factory
  (`include/veritas/core/Status.h:46-58` exposes `InvalidArgument`, `NotFound`,
  `FailedPrecondition`, `Internal`, `DeadlineExceeded` only), so use
  `FailedPrecondition`.
- **The `RunReport` identity doctrine gains a second exception, and it must be
  recorded rather than left implied.** `include/veritas/observability/RunReport.h:144-149`
  states that `conformance_oracle` is "the one analysis-config knob no
  configuration hash covers". After Task 1 there are two, and the reasons are
  different in kind: `conformance_oracle` is uncovered because it changes what a
  run *does* and cannot be recovered from either config hash, whereas
  `scale_profile` is uncovered **deliberately and temporarily**, so that adding
  the field moves no identity. Task 1 Step 7 amends that comment and
  `docs/specs/veritas-build-analyze-phase-observability-design-spec.md` §6.3 to
  say so.
- **Why an uncovered knob is safe here and not later.** The doctrine warns that a
  difference inside the identity block means two runs were not like-for-like.
  An uncovered knob that changes a run therefore makes two genuinely different
  runs share one `run_id`. That is harmless in M13 only because `scaled` is
  rejected, so the knob cannot vary. **M15 must append `scale_profile` to
  `WpaConfigurationHash` when it gives the value meaning.** Record that
  obligation in the acceptance record (Task 7), because nothing else enforces it.
- **The instrument's projection is determined by measurement, never assumed.**
  Round 3's headline finding is that every magnitude attached to a real hot spot
  was wrong until a task measured it. Task 6 re-derives the projection against
  two real runs and corrects Task 4's table wherever measurement disagrees.
- **The instrument must never mutate the store it inspects.** Opening a store
  read-only must not create the file, apply schema, or change a byte.
- **The instrument is not the identity check.** `RunReport` compares the identity
  block *precisely because* a difference there means the runs were not
  like-for-like; `veritas-store-diff` compares **content** while excluding
  run-scoped columns *precisely so* that two legitimate independent runs compare
  equal. The two are complementary and neither replaces the other. A plan reader
  who sees "exclude the run id" here and "never exclude the identity block" in
  `RunReport.h` should not read that as a contradiction: one proves the runs are
  comparable, the other proves the comparable runs agree. Task 6 Step 5 uses both,
  in that order.
- **No RTTI, no exceptions.** Use `veritas::Status` and `StatusOr<T>`; never
  `dynamic_cast`, `typeid`, `throw`, `try`, or `catch` in VERITAS code.
- **Apache-2.0 header** in the first 20 lines of every created or modified
  `.cpp`, `.h`, `.py`, and `CMakeLists.txt`.
- **A `GTEST_SKIP` reports to CTest as a pass**, so a green summary proves
  nothing. No task may leave a skipped test. Tests registered with
  `gtest_discover_tests` here get **no per-test TIMEOUT** (only
  `DISCOVERY_TIMEOUT`, which bounds discovery, not the test); any test needing a
  timeout must use explicit `add_test` + `set_tests_properties`.
- All edits, builds, tests, and commits happen in the task worktree, never in the
  primary checkout and never on `main`.

## File Structure

Line numbers are as of `main` at `709f2d9`. Verify a symbol before editing; the
files move.

| File | Responsibility in this change |
| --- | --- |
| `include/veritas/analysis/ProjectAnalyzer.h` | Add `ScaleProfile` enum next to `WpaEngineMode` (`:43-46`) and the `scale_profile` field on `AnalysisConfig` (`:49-65`). |
| `src/analysis/ProjectAnalyzer.cpp` | `AnalysisConfig::Default()` (`:63`) returns `ScaleProfile::kBaseline`. Nothing else in this file changes — in particular `WpaConfigurationHash` (`:107-123`) is untouched. |
| `include/veritas/observability/RunReport.h` | Amend the `conformance_oracle` comment (`:144-149`) to name both uncovered knobs. No behaviour change. |
| `docs/specs/veritas-build-analyze-phase-observability-design-spec.md` | Amend §6.3's "one knob" claim to name both. |
| `src/tools/veritas-build.cpp` | `--scale-profile` parse branch modelled on `--wpa-engine` (`:185-192`), the `kUsage` table (`:45-57`) — which already carries the `--metrics*` lines and must keep them — and the `scaled` rejection beside the config build in `Analyze` (`:611-619`). |
| `include/veritas/summarydb/MetadataStore.h` | Add the read-only open beside `Open` (`:94`). |
| `src/summarydb/MetadataStore.cpp` | Implement it beside `Open` (`:118`). |
| `include/veritas/summarydb/StoreEquivalence.h` | New: the canonical dump and comparison vocabulary. |
| `src/summarydb/StoreEquivalence.cpp` | New: table enumeration, the determined projection, the canonical row stream and its digest. |
| `src/tools/veritas-store-diff.cpp` | New: the two-store CLI. |
| `src/tools/CMakeLists.txt` | Register the new CLI in the existing `foreach` list and link `veritas_summarydb`. |
| `tests/unit/analysis/AnalysisConfigTest.cpp` | New: the default and the inertness of the new field. |
| `tests/unit/summarydb/MetadataStoreTest.cpp` | Extended: read-only open does not create, migrate, or mutate. |
| `tests/unit/summarydb/StoreEquivalenceTest.cpp` | New: projection, digest determinism, and every perturbation. |
| `tests/unit/summarydb/StoreEquivalenceEndToEndTest.cpp` | New: two analyses of one fixture compare equivalent. |
| `tests/integration/build/VeritasBuildAnalyzeCliTest.cpp` | Extended: `--scale-profile` parsing, default, rejection, and stderr capture. |

---

### Task 1: `ScaleProfile` on `AnalysisConfig`, inert by construction

**Files:**
- Modify: `include/veritas/analysis/ProjectAnalyzer.h` (add the enum after `WpaEngineMode`, which sits at `:43-46`, and the field in `AnalysisConfig` at `:49-65`)
- Modify: `src/analysis/ProjectAnalyzer.cpp` (`AnalysisConfig::Default()` at `:63`)
- Modify: `include/veritas/observability/RunReport.h` (the comment at `:144-149`)
- Modify: `docs/specs/veritas-build-analyze-phase-observability-design-spec.md` (§6.3)
- Create: `tests/unit/analysis/AnalysisConfigTest.cpp`
- Modify: `tests/unit/analysis/CMakeLists.txt` (append a new target block)

**Interfaces:**
- Consumes: nothing.
- Produces: `veritas::analysis::ScaleProfile` with values `kBaseline` and `kScaled`; the `AnalysisConfig::scale_profile` field, defaulting to `ScaleProfile::kBaseline` from `AnalysisConfig::Default()`. Tasks 2 and 3 consume both.

- [ ] **Step 1: Write the failing test**

Create `tests/unit/analysis/AnalysisConfigTest.cpp`:

```cpp
// Copyright 2026 VERITAS Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "veritas/analysis/ProjectAnalyzer.h"

#include <gtest/gtest.h>

namespace veritas::analysis {
namespace {

// The default profile is `baseline`, which is today's behaviour. This is the
// whole safety argument for adding the field at all: an existing caller that
// constructs `AnalysisConfig::Default()` and knows nothing about scale profiles
// gets exactly the run it got before this milestone.
TEST(AnalysisConfigTest, DefaultProfileIsBaseline) {
  const AnalysisConfig config = AnalysisConfig::Default();
  EXPECT_EQ(config.scale_profile, ScaleProfile::kBaseline);
}

// `kBaseline` must be the first enumerator so that a zero-initialised
// `AnalysisConfig` is also `baseline`. Any other order silently makes
// `AnalysisConfig{}` mean `scaled`.
TEST(AnalysisConfigTest, BaselineIsTheZeroEnumerator) {
  EXPECT_EQ(static_cast<int>(ScaleProfile::kBaseline), 0);
}

TEST(AnalysisConfigTest, ProfileIsAssignable) {
  AnalysisConfig config = AnalysisConfig::Default();
  config.scale_profile = ScaleProfile::kScaled;
  EXPECT_EQ(config.scale_profile, ScaleProfile::kScaled);
}

}  // namespace
}  // namespace veritas::analysis
```

- [ ] **Step 2: Register the test target**

Append to `tests/unit/analysis/CMakeLists.txt`:

```cmake
add_executable(AnalysisConfigTest AnalysisConfigTest.cpp)
target_link_libraries(AnalysisConfigTest PRIVATE
  veritas_analysis
  GTest::gtest_main
)
veritas_add_warnings(AnalysisConfigTest)
gtest_discover_tests(AnalysisConfigTest DISCOVERY_TIMEOUT 60)
```

- [ ] **Step 3: Run the test to verify it fails**

```bash
cmake --build --preset default --target AnalysisConfigTest
```

Expected: the build fails compiling `AnalysisConfigTest.cpp` — `ScaleProfile` is
not a member of `veritas::analysis`, and `AnalysisConfig` has no
`scale_profile`.

- [ ] **Step 4: Add the enum and the field**

In `include/veritas/analysis/ProjectAnalyzer.h`, immediately after the
`WpaEngineMode` enumerator (`:43-46`), add:

```cpp
// The scale profile selects which structural implementation of the analysis
// pipeline a run uses. `kBaseline` is today's implementation and the default.
// `kScaled` is reserved for the M15-M19 changes specified in
// docs/plans/veritas-scaling-milestone-roadmap.md; until those land,
// `veritas-build analyze` rejects it rather than silently doing nothing.
//
// This value is deliberately absent from WpaConfigurationHash and every other
// canonical config encoding: it must not move any identity until the milestone
// that gives it meaning also versions the identity it changes.
enum class ScaleProfile : std::uint8_t {
  kBaseline = 0,
  kScaled = 1,
};
```

In `AnalysisConfig` (same file, `:49-65`), add the field after `wpa_engine`:

```cpp
  ScaleProfile scale_profile;
```

`AnalysisConfig::Default()` at `src/analysis/ProjectAnalyzer.cpp:63` is a single
`return AnalysisConfig{...}` with **designated initializers**, not a sequence of
assignments. Add a designated entry in declaration order, next to
`.wpa_engine = WpaEngineMode::kSouffle,`:

```cpp
      .scale_profile = ScaleProfile::kBaseline,
```

Do not write `config.scale_profile = ...;` — there is no `config` in that
function, and a designated-initializer aggregate cannot be assigned into after
construction without dropping the other designated entries.

- [ ] **Step 5: Run the test to verify it passes**

```bash
cmake --build --preset default --target AnalysisConfigTest
./build/bin/AnalysisConfigTest
```

Expected: `[  PASSED  ] 3 tests.`

- [ ] **Step 6: Prove the field is inert**

The identity encodings are file-static functions, so this cannot be unit-tested
directly. Instead, confirm by inspection and record it in the commit message:

```bash
grep -n "scale_profile" src/analysis/ProjectAnalyzer.cpp
```

Expected: exactly the `Default()` initialiser, and nothing else. If
`WpaConfigurationHash` (`:107-123`) or `SvfConfigurationHash`/`ToSvfConfig` show
a hit, the field is participating in an identity and this milestone's central
constraint is violated — remove it. Do not "fix" this by editing a hash; the
whole point is that the field is inert.

- [ ] **Step 7: Record that the uncovered-knob doctrine now has two exceptions**

`include/veritas/observability/RunReport.h:144-149` says `conformance_oracle` is
"the one analysis-config knob no configuration hash covers". That is now false.
Replace that sentence with wording that names both, and makes the different
reasons explicit — something equivalent to:

```cpp
  // Two knobs here are not covered by any configuration hash, for different
  // reasons. `conformance_oracle` changes what the run does — a second full WPA
  // whose canonical results must agree — and cannot be recovered from
  // svf_config_hash or wpa_config_hash. `scale_profile` is uncovered
  // *deliberately and temporarily*, so that introducing the field moves no
  // identity; the milestone that gives its value meaning must append it to
  // WpaConfigurationHash. Until then, only `baseline` is reachable at run time,
  // so the knob cannot vary and two runs can never differ in it alone.
  bool conformance_oracle = false;
```

Then make the same amendment in
`docs/specs/veritas-build-analyze-phase-observability-design-spec.md` §6.3,
which is the normative home of the claim. Add the M15 obligation to that
section, since nothing in code enforces it and Task 7's acceptance record is the
only other place it is written down.

- [ ] **Step 8: Run the tests to verify nothing else moved**

```bash
cmake --build --preset default
./build/bin/AnalysisConfigTest
ctest --test-dir build -R PhaseObservabilityIdentityTest --output-on-failure
```

Expected: `AnalysisConfigTest` passes, and the existing phase-observability
identity test still passes — the comment edit changed no behaviour, and that
test is the one that would notice if it had.

- [ ] **Step 9: Commit**

```bash
git add include/veritas/analysis/ProjectAnalyzer.h \
        src/analysis/ProjectAnalyzer.cpp \
        include/veritas/observability/RunReport.h \
        docs/specs/veritas-build-analyze-phase-observability-design-spec.md \
        tests/unit/analysis/AnalysisConfigTest.cpp \
        tests/unit/analysis/CMakeLists.txt
git commit -m "feat(analysis): add an inert ScaleProfile to AnalysisConfig"
```

---

### Task 2: `--scale-profile` on the `analyze` command

**Files:**
- Modify: `src/tools/veritas-build.cpp` — the `kUsage` table (`:45-57`), `AnalyzeArguments` (`:59-70`), a new branch in `ParseAnalyzeArguments` after the `--wpa-engine` branch (`:185-192`), and the `scaled` rejection beside the config build in `Analyze` (`:611-619`)
- Modify: `tests/integration/build/VeritasBuildAnalyzeCliTest.cpp` (add stderr capture and the new cases)

**Interfaces:**
- Consumes: `veritas::analysis::ScaleProfile` from Task 1.
- Produces: the `--scale-profile baseline|scaled` CLI surface, defaulting to
  `baseline`, with `scaled` rejected as a run-time `FailedPrecondition`. Task 7's
  LevelDB acceptance run consumes it.

- [ ] **Step 1: Write the failing tests**

The existing `CliResult` captures stdout only
(`tests/integration/build/VeritasBuildAnalyzeCliTest.cpp:42-45`). The rejection
message goes to stderr, so extend the harness first. In that file, replace the
`CliResult` struct with:

```cpp
struct CliResult {
  int exit_code = -1;
  std::string stdout_text;
  std::string stderr_text;
};
```

and change `RunVeritasBuild`'s command construction to capture both streams.
Find the line that appends the redirect and make it redirect stderr into stdout,
then split the two on the first occurrence of a sentinel line. The simplest
correct form is to append `2>&1` and record both streams together:

```cpp
  command += " 2>&1";
```

then set both `stdout_text` and `stderr_text` to the captured output, so a caller
that only cares that the message appeared can check either field. Add a comment
saying exactly that, because a reader will otherwise assume the split is real.

Then append these test cases at the end of the file, before the closing
namespace:

```cpp
// The profile defaults to `baseline`, so a run that never mentions the flag is
// byte-identical to a run that names it explicitly.
TEST(VeritasBuildAnalyzeCliTest, ScaleProfileDefaultsToBaseline) {
  const auto implicit = RunVeritasBuild(
      {"analyze", "--project", testing::FixtureProject("store_load").string()});
  const auto explicit_run = RunVeritasBuild(
      {"analyze", "--project", testing::FixtureProject("store_load").string(),
       "--scale-profile", "baseline"});
  EXPECT_EQ(implicit.exit_code, 0) << implicit.stderr_text;
  EXPECT_EQ(explicit_run.exit_code, 0) << explicit_run.stderr_text;
}

// `scaled` parses but is rejected at run time: no milestone has implemented any
// structural change yet, so accepting it would claim an opt-in that does
// nothing.
TEST(VeritasBuildAnalyzeCliTest, ScaledProfileIsRejectedWithAReason) {
  const auto result = RunVeritasBuild(
      {"analyze", "--project", testing::FixtureProject("store_load").string(),
       "--scale-profile", "scaled"});
  EXPECT_NE(result.exit_code, 0);
  EXPECT_NE(result.stderr_text.find("scaled"), std::string::npos)
      << "the rejection must name the profile; got: " << result.stderr_text;
  EXPECT_NE(result.stderr_text.find("veritas-scaling-milestone-roadmap"),
            std::string::npos)
      << "the rejection must point at the roadmap; got: " << result.stderr_text;
}

TEST(VeritasBuildAnalyzeCliTest, UnknownScaleProfileIsAUsageError) {
  const auto result = RunVeritasBuild(
      {"analyze", "--project", testing::FixtureProject("store_load").string(),
       "--scale-profile", "turbo"});
  EXPECT_NE(result.exit_code, 0);
  EXPECT_NE(result.stderr_text.find("--scale-profile"), std::string::npos)
      << result.stderr_text;
}
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cmake --build --preset default --target VeritasBuildAnalyzeCliTest
./build/bin/VeritasBuildAnalyzeCliTest --gtest_filter='*ScaleProfile*:*Scaled*'
```

Expected: FAIL — the first case fails because `--scale-profile` is an unknown
argument, and the other two fail because the process rejects the flag before
reaching any profile logic. Confirm each test failed for the reason you expect,
not for a compile error in the harness edit.

- [ ] **Step 3: Implement the parser and the rejection**

In `src/tools/veritas-build.cpp`, extend `kUsage` (`:45-57`). It already carries
the five `--metrics*` lines and **they must survive this edit** — dropping them
regresses the usage text for four shipped flags:

```cpp
constexpr std::string_view kUsage =
    "usage:\n"
    "  veritas-build --version\n"
    "  veritas-build analyze --project <directory> [--output <directory>]\n"
    "      [--wpa-engine souffle|cpp-emergency]\n"
    "      [--scale-profile baseline|scaled]\n"
    "      [--field-sensitive true|false] [--max-alias-pairs <n>]\n"
    "      [--metrics true|false] [--metrics-interval-ms <n>]\n"
    "      [--metrics-top-n <k>] [--metrics-series true|false]\n"
    "      [--metrics-path <file>]\n"
    "\n"
    "`analyze` is the only source-input command. No `--compile-db`,\n"
    "`--manifest`, `--bitcode`, `--llvm-module`, or `--svf-input` alternative\n"
    "is accepted; the project directory is the sole public source-input.\n"
    "\n"
    "`--scale-profile` defaults to `baseline`, which is the current\n"
    "implementation. `scaled` is reserved for the scaling milestones and is\n"
    "rejected until one lands; see\n"
    "docs/plans/veritas-scaling-milestone-roadmap.md.\n";
```

Add the field to `AnalyzeArguments` (`:59-70`), beside `wpa_engine`:

```cpp
  std::string scale_profile = "baseline";
```

Add the parse branch immediately after the `--wpa-engine` branch (`:185-192`),
copying its shape:

```cpp
    } else if (arg == "--scale-profile") {
      auto value = take_value(i, "--scale-profile");
      if (!value.ok()) return value.status();
      if (*value != "baseline" && *value != "scaled") {
        return veritas::Status::InvalidArgument(
            "--scale-profile must be baseline or scaled, got: " + *value);
      }
      parsed.scale_profile = *value;
```

In `Analyze`, after the config is built (`:611-619`, where `config.wpa_engine`
and `config.svf_field_sensitive` are assigned) and before `AnalyzeProject` is
invoked, add the rejection:

```cpp
  if (parsed->scale_profile == "scaled") {
    return veritas::Status::FailedPrecondition(
        "--scale-profile=scaled has no implemented changes yet, so this run "
        "would silently do nothing different from baseline. The scaled profile "
        "is specified in docs/plans/veritas-scaling-milestone-roadmap.md and "
        "becomes usable when its first structural milestone lands.");
  }
  config.scale_profile =
      parsed->scale_profile == "scaled" ? veritas::analysis::ScaleProfile::kScaled
                                        : veritas::analysis::ScaleProfile::kBaseline;
```

The `config.scale_profile` assignment is unreachable for `scaled` today. Keep it
anyway, so the field is wired to the flag rather than merely declared; the
milestone that implements `scaled` deletes the rejection above and needs no
other change. Say so in a comment directly above the assignment.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build --preset default --target VeritasBuildAnalyzeCliTest
./build/bin/VeritasBuildAnalyzeCliTest --gtest_filter='*ScaleProfile*:*Scaled*'
```

Expected: `[  PASSED  ] 3 tests.`

- [ ] **Step 5: Run the whole CLI suite to check nothing regressed**

```bash
./build/bin/VeritasBuildAnalyzeCliTest
```

Expected: all cases pass. If a pre-existing case asserted on the exact
`kUsage` text, update that assertion — the usage string changed deliberately.

- [ ] **Step 6: Commit**

```bash
git add src/tools/veritas-build.cpp \
        tests/integration/build/VeritasBuildAnalyzeCliTest.cpp
git commit -m "feat(build): add --scale-profile with scaled rejected until implemented"
```

---

### Task 3: A read-only `MetadataStore::Open` for inspection

**Files:**
- Modify: `include/veritas/summarydb/MetadataStore.h` (declare beside `Open` at `:92-94`)
- Modify: `src/summarydb/MetadataStore.cpp` (implement beside `Open` at `:117-138`)
- Modify: `tests/unit/summarydb/MetadataStoreTest.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `static StatusOr<MetadataStore> OpenReadOnly(const std::filesystem::path& db_path);`
  — opens an existing store for reading without creating, migrating, or writing
  to it, returning `NotFound` when no store exists at the path. Task 4 and Task 5
  consume it.

Why this is required rather than convenient: the existing `Open` applies the
schema, so pointing an inspection tool at a path would *create* a store there and
pointing it at a real store could add tables. An instrument that mutates the
artifact it measures is not an instrument.

- [ ] **Step 1: Write the failing tests**

Append to `tests/unit/summarydb/MetadataStoreTest.cpp`, inside the existing test
namespace:

```cpp
// An inspection tool must not create a store as a side effect of looking for
// one, or a typo in a path silently produces an empty store that compares equal
// to nothing and reports success.
TEST(MetadataStoreTest, OpenReadOnlyDoesNotCreateAMissingStore) {
  const auto path = TempDbPath();
  std::filesystem::remove(path);
  const auto store = MetadataStore::OpenReadOnly(path);
  EXPECT_FALSE(store.ok());
  EXPECT_EQ(store.status().code(), StatusCode::kNotFound);
  EXPECT_FALSE(std::filesystem::exists(path))
      << "OpenReadOnly created the file it was asked to read";
}

// Opening a real store for reading must leave every byte alone. Comparing a
// digest of the file before and after is the only check that catches a schema
// migration that happens to be idempotent in content but writes anyway.
TEST(MetadataStoreTest, OpenReadOnlyLeavesTheStoreByteIdentical) {
  const auto path = TempDbPath();
  {
    auto store = MetadataStore::Open(path);
    ASSERT_TRUE(store.ok()) << store.status().message();
    ASSERT_TRUE(store->Execute("CREATE TABLE probe (a TEXT)", {}).ok());
    ASSERT_TRUE(store->Execute("INSERT INTO probe (a) VALUES ('x')", {}).ok());
  }

  const auto before = FileDigest(path);
  {
    const auto ro = MetadataStore::OpenReadOnly(path);
    ASSERT_TRUE(ro.ok()) << ro.status().message();
    const auto rows = ro->Query("SELECT a FROM probe", {});
    ASSERT_TRUE(rows.ok()) << rows.status().message();
    ASSERT_EQ(rows->size(), 1u);
    EXPECT_EQ((*rows)[0][0], "x");
  }
  EXPECT_EQ(FileDigest(path), before) << "OpenReadOnly wrote to the store";
}
```

Add the `FileDigest` helper above the fixture in the same file:

```cpp
// SHA-256 of a file's bytes, for "did this change?" checks. Uses the same
// hashing the project already depends on so no new dependency appears.
std::string FileDigest(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
  return veritas::core::HexDigestOf(bytes);
}
```

If `veritas::core::HexDigestOf` does not exist, use whatever
`include/veritas/core/Hash.h` exposes for a one-shot SHA-256 over a byte span —
read that header first and use its actual name rather than inventing one.

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cmake --build --preset default --target MetadataStoreTest
./build/bin/MetadataStoreTest --gtest_filter='*OpenReadOnly*'
```

Expected: build failure — `OpenReadOnly` is not a member of `MetadataStore`.

- [ ] **Step 3: Implement the read-only open**

In `include/veritas/summarydb/MetadataStore.h`, after the `Open` declaration at
`:94`:

```cpp
  // Open an existing metadata database for reading only. Unlike `Open`, this
  // never creates the file, never applies the schema, and never writes to it:
  // it sets SQLITE_OPEN_READONLY so a write attempt fails rather than silently
  // mutating the store. Returns NotFound when no store exists at db_path.
  //
  // An inspection tool must use this. `Open` would create an empty store at a
  // mistyped path, and an empty store compares equal to nothing while looking
  // like a successful comparison.
  static StatusOr<MetadataStore> OpenReadOnly(
      const std::filesystem::path& db_path);
```

In `src/summarydb/MetadataStore.cpp`, implement it beside `Open` (`:118`) by
copying `Open`'s body and changing exactly three things:

1. Check existence first —
   `if (!std::filesystem::exists(db_path)) return Status::NotFound(...)` with a
   message naming the path.
2. Pass `SQLITE_OPEN_READONLY` instead of the read-write flags.
3. Delete the `ApplySchema(store)` call. Leaving it in is the bug this task
   exists to prevent; add a comment where it used to be saying the read-only path
   deliberately does not migrate.

Do not set `PRAGMA foreign_keys` on this connection either — the existing `Open`
sets it, and matching a read-only connection's pragmas to it is unnecessary; but
if you find that omitting it changes any read result, keep the pragma and say so
in a comment.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build --preset default --target MetadataStoreTest
./build/bin/MetadataStoreTest
```

Expected: all cases pass, including the pre-existing ones.

- [ ] **Step 5: Commit**

```bash
git add include/veritas/summarydb/MetadataStore.h \
        src/summarydb/MetadataStore.cpp \
        tests/unit/summarydb/MetadataStoreTest.cpp
git commit -m "feat(summarydb): add a MetadataStore read-only open for inspection"
```

---

### Task 4: The canonical store dump

**Files:**
- Create: `include/veritas/summarydb/StoreEquivalence.h`
- Create: `src/summarydb/StoreEquivalence.cpp`
- Create: `tests/unit/summarydb/StoreEquivalenceTest.cpp`
- Modify: `tests/unit/summarydb/CMakeLists.txt`

**Interfaces:**
- Consumes: `MetadataStore::OpenReadOnly` from Task 3.
- Produces:
  - `struct veritas::summarydb::TableProjection { std::string order_by; std::vector<std::string> excluded_columns; };`
  - `TableProjection ResolveTableProjection(std::string_view table);`
  - `struct TableDump { std::string table; std::string order_by; std::vector<std::string> excluded_columns; std::size_t row_count; std::string sha256; };`
  - `struct StoreDump { std::vector<TableDump> tables; std::string sha256; };`
  - `StatusOr<StoreDump> DumpStore(const std::filesystem::path& metadata_db_path);`

  Task 5 and Task 6 consume all five.

- [ ] **Step 1: Write the failing tests**

Create `tests/unit/summarydb/StoreEquivalenceTest.cpp`:

```cpp
// Copyright 2026 VERITAS Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "veritas/summarydb/StoreEquivalence.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "veritas/summarydb/MetadataStore.h"

namespace veritas::summarydb {
namespace {

namespace fs = std::filesystem;

class StoreEquivalenceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info =
        ::testing::UnitTest::GetInstance()->current_test_info();
    db_path_ = fs::temp_directory_path() /
               ("veritas_store_equiv_" + std::string(info->name()) + ".db");
    fs::remove(db_path_);
  }

  void TearDown() override { fs::remove(db_path_); }

  // A minimal two-table store, small enough to reason about by hand.
  void MakeStore(const std::string& second_row) {
    auto store = MetadataStore::Open(db_path_);
    ASSERT_TRUE(store.ok()) << store.status().message();
    ASSERT_TRUE(store->Execute("CREATE TABLE alpha (id TEXT, payload TEXT)", {}).ok());
    ASSERT_TRUE(store->Execute("CREATE TABLE beta (id TEXT, note TEXT)", {}).ok());
    ASSERT_TRUE(store->Execute("INSERT INTO alpha (id, payload) VALUES ('a1','p1')", {}).ok());
    ASSERT_TRUE(store->Execute("INSERT INTO alpha (id, payload) VALUES ('a2','p2')", {}).ok());
    ASSERT_TRUE(store->Execute("INSERT INTO beta (id, note) VALUES ('b1','" +
                               second_row + "')", {}).ok());
  }

  fs::path db_path_;
};

// The projection for a table with no determined entry is the default: order by
// the physical row order and exclude nothing. `rowid` is the one key every
// table in this store has, and the writer's own row order is what round 3's
// instrument uses for the run-scoped tables.
TEST_F(StoreEquivalenceTest, UndeterminedTablesUseTheDefaultProjection) {
  const auto projection = ResolveTableProjection("some_future_table");
  EXPECT_EQ(projection.order_by, "rowid");
  EXPECT_TRUE(projection.excluded_columns.empty());
}

// The four published tables carry the projection round 3 determined. These
// entries are the reason the instrument is trustworthy: they are recorded
// rather than guessed, and Task 6 re-derives them.
TEST_F(StoreEquivalenceTest, PublishedTablesCarryTheDeterminedProjection) {
  const auto facts = ResolveTableProjection("analysis_facts");
  EXPECT_EQ(facts.order_by, "fact_id");
  EXPECT_TRUE(facts.excluded_columns.empty());

  const auto bindings = ResolveTableProjection("run_fact_bindings");
  EXPECT_EQ(bindings.order_by, "rowid");
  EXPECT_EQ(bindings.excluded_columns,
            (std::vector<std::string>{"run_id", "analyzer_run_id", "binding_id"}));

  const auto nodes = ResolveTableProjection("provenance_nodes");
  EXPECT_EQ(nodes.order_by, "rowid");
  EXPECT_EQ(nodes.excluded_columns, (std::vector<std::string>{"run_id"}));

  const auto edges = ResolveTableProjection("provenance_edges");
  EXPECT_EQ(edges.order_by, "rowid");
  EXPECT_EQ(edges.excluded_columns, (std::vector<std::string>{"run_id"}));
}

// A dump is a function of content, not of the file's incidental state.
TEST_F(StoreEquivalenceTest, DumpingTheSameStoreTwiceIsIdentical) {
  MakeStore("n1");
  const auto first = DumpStore(db_path_);
  const auto second = DumpStore(db_path_);
  ASSERT_TRUE(first.ok()) << first.status().message();
  ASSERT_TRUE(second.ok()) << second.status().message();
  EXPECT_EQ(first->sha256, second->sha256);
  ASSERT_EQ(first->tables.size(), second->tables.size());
  EXPECT_EQ(first->tables.size(), 3u)
      << "expected alpha, beta, and sqlite_sequence";
}

// Every table is present exactly once and the order is stable, so a digest is
// comparable across machines.
TEST_F(StoreEquivalenceTest, TablesAreSortedByName) {
  MakeStore("n1");
  const auto dump = DumpStore(db_path_);
  ASSERT_TRUE(dump.ok()) << dump.status().message();
  for (std::size_t i = 1; i < dump->tables.size(); ++i) {
    EXPECT_LT(dump->tables[i - 1].table, dump->tables[i].table);
  }
}

// The digest must be sensitive to content, or every later comparison is
// vacuous. This is the instrument's own negative control.
TEST_F(StoreEquivalenceTest, ADifferentCellChangesTheDigest) {
  MakeStore("n1");
  const auto first = DumpStore(db_path_);
  ASSERT_TRUE(first.ok()) << first.status().message();

  ASSERT_TRUE(fs::remove(db_path_));
  MakeStore("n2");
  const auto second = DumpStore(db_path_);
  ASSERT_TRUE(second.ok()) << second.status().message();

  EXPECT_NE(first->sha256, second->sha256);
}

// Row counts are reported so a caller can tell "same content, fewer rows" from
// "different content".
TEST_F(StoreEquivalenceTest, RowCountsAreReported) {
  MakeStore("n1");
  const auto dump = DumpStore(db_path_);
  ASSERT_TRUE(dump.ok()) << dump.status().message();
  std::size_t alpha_rows = 0;
  for (const auto& table : dump->tables) {
    if (table.table == "alpha") alpha_rows = table.row_count;
  }
  EXPECT_EQ(alpha_rows, 2u);
}

}  // namespace
}  // namespace veritas::summarydb
```

- [ ] **Step 2: Register the test target**

Append to `tests/unit/summarydb/CMakeLists.txt`:

```cmake
add_executable(StoreEquivalenceTest StoreEquivalenceTest.cpp)
target_link_libraries(StoreEquivalenceTest PRIVATE
  veritas_summarydb
  GTest::gtest_main
)
veritas_add_warnings(StoreEquivalenceTest)
gtest_discover_tests(StoreEquivalenceTest DISCOVERY_TIMEOUT 60)
```

- [ ] **Step 3: Run the tests to verify they fail**

```bash
cmake --build --preset default --target StoreEquivalenceTest
```

Expected: build failure — `StoreEquivalence.h` does not exist.

- [ ] **Step 4: Declare the header**

Create `include/veritas/summarydb/StoreEquivalence.h`:

```cpp
// Copyright 2026 VERITAS Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef VERITAS_SUMMARYDB_STORE_EQUIVALENCE_H_
#define VERITAS_SUMMARYDB_STORE_EQUIVALENCE_H_

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "veritas/core/Status.h"

namespace veritas::summarydb {

// How one table is projected into a canonical, comparable form.
//
// `order_by` is the column or key the rows are sorted by before digesting.
// Sorting is what makes the digest independent of insertion order and of the
// physical fan-out the writer happened to produce.
//
// `excluded_columns` are omitted from the digest. They hold run-scoped
// identities that move between runs of unchanged input by construction, so
// including them would make every comparison fail.
struct TableProjection {
  std::string order_by;
  std::vector<std::string> excluded_columns;
};

// The determined projection for `table`. Tables with no recorded entry get
// `{"rowid", {}}`. The entries for the published tables are the projection
// documented in
// docs/specs/veritas-build-analyze-round3-performance-design-spec.md section
// 9.1, which round 3 determined by measurement rather than assumed.
//
// A column named in an entry that does not exist in the table is an error at
// dump time, not silently ignored: a stale exclusion would otherwise widen the
// comparison without anyone noticing.
TableProjection ResolveTableProjection(std::string_view table);

// One table's canonical dump.
struct TableDump {
  std::string table;
  std::string order_by;
  std::vector<std::string> excluded_columns;
  std::size_t row_count = 0;
  // SHA-256 over the canonical row stream: for each row in `order_by` order,
  // the retained column values joined by '\t', each row terminated by '\n'.
  // Column and row boundaries are therefore unambiguous only because no value
  // in this store contains a literal tab or newline; Task 5 asserts that
  // assumption on the tables it compares rather than trusting it.
  std::string sha256;
};

// A canonical dump of every table in a store.
struct StoreDump {
  std::vector<TableDump> tables;  // sorted by table name
  // SHA-256 over the concatenation of each table's name, row count, and digest,
  // in `tables` order.
  std::string sha256;
};

// Dump every user table of the store at `metadata_db_path` in canonical form.
//
// The store is opened read-only; nothing is created, migrated, or written.
// SQLite's own internal tables (names beginning `sqlite_`) are included, because
// `sqlite_sequence` records AUTOINCREMENT high-water marks that differ between
// runs and a caller should see that rather than have it hidden.
//
// Returns NotFound when no store exists at the path, and FailedPrecondition when
// a table's determined projection names a column the table does not have.
StatusOr<StoreDump> DumpStore(const std::filesystem::path& metadata_db_path);

}  // namespace veritas::summarydb

#endif  // VERITAS_SUMMARYDB_STORE_EQUIVALENCE_H_
```

- [ ] **Step 5: Implement the dump**

Create `src/summarydb/StoreEquivalence.cpp`. The shape, which you should follow
exactly because each piece is load-bearing:

```cpp
// Copyright 2026 VERITAS Contributors
// ... the standard Apache-2.0 header ...

#include "veritas/summarydb/StoreEquivalence.h"

#include <algorithm>
#include <map>
#include <sstream>

#include "veritas/core/Hash.h"
#include "veritas/summarydb/MetadataStore.h"

namespace veritas::summarydb {
namespace {

// The determined projection. Entries are recorded from
// docs/specs/veritas-build-analyze-round3-performance-design-spec.md section
// 9.1 and re-derived by the M13 acceptance task. An empty `order_by` never
// appears: missing entries fall through to the default below.
struct RecordedProjection {
  std::string_view table;
  std::string_view order_by;
  std::vector<std::string_view> excluded_columns;
};

constexpr RecordedProjection kRecorded[] = {
    {"analysis_facts", "fact_id", {}},
    {"run_fact_bindings", "rowid", {"run_id", "analyzer_run_id", "binding_id"}},
    {"provenance_nodes", "rowid", {"run_id"}},
    {"provenance_edges", "rowid", {"run_id"}},
};

// Every user table, in stable name order, plus SQLite's own bookkeeping.
StatusOr<std::vector<std::string>> ListTables(MetadataStore* store) {
  auto rows = store->Query(
      "SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name", {});
  if (!rows.ok()) return rows.status();
  std::vector<std::string> tables;
  tables.reserve(rows->size());
  for (const auto& row : *rows) {
    if (!row.empty()) tables.push_back(row[0]);
  }
  return tables;
}

// The column names of `table`, in declaration order.
StatusOr<std::vector<std::string>> ListColumns(MetadataStore* store,
                                              const std::string& table) {
  // PRAGMA table_info returns (cid, name, type, notnull, dflt_value, pk);
  // column 1 is the name.
  auto rows = store->Query("PRAGMA table_info(\"" + table + "\")", {});
  if (!rows.ok()) return rows.status();
  std::vector<std::string> columns;
  columns.reserve(rows->size());
  for (const auto& row : *rows) {
    if (row.size() < 2) {
      return Status::Internal("PRAGMA table_info returned a short row for " +
                              table);
    }
    columns.push_back(row[1]);
  }
  return columns;
}

}  // namespace

TableProjection ResolveTableProjection(std::string_view table) {
  for (const auto& recorded : kRecorded) {
    if (recorded.table == table) {
      TableProjection projection;
      projection.order_by = std::string(recorded.order_by);
      projection.excluded_columns.assign(recorded.excluded_columns.begin(),
                                         recorded.excluded_columns.end());
      return projection;
    }
  }
  return TableProjection{.order_by = "rowid", .excluded_columns = {}};
}

StatusOr<StoreDump> DumpStore(const std::filesystem::path& metadata_db_path) {
  auto store = MetadataStore::OpenReadOnly(metadata_db_path);
  if (!store.ok()) return store.status();

  auto tables = ListTables(&*store);
  if (!tables.ok()) return tables.status();

  StoreDump dump;
  std::string combined;
  for (const auto& table : *tables) {
    auto columns = ListColumns(&*store, table);
    if (!columns.ok()) return columns.status();
    const auto projection = ResolveTableProjection(table);

    // Reject a stale exclusion rather than letting it silently widen the
    // comparison. This is the check that would have caught a hand-written
    // exclusion list drifting away from the schema.
    for (const auto& excluded : projection.excluded_columns) {
      if (std::find(columns->begin(), columns->end(), excluded) == columns->end()) {
        return Status::FailedPrecondition(
            "table " + table + " has no column " + excluded +
            ", but its recorded projection excludes it");
      }
    }

    std::string select = "SELECT ";
    bool first = true;
    for (const auto& column : *columns) {
      if (std::find(projection.excluded_columns.begin(),
                    projection.excluded_columns.end(),
                    column) != projection.excluded_columns.end()) {
        continue;
      }
      if (!first) select += ", ";
      select += "\"" + column + "\"";
      first = false;
    }
    if (first) {
      return Status::FailedPrecondition("every column of " + table +
                                       " is excluded from its projection");
    }
    select += " FROM \"" + table + "\" ORDER BY " + projection.order_by;

    auto rows = store->Query(select, {});
    if (!rows.ok()) return rows.status();

    std::string stream;
    for (const auto& row : *rows) {
      for (std::size_t i = 0; i < row.size(); ++i) {
        if (i != 0) stream.push_back('\t');
        stream.append(row[i]);
      }
      stream.push_back('\n');
    }

    TableDump table_dump;
    table_dump.table = table;
    table_dump.order_by = projection.order_by;
    table_dump.excluded_columns = projection.excluded_columns;
    table_dump.row_count = rows->size();
    table_dump.sha256 = core::Sha256Hex(stream);  // use the real name from Hash.h
    combined += table + ":" + std::to_string(table_dump.row_count) + ":" +
                table_dump.sha256 + "\n";
    dump.tables.push_back(std::move(table_dump));
  }

  dump.sha256 = core::Sha256Hex(combined);  // use the real name from Hash.h
  return dump;
}

}  // namespace veritas::summarydb
```

Two things to check rather than assume: the one-shot SHA-256 helper's real name
in `include/veritas/core/Hash.h`, and that `MetadataStore::Query` collapses NULL
to the empty string (`src/summarydb/MetadataStore.cpp:609` documents this).
The second means this instrument **cannot distinguish NULL from `''`**. Add that
as a comment on `StoreDump` in the header, because a future caller will otherwise
assume it can.

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build --preset default --target StoreEquivalenceTest
./build/bin/StoreEquivalenceTest
```

Expected: `[  PASSED  ] 6 tests.`

- [ ] **Step 7: Commit**

```bash
git add include/veritas/summarydb/StoreEquivalence.h \
        src/summarydb/StoreEquivalence.cpp \
        tests/unit/summarydb/StoreEquivalenceTest.cpp \
        tests/unit/summarydb/CMakeLists.txt
git commit -m "feat(summarydb): add the canonical store dump instrument"
```

---

### Task 5: Store comparison and its perturbation controls

**Files:**
- Modify: `include/veritas/summarydb/StoreEquivalence.h` (add the comparison vocabulary and `CompareDumps`)
- Modify: `src/summarydb/StoreEquivalence.cpp`
- Modify: `tests/unit/summarydb/StoreEquivalenceTest.cpp`

**Interfaces:**
- Consumes: `StoreDump`, `DumpStore` from Task 4.
- Produces:
  - `struct TableComparison { std::string table; std::size_t left_rows; std::size_t right_rows; bool equal; };`
  - `struct StoreComparison { bool equal; std::vector<TableComparison> differing; std::vector<std::string> left_only; std::vector<std::string> right_only; };`
  - `StoreComparison CompareDumps(const StoreDump& left, const StoreDump& right);`
  - `StatusOr<StoreComparison> CompareStoreFiles(const std::filesystem::path& left, const std::filesystem::path& right);`

  Task 7 and the M14 harness consume these. Later milestones consume them as the
  `baseline`-equivalence gate.

- [ ] **Step 1: Write the failing tests**

Append to `tests/unit/summarydb/StoreEquivalenceTest.cpp`. Each case is a
perturbation the instrument must detect; an instrument that cannot fail its own
controls proves nothing.

```cpp
TEST_F(StoreEquivalenceTest, IdenticalStoresCompareEqual) {
  MakeStore("n1");
  const auto left = DumpStore(db_path_);
  ASSERT_TRUE(left.ok()) << left.status().message();
  const auto comparison = CompareDumps(*left, *left);
  EXPECT_TRUE(comparison.equal);
  EXPECT_TRUE(comparison.differing.empty());
  EXPECT_TRUE(comparison.left_only.empty());
  EXPECT_TRUE(comparison.right_only.empty());
}

// A single changed cell in one row of one table, with the row count unchanged.
// This is the perturbation that a row-count-only check would miss.
TEST_F(StoreEquivalenceTest, ASingleChangedCellIsDetected) {
  MakeStore("n1");
  const auto left = DumpStore(db_path_);
  ASSERT_TRUE(left.ok()) << left.status().message();

  ASSERT_TRUE(fs::remove(db_path_));
  MakeStore("n2");
  const auto right = DumpStore(db_path_);
  ASSERT_TRUE(right.ok()) << right.status().message();

  const auto comparison = CompareDumps(*left, *right);
  EXPECT_FALSE(comparison.equal);
  ASSERT_EQ(comparison.differing.size(), 1u);
  EXPECT_EQ(comparison.differing[0].table, "beta");
  EXPECT_EQ(comparison.differing[0].left_rows, 1u);
  EXPECT_EQ(comparison.differing[0].right_rows, 1u);
}

// A dropped row in one table.
TEST_F(StoreEquivalenceTest, ADroppedRowIsDetected) {
  MakeStore("n1");
  const auto left = DumpStore(db_path_);
  ASSERT_TRUE(left.ok()) << left.status().message();

  {
    auto store = MetadataStore::Open(db_path_);
    ASSERT_TRUE(store.ok());
    ASSERT_TRUE(store->Execute("DELETE FROM alpha WHERE id = 'a2'", {}).ok());
  }
  const auto right = DumpStore(db_path_);
  ASSERT_TRUE(right.ok()) << right.status().message();

  const auto comparison = CompareDumps(*left, *right);
  EXPECT_FALSE(comparison.equal);
  ASSERT_EQ(comparison.differing.size(), 1u);
  EXPECT_EQ(comparison.differing[0].table, "alpha");
  EXPECT_EQ(comparison.differing[0].left_rows, 2u);
  EXPECT_EQ(comparison.differing[0].right_rows, 1u);
}

// A table present on one side only. Reporting this separately from "differing"
// is what lets a caller distinguish a schema change from a content change.
TEST_F(StoreEquivalenceTest, ATablePresentOnOneSideIsReportedSeparately) {
  MakeStore("n1");
  const auto left = DumpStore(db_path_);
  ASSERT_TRUE(left.ok()) << left.status().message();

  {
    auto store = MetadataStore::Open(db_path_);
    ASSERT_TRUE(store.ok());
    ASSERT_TRUE(store->Execute("CREATE TABLE gamma (x TEXT)", {}).ok());
  }
  const auto right = DumpStore(db_path_);
  ASSERT_TRUE(right.ok()) << right.status().message();

  const auto comparison = CompareDumps(*left, *right);
  EXPECT_FALSE(comparison.equal);
  ASSERT_EQ(comparison.right_only.size(), 1u);
  EXPECT_EQ(comparison.right_only[0], "gamma");
}

// Row order within a table must not affect the digest: the dump sorts, and the
// writer's physical order is not semantic.
TEST_F(StoreEquivalenceTest, RowOrderDoesNotChangeTheDigest) {
  MakeStore("n1");
  const auto left = DumpStore(db_path_);
  ASSERT_TRUE(left.ok()) << left.status().message();

  {
    auto store = MetadataStore::Open(db_path_);
    ASSERT_TRUE(store.ok());
    ASSERT_TRUE(store->Execute("DELETE FROM alpha", {}).ok());
    ASSERT_TRUE(store->Execute("INSERT INTO alpha VALUES ('a2','p2')", {}).ok());
    ASSERT_TRUE(store->Execute("INSERT INTO alpha VALUES ('a1','p1')", {}).ok());
  }
  const auto right = DumpStore(db_path_);
  ASSERT_TRUE(right.ok()) << right.status().message();

  const auto comparison = CompareDumps(*left, *right);
  EXPECT_TRUE(comparison.equal) << "reinserting the same rows in the opposite "
                                   "order changed the digest";
}

// Comparing across files is the form every later milestone uses.
TEST_F(StoreEquivalenceTest, CompareStoreFilesAgreesWithCompareDumps) {
  MakeStore("n1");
  const auto path = db_path_;
  const auto right_path = fs::temp_directory_path() / "veritas_store_equiv_right.db";
  fs::remove(right_path);
  ASSERT_TRUE(fs::copy_file(path, right_path));

  const auto comparison = CompareStoreFiles(path, right_path);
  ASSERT_TRUE(comparison.ok()) << comparison.status().message();
  EXPECT_TRUE(comparison->equal);

  fs::remove(right_path);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cmake --build --preset default --target StoreEquivalenceTest
```

Expected: build failure — `CompareDumps` and `CompareStoreFiles` do not exist.

- [ ] **Step 3: Declare and implement the comparison**

Add to `include/veritas/summarydb/StoreEquivalence.h`, before `DumpStore`:

```cpp
// One table's comparison outcome. `equal` is false when the digests differ or
// the row counts differ.
struct TableComparison {
  std::string table;
  std::size_t left_rows = 0;
  std::size_t right_rows = 0;
  bool equal = false;
};

// The outcome of comparing two dumps.
//
// `differing` holds only tables present on both sides whose digest or row count
// disagrees. `left_only` and `right_only` hold tables present on exactly one
// side, so a caller can tell a schema difference from a content difference
// without inspecting the dumps.
struct StoreComparison {
  bool equal = false;
  std::vector<TableComparison> differing;
  std::vector<std::string> left_only;
  std::vector<std::string> right_only;
};

// Compare two dumps by table name. Neither argument is modified.
StoreComparison CompareDumps(const StoreDump& left, const StoreDump& right);

// Dump and compare two stores. Returns the first dump's failure if either store
// cannot be read.
StatusOr<StoreComparison> CompareStoreFiles(const std::filesystem::path& left,
                                            const std::filesystem::path& right);
```

Implement in `src/summarydb/StoreEquivalence.cpp`:

```cpp
StoreComparison CompareDumps(const StoreDump& left, const StoreDump& right) {
  std::map<std::string, const TableDump*> left_tables;
  std::map<std::string, const TableDump*> right_tables;
  for (const auto& table : left.tables) left_tables[table.table] = &table;
  for (const auto& table : right.tables) right_tables[table.table] = &table;

  StoreComparison comparison;
  for (const auto& [name, table] : left_tables) {
    const auto found = right_tables.find(name);
    if (found == right_tables.end()) {
      comparison.left_only.push_back(name);
      continue;
    }
    if (table->sha256 != found->second->sha256 ||
        table->row_count != found->second->row_count) {
      comparison.differing.push_back(TableComparison{
          .table = name,
          .left_rows = table->row_count,
          .right_rows = found->second->row_count,
          .equal = false});
    }
  }
  for (const auto& [name, table] : right_tables) {
    (void)table;
    if (left_tables.find(name) == left_tables.end()) {
      comparison.right_only.push_back(name);
    }
  }

  comparison.equal = comparison.differing.empty() &&
                     comparison.left_only.empty() &&
                     comparison.right_only.empty();
  return comparison;
}

StatusOr<StoreComparison> CompareStoreFiles(const std::filesystem::path& left,
                                            const std::filesystem::path& right) {
  auto left_dump = DumpStore(left);
  if (!left_dump.ok()) return left_dump.status();
  auto right_dump = DumpStore(right);
  if (!right_dump.ok()) return right_dump.status();
  return CompareDumps(*left_dump, *right_dump);
}
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build --preset default --target StoreEquivalenceTest
./build/bin/StoreEquivalenceTest
```

Expected: `[  PASSED  ] 12 tests.`

- [ ] **Step 5: Commit**

```bash
git add include/veritas/summarydb/StoreEquivalence.h \
        src/summarydb/StoreEquivalence.cpp \
        tests/unit/summarydb/StoreEquivalenceTest.cpp
git commit -m "feat(summarydb): compare two stores with perturbation controls"
```

---

### Task 6: The `veritas-store-diff` CLI

**Files:**
- Create: `src/tools/veritas-store-diff.cpp`
- Modify: `src/tools/CMakeLists.txt` (extend the `foreach` at `:21` and add a link line)

**Interfaces:**
- Consumes: `CompareStoreFiles`, `StoreComparison`, `TableComparison` from Task 5.
- Produces: `veritas-store-diff <left-store-root> <right-store-root>`, exit 0
  when equal, 1 when different, 2 on error. Task 7 and the M14 harness consume
  the exit code.

The tool takes store **roots** (the directories passed to `--output`), not
`metadata.db` paths, because that is how a user has them.

- [ ] **Step 1: Write the tool**

Create `src/tools/veritas-store-diff.cpp`:

```cpp
// Copyright 2026 VERITAS Contributors
// ... the standard Apache-2.0 header ...

#include <cstring>
#include <filesystem>
#include <iostream>
#include <string_view>

#include "veritas/core/Status.h"
#include "veritas/summarydb/StoreEquivalence.h"

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kUsage =
    "usage:\n"
    "  veritas-store-diff <left-store-root> <right-store-root>\n"
    "\n"
    "Compares the metadata.db of two store roots table by table, using the\n"
    "determined projection in\n"
    "docs/specs/veritas-build-analyze-round3-performance-design-spec.md 9.1:\n"
    "run-scoped columns are excluded, rows are ordered deterministically, and\n"
    "each table is digested. Exits 0 when the stores are equivalent, 1 when\n"
    "they differ, and 2 on error.\n";

int ReportError(const veritas::Status& status) {
  std::cerr << "veritas-store-diff: " << status.message() << '\n';
  return 2;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 3) {
    std::cerr << kUsage;
    return 2;
  }

  const fs::path left = fs::path(argv[1]) / "metadata.db";
  const fs::path right = fs::path(argv[2]) / "metadata.db";

  auto comparison = veritas::summarydb::CompareStoreFiles(left, right);
  if (!comparison.ok()) return ReportError(comparison.status());

  if (comparison->equal) {
    std::cout << "stores are equivalent\n";
    return 0;
  }

  std::cout << "stores differ\n";
  for (const auto& table : comparison->differing) {
    std::cout << "  differs: " << table.table << " (left " << table.left_rows
              << " rows, right " << table.right_rows << " rows)\n";
  }
  for (const auto& table : comparison->left_only) {
    std::cout << "  left only: " << table << '\n';
  }
  for (const auto& table : comparison->right_only) {
    std::cout << "  right only: " << table << '\n';
  }
  return 1;
}
```

- [ ] **Step 2: Register the tool**

In `src/tools/CMakeLists.txt`, add the name to the `foreach` list at `:21`:

```cmake
foreach(_veritas_cli veritas-build veritas-query veritas-diff veritas-explain
        veritas-store-diff)
```

and after the `foreach`, alongside the other link lines:

```cmake
# veritas-store-diff compares two store roots table by table, using the
# determined projection M13 recorded for baseline-equivalence checks.
target_link_libraries(veritas-store-diff PRIVATE veritas_summarydb)
```

- [ ] **Step 3: Build and check the exit codes against real stores**

```bash
cmake --build --preset default --target veritas-store-diff

# Two independent runs of the same fixture must be equivalent.
./build/bin/veritas-build analyze --project tests/fixtures/projects/store_load \
    --output /tmp/m13-left
./build/bin/veritas-build analyze --project tests/fixtures/projects/store_load \
    --output /tmp/m13-right
./build/bin/veritas-store-diff /tmp/m13-left /tmp/m13-right
echo "exit=$?"
```

Expected: **`exit=1` with a non-empty `differs:` list**, because each run
allocates a fresh run id and `sqlite_sequence` high-water marks, so run-scoped
columns are excluded by the projection but *other* per-run values are not yet.
This is the finding Task 7 exists to resolve: the projection is a hypothesis until
two real runs determine it. Do not "fix" this by widening exclusions blind —
Task 7 measures which columns actually move.

If the two runs happen to compare equal, do not delete this step: record in the
commit message that the projection was already sufficient at this revision, and
Task 7 still runs to confirm it on the LevelDB fixture.

- [ ] **Step 4: Check the error paths**

```bash
./build/bin/veritas-store-diff /tmp/m13-left /tmp/does-not-exist
echo "exit=$?"
./build/bin/veritas-store-diff
echo "exit=$?"
```

Expected: `exit=2` both times, with a message naming the missing store and the
usage string respectively.

- [ ] **Step 5: Commit**

```bash
git add src/tools/veritas-store-diff.cpp src/tools/CMakeLists.txt
git commit -m "feat(tools): add veritas-store-diff"
```

---

### Task 7: Determine the projection by measurement, and accept `baseline`

**Files:**
- Modify: `src/summarydb/StoreEquivalence.cpp` (the `kRecorded` table)
- Modify: `tests/unit/summarydb/StoreEquivalenceTest.cpp` (the projection assertions)
- Create: `docs/specs/milestones/m13-scale-profile-acceptance-record.md`

**Interfaces:**
- Consumes: everything above.
- Produces: the measured projection and the acceptance record M14's harness and
  later milestones cite. No code interface.

This is the task that makes the instrument trustworthy. Round 3 recorded its
projection in prose and the reasoning was right, but nothing executed it. Here it
is derived from two real runs and then asserted.

- [ ] **Step 1: Produce two real runs of the same input**

```bash
rm -rf /tmp/m13-a /tmp/m13-b
./build/bin/veritas-build analyze --project tests/fixtures/projects/semantic_zoo \
    --output /tmp/m13-a/store
./build/bin/veritas-build analyze --project tests/fixtures/projects/semantic_zoo \
    --output /tmp/m13-b/store
```

Use `semantic_zoo` rather than `store_load` because it exercises all four WPA
domains and therefore every published table.

- [ ] **Step 2: Find which columns actually move**

For each table in the store, compare the two databases column by column. The
tables come from the store itself, so nothing is assumed:

```bash
for t in $(sqlite3 /tmp/m13-a/store/metadata.db \
             "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name"); do
  cols=$(sqlite3 /tmp/m13-a/store/metadata.db "PRAGMA table_info($t)" | cut -d'|' -f2)
  for c in $cols; do
    a=$(sqlite3 /tmp/m13-a/store/metadata.db "SELECT $c FROM $t ORDER BY rowid" | shasum -a 256)
    b=$(sqlite3 /tmp/m13-b/store/metadata.db "SELECT $c FROM $t ORDER BY rowid" | shasum -a 256)
    [ "$a" = "$b" ] || echo "MOVES: $t.$c"
  done
done | sort
```

Record the output verbatim in the acceptance record. Every column it reports must
appear as an exclusion for that table, and every column it does **not** report
must not.

- [ ] **Step 3: Update the recorded projection**

For each table with a moving column, add or extend its entry in `kRecorded` in
`src/summarydb/StoreEquivalence.cpp`. Keep the published tables' entries as
recorded in round 3 §9.1 unless this measurement contradicts them; where it does,
the measurement wins and the acceptance record must say so explicitly.

Then update the projection assertions in
`tests/unit/summarydb/StoreEquivalenceTest.cpp` so they match the recorded table
exactly — the point of the assertion is to make a silent edit to `kRecorded`
fail a test.

- [ ] **Step 4: Re-run the comparison and require equivalence**

```bash
cmake --build --preset default
./build/bin/veritas-store-diff /tmp/m13-a/store /tmp/m13-b/store
echo "exit=$?"
```

Expected: `exit=0` with `stores are equivalent`.

If it is not 0, read the `differs:` list and go back to Step 2 for the named
tables. Do not add an exclusion you did not measure.

- [ ] **Step 5: Add the end-to-end regression test**

So this cannot silently regress, append to
`tests/unit/summarydb/StoreEquivalenceTest.cpp`:

```cpp
// Two independent analyses of the same fixture must publish equivalent
// content. This is the end-to-end statement the whole milestone exists to make,
// expressed as a test rather than a shell transcript.
TEST(StoreEquivalenceEndToEndTest, TwoRunsOfOneFixtureAreEquivalent) {
  const auto project = testing::FixtureProject("semantic_zoo");
  const auto first_root = fs::temp_directory_path() / "veritas_m13_e2e_first";
  const auto second_root = fs::temp_directory_path() / "veritas_m13_e2e_second";
  fs::remove_all(first_root);
  fs::remove_all(second_root);

  ProjectAnalyzer analyzer;
  const auto first =
      analyzer.AnalyzeProject({.project_root = project, .output_root = first_root},
                              AnalysisConfig::Default());
  ASSERT_TRUE(first.ok()) << first.status().message();
  // The second run reuses the materialized project but writes a fresh store.
  const auto second =
      analyzer.AnalyzeProject({.project_root = project, .output_root = second_root},
                              AnalysisConfig::Default());
  ASSERT_TRUE(second.ok()) << second.status().message();

  const auto comparison =
      CompareStoreFiles(first_root / "metadata.db", second_root / "metadata.db");
  ASSERT_TRUE(comparison.ok()) << comparison.status().message();
  EXPECT_TRUE(comparison->equal) << "differing tables: "
                                 << comparison->differing.size();

  fs::remove_all(first_root);
  fs::remove_all(second_root);
}
```

Register it in `tests/unit/summarydb/CMakeLists.txt` as a target that links
`veritas_analysis` as well as `veritas_summarydb`, and needs a real timeout —
so use the explicit form, not `gtest_discover_tests`:

```cmake
add_executable(StoreEquivalenceEndToEndTest StoreEquivalenceEndToEndTest.cpp)
target_link_libraries(StoreEquivalenceEndToEndTest PRIVATE
  veritas_analysis
  veritas_summarydb
  veritas_test_support
  GTest::gtest_main
)
veritas_add_warnings(StoreEquivalenceEndToEndTest)
add_test(NAME StoreEquivalenceEndToEndTest
  COMMAND StoreEquivalenceEndToEndTest)
set_tests_properties(StoreEquivalenceEndToEndTest PROPERTIES
  TIMEOUT 300
  LABELS "integration;analysis;summarydb"
)
```

Put the end-to-end case in its own file
`tests/unit/summarydb/StoreEquivalenceEndToEndTest.cpp` with its own licence
header, since it links a different set of libraries from the unit file. Do not
leave it inside the unit test file while registering it separately.

- [ ] **Step 6: Write the acceptance record**

Create `docs/specs/milestones/m13-scale-profile-acceptance-record.md`
containing, at minimum:

- the Step 2 command and its verbatim output;
- the final `kRecorded` projection, as the table it is;
- the `veritas-store-diff` exit code and output for the two `semantic_zoo` runs;
- the equivalent for the LevelDB checkout at
  `/Users/skg7on/Workspace/Projects/leveldb` (this is the real acceptance case
  and it takes minutes — run it once, record the result, and quote whether the
  published digests match round 3 §9.1's recorded
  `452a850ec90ab192…` for `analysis_facts`);
- any table where the measurement contradicted round 3's prose, stated as a
  retraction.

- [ ] **Step 7: Full verification**

```bash
cmake --build --preset default
cd build && ctest --output-on-failure --label-regex integration --label-regex analysis
```

Then the policy checks:

```bash
git diff --check
for f in $(git diff --name-only main); do
  case "$f" in
    *.h|*.hpp|*.cpp|*.py|CMakeLists.txt)
      head -20 "$f" | grep -q 'Licensed under the Apache License, Version 2.0' \
        || echo "missing license header: $f" ;;
  esac
done
```

Expected: no whitespace errors, no missing headers. Then run the full suite and
confirm **no skips** — a `GTEST_SKIP` reports as passed, so check the skip count
explicitly rather than trusting a green summary.

- [ ] **Step 8: Commit**

```bash
git add src/summarydb/StoreEquivalence.cpp \
        tests/unit/summarydb/StoreEquivalenceTest.cpp \
        tests/unit/summarydb/StoreEquivalenceEndToEndTest.cpp \
        tests/unit/summarydb/CMakeLists.txt \
        docs/specs/milestones/m13-scale-profile-acceptance-record.md
git commit -m "test(summarydb): determine the store projection by measurement"
```

---

## Plan Self-Review Record

**Spec coverage.** M13's three deliverables in
`docs/plans/veritas-scaling-milestone-roadmap.md` §6.1 map as: the
`--scale-profile` flag with `baseline` default → Tasks 1-2; the store-diff
equivalence harness → Tasks 3-6; the perturbation tests for the harness itself →
Task 5, with the single-cell, dropped-row, and reordered-row controls all
present, plus an extra table-presence control. §6.2's oracle role is served by
`veritas-store-diff` being usable on any two roots. §14.1's "whole-store sweep,
not a four-table check" is satisfied by `ListTables` enumerating
`sqlite_master` rather than a hardcoded list. §14.6's committed measurement
artefact for M13 is the acceptance record in Task 7.

**Task ordering.** Task 5's perturbation tests need `MetadataStore::Open` to
mutate a store mid-test, which is why Task 3's read-only open is separate and
earlier. Task 6's Step 3 is expected to *fail* to show equivalence, and Task 7
resolves that — the plan does not pretend the projection is known before it is
measured.

**Known gap, deliberately left to M14.** Nothing here measures wall time, peak
RSS, or store bytes. Those are §14.6's measurement record and belong to the M14
plan, because they need the corpus that plan builds.
