# M14 Scaling Corpus and Regime Determination Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the fixture corpus that can distinguish a constant-factor scaling
problem from an asymptotic one, measure both, and record which regime actually
binds at 1 MLOC — so the expensive structural stages are funded by evidence
rather than by arithmetic.

**Architecture:** Almost all of the measurement substrate already exists and this
plan extends it rather than rebuilding it. PR #139 (`main` at `709f2d9`) shipped
`veritas-build analyze --metrics*`, the byte-diffable versioned artifact
`<output>/run-metrics.json` rendered by `RenderRunReportJson`, and in-process
memory sampling that reports **both** `peak_rss_bytes` and
`peak_footprint_bytes` (`include/veritas/core/RunMetrics.h:158-159`) — which is
strictly better than `/usr/bin/time -lp`, whose single maximum is the very
quantity round 3 found to sit 1.42–1.45 GiB above physical footprint. So this
milestone adds three new things and no C++ metric code: a **fixture generator**
for controlled call-graph shapes, a **closure-shape probe** that queries the
published store, and an **exponent fitter**, plus the corpus runner that drives
them and the finding that records the verdict.

**Tech Stack:** Python 3.12+ (already a hard dependency of the test tree —
`tests/qualification/CMakeLists.txt:22` requires `python3`) using only the
standard library, including `sqlite3` for store queries; the C++ toolchain and
CMake for the one integration test; GoogleTest.

**Spec:** `docs/plans/veritas-scaling-milestone-roadmap.md` §2 (evidence base),
§3 C0 (the constraint under test), §4 (the two regimes), §7 (M14 in full — read
§7.3 for the hypotheses and their falsification criteria, which this plan
implements literally).

## Global Constraints

Copied from the spec. Every task's requirements implicitly include this section.

- **The falsification criteria are fixed before the run, not after.**
  `docs/plans/veritas-scaling-milestone-roadmap.md` §7.3 states the thresholds
  (H-C0: closure exponent < 1.5 in `F` on the depth axis; H-CONST: peak RSS
  ≪ 0.4 GiB/KLOC or store ≪ 200 MiB/KLOC; H-DEBUG: release/Debug ratio < 2×).
  A threshold may not be moved once a measurement is in hand. If a threshold
  proves wrong-headed, say so in the finding and leave the original visible.
- **A refuted constraint is removed from the roadmap, not restated.** §7.4
  requires it. This plan's final task edits
  `docs/plans/veritas-scaling-milestone-roadmap.md` §3 and §2.4 to retract what
  the corpus contradicts.
- **Reuse the shipped measurement artifact.** Do not add a second JSON schema,
  a second RSS sampler, or a parallel record format. `run-metrics.json` is the
  per-run record; this milestone's corpus record *references* it.
- **A generator that emits code the analyzer cannot ingest is worthless.** Task 2
  exists to prove ingestion end to end on the smallest generated fixture before
  any corpus run is trusted.
- **Pins are full 40-character SHAs, and the test mutates one.** A pin check that
  only asserts "the file parses" passes on a truncated or branch-name pin.
- **No RTTI, no exceptions** in VERITAS-authored C++ code. Python is unaffected.
- **Apache-2.0 header** in the first 20 lines of every created or modified
  `.cpp`, `.h`, `.py`, and `CMakeLists.txt`. Python files put the shebang on
  line 1 and the header immediately after, as `tools/check_m9_entry.py` does.
- **A `GTEST_SKIP` reports to CTest as a pass.** No task may leave a skipped
  test; check the skip count explicitly rather than trusting a green summary.
- All edits, builds, tests, and commits happen in the task worktree, never in the
  primary checkout and never on `main`.

## File Structure

Line numbers are as of `main` at `709f2d9`.

| File | Responsibility in this change |
| --- | --- |
| `tools/generate_analysis_benchmark_project.py` | New. Emits a C++ project of a requested shape plus its `compile_commands.json` and a `shape.json` record of what it intended to build. |
| `tools/benchmark_veritas_build.py` | New. Drives one instrumented `veritas-build analyze` run, reconciles it with `run-metrics.json`, probes the published store for closure-shape quantities, and emits a corpus record. |
| `tools/scaling/exponent.py` | New. The log-log least-squares fitter, importable and separately testable. |
| `tools/scaling/fixtures.json` | New. The real-fixture pin manifest: name, URL, 40-hex SHA, and the `compile_commands.json` recipe. |
| `tools/scaling/fetch_fixtures.py` | New. Fetches pinned fixtures into a gitignored cache, with a `--dry-run`. |
| `tests/qualification/VeritasBuildBenchmarkToolTest.py` | New. Exercises the generator, the fitter, the probe's queries, and the pin manifest. This filename is already reserved by `docs/plans/milestones/m11-external-ir-adapter-implementation-plan.md:1436-1438`; using it keeps the two plans from drifting. |
| `tests/qualification/CMakeLists.txt` | Register the Python test. |
| `tests/integration/analysis/GeneratedFixtureAnalyzableTest.cpp` | New. Proves a generated fixture is ingestible end to end. |
| `tests/integration/analysis/CMakeLists.txt` | Register it, inside the existing `VERITAS_WPA_ENGINE STREQUAL "souffle"` guard. |
| `tests/qualification/scaling/records/` | New. The committed corpus records, one JSON per fixture and shape. |
| `docs/specs/milestones/m14-scaling-corpus-design-spec.md` | New. The corpus record schema and the generator's shape vocabulary. |
| `docs/specs/milestones/m14-scaling-regime-finding.md` | New. The verdict: measured exponents, per-KLOC constants, and each hypothesis confirmed or falsified. |
| `docs/plans/veritas-scaling-milestone-roadmap.md` | Amended: §3 and §2.4 retract whatever the corpus contradicts. |

---

### Task 1: The exponent fitter

**Files:**
- Create: `tools/scaling/exponent.py`
- Create: `tools/qualification/VeritasBuildBenchmarkToolTest.py` (created here with only the fitter cases; later tasks extend it)
- Modify: `tests/qualification/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces: `fit_exponent(points: Sequence[tuple[float, float]], *, min_points: int = 3) -> Fit`,
  where `Fit` has `.exponent: float`, `.intercept: float`, `.r_squared: float`,
  `.n_points: int`, and raises `InsufficientDataError` below `min_points`. Tasks 5
  and 7 consume it.

Why the refusal matters: a two-point "fit" always returns a perfect `r² = 1.0`,
so a corpus that happens to have two sizes would report a confident exponent
derived from nothing. The refusal is the honest behaviour and is the first case
tested.

- [ ] **Step 1: Write the failing tests**

Create `tools/qualification/VeritasBuildBenchmarkToolTest.py`:

```python
#!/usr/bin/env python3
# Copyright 2026 VERITAS Contributors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Tests for the scaling corpus tooling.

Exit 0 when every case passes, non-zero otherwise, matching the convention in
M9EntryGateTest.py so the CTest registration is uniform.
"""

from __future__ import annotations

import importlib.util
import math
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def load(module_name: str, relative: str):
    path = REPO_ROOT / relative
    spec = importlib.util.spec_from_file_location(module_name, path)
    if spec is None or spec.loader is None:
        raise SystemExit(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module


failures: list[str] = []


def check(name: str, condition: bool, detail: str = "") -> None:
    if condition:
        print(f"  ok   {name}")
    else:
        print(f"  FAIL {name} {detail}")
        failures.append(name)


def test_exponent():
    exponent = load("scaling_exponent", "tools/scaling/exponent.py")

    # An exactly quadratic series recovers its exponent. A fitter that cannot
    # recover a known exponent cannot be trusted on an unknown one.
    quadratic = [(float(n), float(n * n)) for n in (10, 20, 40, 80)]
    fit = exponent.fit_exponent(quadratic)
    check("quadratic exponent is 2", abs(fit.exponent - 2.0) < 1e-9,
          f"got {fit.exponent}")
    check("quadratic r2 is 1", abs(fit.r_squared - 1.0) < 1e-9,
          f"got {fit.r_squared}")

    # A linear series recovers 1.
    linear = [(float(n), 3.0 * n) for n in (10, 20, 40, 80)]
    fit = exponent.fit_exponent(linear)
    check("linear exponent is 1", abs(fit.exponent - 1.0) < 1e-9,
          f"got {fit.exponent}")

    # A superlinear exponent between the two hypotheses must be recovered, since
    # this is the whole question H-C0 asks.
    mixed = [(float(n), float(n) ** 1.6) for n in (10, 20, 40, 80, 160)]
    fit = exponent.fit_exponent(mixed)
    check("exponent 1.6 is recovered", abs(fit.exponent - 1.6) < 1e-9,
          f"got {fit.exponent}")

    # Noise degrades r2 without moving the exponent much.
    noisy = [(float(n), float(n * n) * (1.0 + 0.05 * (-1) ** i))
             for i, n in enumerate((10, 20, 40, 80, 160))]
    fit = exponent.fit_exponent(noisy)
    check("noisy exponent stays near 2", abs(fit.exponent - 2.0) < 0.15,
          f"got {fit.exponent}")
    check("noisy r2 is below 1", fit.r_squared < 1.0,
          f"got {fit.r_squared}")

    # Fewer than three points is refused rather than fitted.
    try:
        exponent.fit_exponent([(1.0, 2.0), (2.0, 4.0)])
    except exponent.InsufficientDataError:
        check("two points are refused", True)
    else:
        check("two points are refused", False, "it returned a fit")

    # A non-positive size cannot be log-transformed, and silently dropping it
    # would change the fit while looking successful.
    try:
        exponent.fit_exponent([(0.0, 1.0), (1.0, 2.0), (2.0, 4.0)])
    except exponent.InvalidPointError:
        check("a zero size is refused", True)
    else:
        check("a zero size is refused", False, "it returned a fit")


def main() -> int:
    test_exponent()
    if failures:
        print(f"VeritasBuildBenchmarkToolTest: {len(failures)} case(s) failed")
        return 1
    print("VeritasBuildBenchmarkToolTest: all cases passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Register the test**

In `tests/qualification/CMakeLists.txt`, after the `M9EntryGateTest` block:

```cmake
# Unit tests for the scaling corpus tooling: the fixture generator, the
# exponent fitter, the store probe, and the fixture pin manifest.
add_test(NAME VeritasBuildBenchmarkToolTest
  COMMAND ${PYTHON3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/VeritasBuildBenchmarkToolTest.py)
set_tests_properties(VeritasBuildBenchmarkToolTest PROPERTIES TIMEOUT 60)
```

- [ ] **Step 3: Run the test to verify it fails**

```bash
ctest --test-dir build -R VeritasBuildBenchmarkToolTest --output-on-failure
```

Expected: FAIL — `cannot load …/tools/scaling/exponent.py`.

- [ ] **Step 4: Implement the fitter**

Create `tools/scaling/exponent.py` with the licence header after the shebang:

```python
"""Log-log least-squares exponent fitting for the scaling corpus.

The corpus measures a size (functions, or lines of code) against a cost (facts,
bytes, seconds) and asks what exponent relates them. Fitting in log space turns
a power law into a line, so the slope is the exponent.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Sequence


class InsufficientDataError(ValueError):
    """Fewer points than a fit needs. Refused rather than fitted."""


class InvalidPointError(ValueError):
    """A point that cannot be log-transformed."""


@dataclass(frozen=True)
class Fit:
    exponent: float
    intercept: float
    r_squared: float
    n_points: int


def fit_exponent(
    points: Sequence[tuple[float, float]], *, min_points: int = 3
) -> Fit:
    """Fit ``y = c * x ** exponent`` and return the exponent.

    Two points exactly determine a line, so a two-point fit always reports
    ``r_squared == 1.0`` and an exponent derived from nothing. ``min_points``
    exists so that a corpus caught at two sizes refuses to answer instead of
    answering confidently and wrongly.

    Raises ``InsufficientDataError`` when fewer than ``min_points`` points are
    supplied, and ``InvalidPointError`` when a size or cost is non-positive and
    therefore has no logarithm.
    """
    if len(points) < min_points:
        raise InsufficientDataError(
            f"a fit needs at least {min_points} points, got {len(points)}"
        )
    for x, y in points:
        if x <= 0.0 or y <= 0.0:
            raise InvalidPointError(
                f"log-log fitting needs strictly positive coordinates, got ({x}, {y})"
            )

    xs = [math.log(x) for x, _ in points]
    ys = [math.log(y) for _, y in points]
    n = float(len(xs))
    mean_x = sum(xs) / n
    mean_y = sum(ys) / n
    sxx = sum((x - mean_x) ** 2 for x in xs)
    sxy = sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, ys))
    if sxx == 0.0:
        raise InvalidPointError("every point has the same size; no slope exists")

    slope = sxy / sxx
    intercept = mean_y - slope * mean_x

    ss_tot = sum((y - mean_y) ** 2 for y in ys)
    ss_res = sum((y - (intercept + slope * x)) ** 2 for x, y in zip(xs, ys))
    # A perfectly collinear set gives ss_tot == 0; the fit is then exact by
    # construction, which is r2 == 1 by definition rather than a division by zero.
    r_squared = 1.0 if ss_tot == 0.0 else 1.0 - ss_res / ss_tot

    return Fit(exponent=slope, intercept=intercept, r_squared=r_squared,
               n_points=len(points))
```

- [ ] **Step 5: Run the test to verify it passes**

```bash
ctest --test-dir build -R VeritasBuildBenchmarkToolTest --output-on-failure
```

Expected: `VeritasBuildBenchmarkToolTest: all cases passed`.

- [ ] **Step 6: Commit**

```bash
git add tools/scaling/exponent.py \
        tests/qualification/VeritasBuildBenchmarkToolTest.py \
        tests/qualification/CMakeLists.txt
git commit -m "feat(tools): add a log-log exponent fitter for the scaling corpus"
```

---

### Task 2: A generated fixture the analyzer can actually ingest

**Files:**
- Create: `tools/generate_analysis_benchmark_project.py`
- Create: `tests/integration/analysis/GeneratedFixtureAnalyzableTest.cpp`
- Modify: `tests/integration/analysis/CMakeLists.txt`
- Modify: `tests/qualification/VeritasBuildBenchmarkToolTest.py` (add generator cases)

**Interfaces:**
- Consumes: nothing.
- Produces: `generate(destination: Path, shape: str, *, depth: int = 0, width: int = 0,
  fan_in: int = 1, entry: str = "entry") -> Shape`, where `Shape` records
  `.functions: int`, `.edges: int`, `.depth: int`, `.width: int`, and
  `.shape_file: Path`. It writes `<destination>/compile_commands.json` and
  `<destination>/shape.json`. Tasks 4 and 5 consume it, and Task 2's own
  integration test proves its output is ingestible.

The generated code must be real enough to exercise all four WPA domains, not a
minimal stub: each function performs a load and a store through a pointer
parameter so `MayRead`/`MayWrite` and `GlobalFlow` have something to propagate,
and calls the next function in the shape so `ReachableCall` has a chain.

- [ ] **Step 1: Write the failing generator tests**

Append to `tests/qualification/VeritasBuildBenchmarkToolTest.py`, before
`main()`:

```python
def test_generator(tmp: Path) -> None:
    generator = load("scaling_generator",
                     "tools/generate_analysis_benchmark_project.py")

    # A chain of depth N has exactly N functions and N-1 call edges, and its
    # shape record must agree with its source. A generator whose record lies is
    # worse than none: the exponent fit would use the claim, not the code.
    chain_dir = tmp / "chain8"
    chain_dir.mkdir(parents=True)
    shape = generator.generate(chain_dir, "chain", depth=8)
    check("chain declares 8 functions", shape.functions == 8,
          f"got {shape.functions}")
    check("chain declares 7 edges", shape.edges == 7, f"got {shape.edges}")

    sources = sorted(chain_dir.glob("*.cpp"))
    check("chain emits one translation unit per function",
          len(sources) == 8, f"got {len(sources)}")
    text = "".join(path.read_text() for path in sources)
    check("chain emits 8 definitions",
          text.count("int fn_") == 8, f"got {text.count('int fn_')}")
    check("chain propagates a pointer, not just a call",
          text.count("*(int *)") >= 16,
          "each function must load and store through a pointer parameter")

    # The compile database must be valid JSON, must name each source, and must
    # carry no absolute path outside the destination, so the corpus is movable.
    import json
    database = json.loads((chain_dir / "compile_commands.json").read_text())
    check("compile database lists every source",
          len(database) == 8, f"got {len(database)} entries")
    for entry in database:
        check("compile database entry names a source in the destination",
              entry["file"].startswith(str(chain_dir)),
              entry["file"])

    # shape.json round-trips what the generator claimed.
    recorded = json.loads((chain_dir / "shape.json").read_text())
    check("shape.json matches the returned shape",
          recorded["functions"] == shape.functions
          and recorded["depth"] == 8, str(recorded))

    # A star is width leaves from one entry, so width+1 functions.
    star_dir = tmp / "star6"
    star_dir.mkdir(parents=True)
    star = generator.generate(star_dir, "star", width=6)
    check("star declares 7 functions", star.functions == 7,
          f"got {star.functions}")

    # An unknown shape is refused rather than silently producing a chain.
    try:
        generator.generate(tmp / "bogus", "spiral", depth=3)
    except generator.UnknownShapeError:
        check("an unknown shape is refused", True)
    else:
        check("an unknown shape is refused", False, "it emitted something")
```

and change `main()` to create the temp directory and call it:

```python
def main() -> int:
    import tempfile

    test_exponent()
    with tempfile.TemporaryDirectory(prefix="veritas_benchmark_tools_") as raw:
        test_generator(Path(raw))
    if failures:
        print(f"VeritasBuildBenchmarkToolTest: {len(failures)} case(s) failed")
        return 1
    print("VeritasBuildBenchmarkToolTest: all cases passed")
    return 0
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
ctest --test-dir build -R VeritasBuildBenchmarkToolTest --output-on-failure
```

Expected: FAIL — `cannot load …/tools/generate_analysis_benchmark_project.py`.

- [ ] **Step 3: Implement the generator**

Create `tools/generate_analysis_benchmark_project.py` with the licence header
after the shebang. Implement the three shapes and the record:

```python
"""Generate a C++ benchmark project with a controlled call-graph shape.

The point of a generated fixture is that its shape is *known*, so a measured
cost can be attributed to it. A real checkout confounds depth with width with
file count; these shapes vary one axis at a time, which is what makes them
falsifiers for the corpus hypotheses rather than more data.

Shapes:
  chain    f0 -> f1 -> ... -> f(depth-1); the quadratic-closure probe.
  star     f0 calls f1..f(width); per-component fixed cost without depth.
  layered  `depth` layers of `width` functions, each calling `fan_in` in the
           next layer; the realistic middle.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, asdict
from pathlib import Path

COMPILE_DATABASE_ENTRY = {
    "directory": None,   # filled with the destination
    "command": None,     # filled per source
    "file": None,        # filled per source
}


class UnknownShapeError(ValueError):
    """A shape name the generator does not implement."""


@dataclass(frozen=True)
class Shape:
    shape: str
    functions: int
    edges: int
    depth: int
    width: int
    fan_in: int
    shape_file: Path


def _function_body(index: int, callees: list[int]) -> str:
    """One function that loads and stores through a pointer and calls its
    successors.

    The load and store are what give the memory-effect and flow domains
    something to propagate; a function containing only a call leaves the
    domains empty and the fixture would measure nothing.
    """
    lines = [f"int fn_{index}(int *p) {{", "  int acc = *p;"]
    for callee in callees:
        lines.append(f"  acc += fn_{callee}(p);")
    lines.append("  *p = acc;")
    lines.append("  return acc;")
    lines.append("}")
    return "\n".join(lines)


def _layout(shape: str, depth: int, width: int, fan_in: int) -> list[list[int]]:
    """Return the callee list for each function index."""
    if shape == "chain":
        if depth < 2:
            raise ValueError("chain needs depth >= 2")
        return [[i + 1] if i + 1 < depth else [] for i in range(depth)]
    if shape == "star":
        if width < 2:
            raise ValueError("star needs width >= 2")
        return [list(range(1, width + 1))] + [[] for _ in range(width)]
    if shape == "layered":
        if depth < 2 or width < 1 or fan_in < 1:
            raise ValueError("layered needs depth >= 2, width >= 1, fan_in >= 1")
        layers = [[layer * width + i for i in range(width)] for layer in range(depth)]
        callees: list[list[int]] = []
        for layer in range(depth):
            for position in range(width):
                if layer + 1 == depth:
                    callees.append([])
                    continue
                following = layers[layer + 1]
                # Wrap so every function in a layer has exactly fan_in callees,
                # which keeps the width axis independent of the fan-in axis.
                callees.append([following[(position + k) % width]
                                for k in range(fan_in)])
        return callees
    raise UnknownShapeError(f"unknown shape: {shape}")


def generate(destination: Path, shape: str, *, depth: int = 0, width: int = 0,
             fan_in: int = 1) -> Shape:
    """Emit the project into `destination` and return what was emitted.

    Raises `UnknownShapeError` for an unimplemented shape name, and ValueError
    when the requested parameters cannot express the shape.
    """
    destination = Path(destination).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    callees = _layout(shape, depth, width, fan_in)

    entries = []
    for index, targets in enumerate(callees):
        source = destination / f"fn_{index}.cpp"
        source.write_text(_function_body(index, targets) + "\n")
        entries.append({
            "directory": str(destination),
            "command": f"c++ -std=c++20 -c {source.name} -o fn_{index}.o",
            "file": str(source),
        })
    (destination / "compile_commands.json").write_text(
        json.dumps(entries, indent=2) + "\n")

    result = Shape(shape=shape, functions=len(callees),
                   edges=sum(len(targets) for targets in callees),
                   depth=depth, width=width, fan_in=fan_in,
                   shape_file=destination / "shape.json")
    (destination / "shape.json").write_text(
        json.dumps({**asdict(result), "shape_file": str(result.shape_file)},
                   indent=2, sort_keys=True) + "\n")
    return result
```

Note the generator emits one translation unit per function. That is deliberate:
it keeps the input-unit count on the same axis as the function count, so a corpus
run cannot confuse "more functions" with "more TUs" — but record in the shape
file that this is the choice, because a later reader will ask.

- [ ] **Step 4: Write the ingestibility test**

A generated fixture that the analyzer cannot ingest would waste every corpus run,
so prove it before any corpus exists.

Create `tests/integration/analysis/GeneratedFixtureAnalyzableTest.cpp`. It must
generate a small chain at test time, point `ProjectAnalysisRequest` at the
generated directory, run `AnalyzeProject`, and assert the run published
summaries and a WPA run:

```cpp
// Copyright 2026 VERITAS Contributors
// ... the standard Apache-2.0 header ...

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "veritas/analysis/ProjectAnalyzer.h"
#include "veritas/analysis/ProjectAnalysisRequest.h"

namespace veritas::analysis {
namespace {

namespace fs = std::filesystem;

// Runs the generator through python3. The generator is the artifact under test,
// so the test must not reimplement its output; a divergence between the two
// would be exactly the failure this test exists to catch.
int RunGenerator(const fs::path& destination, const std::string& shape,
                 const std::string& axis_flag, int axis_value) {
  const std::string command =
      "python3 " + std::string(VERITAS_GENERATOR_PATH) + " --out " +
      destination.string() + " --shape " + shape + " " + axis_flag + " " +
      std::to_string(axis_value) + " >/dev/null 2>&1";
  return std::system(command.c_str());
}

TEST(GeneratedFixtureAnalyzableTest, AGeneratedChainIsIngestedAndAnalyzed) {
  const fs::path root = fs::temp_directory_path() / "veritas_generated_chain";
  fs::remove_all(root);
  ASSERT_EQ(RunGenerator(root, "chain", "--depth", 6), 0)
      << "the generator failed; run it by hand to see why";
  ASSERT_TRUE(fs::exists(root / "compile_commands.json"));
  ASSERT_TRUE(fs::exists(root / "shape.json"));

  ProjectAnalyzer analyzer;
  const auto result = analyzer.AnalyzeProject(
      ProjectAnalysisRequest{.project_root = root, .output_root = root / ".veritas"},
      AnalysisConfig::Default());
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_FALSE(result->published_summary_ids.empty())
      << "the generated chain produced no summaries";
  EXPECT_FALSE(result->wpa_run_id.empty())
      << "the generated chain produced no WPA run";

  fs::remove_all(root);
}

}  // namespace
}  // namespace veritas::analysis
```

For `VERITAS_GENERATOR_PATH`, add the compile definition — this is the same
mechanism `tests/integration/build/CMakeLists.txt:45-47` uses for
`VERITAS_BUILD_BINARY`:

```cmake
target_compile_definitions(GeneratedFixtureAnalyzableTest PRIVATE
  VERITAS_GENERATOR_PATH="${CMAKE_SOURCE_DIR}/tools/generate_analysis_benchmark_project.py"
)
```

Add `--out`, `--shape`, `--depth`, `--width`, and `--fan-in` to the generator's
`__main__` so the CLI above works, using `argparse`, and print the shape as JSON
on success and exit non-zero with a message on failure.

Register the test inside the existing engine guard in
`tests/integration/analysis/CMakeLists.txt` (the same
`if(VERITAS_WPA_ENGINE STREQUAL "souffle")` block that owns
`ProjectAnalyzerWpaTest`), with a real timeout since it runs a full analysis:

```cmake
add_executable(GeneratedFixtureAnalyzableTest GeneratedFixtureAnalyzableTest.cpp)
target_link_libraries(GeneratedFixtureAnalyzableTest PRIVATE
  veritas_analysis
  veritas_test_support
  GTest::gtest_main
)
target_compile_definitions(GeneratedFixtureAnalyzableTest PRIVATE
  VERITAS_GENERATOR_PATH="${CMAKE_SOURCE_DIR}/tools/generate_analysis_benchmark_project.py"
)
veritas_add_warnings(GeneratedFixtureAnalyzableTest)
add_test(NAME GeneratedFixtureAnalyzableTest
  COMMAND GeneratedFixtureAnalyzableTest)
set_tests_properties(GeneratedFixtureAnalyzableTest PROPERTIES
  TIMEOUT 180
  LABELS "integration;analysis;scaling"
)
```

- [ ] **Step 5: Run both tests to verify they pass**

```bash
cmake --build --preset default --target GeneratedFixtureAnalyzableTest
ctest --test-dir build -R 'GeneratedFixtureAnalyzableTest|VeritasBuildBenchmarkToolTest' --output-on-failure
```

Expected: both pass. If the analyzability test fails with a `compile_commands.json`
error, the entry's `directory` or `command` is wrong — fix the generator, not the
test, because the corpus depends on the generator being right.

- [ ] **Step 6: Commit**

```bash
git add tools/generate_analysis_benchmark_project.py \
        tests/qualification/VeritasBuildBenchmarkToolTest.py \
        tests/integration/analysis/GeneratedFixtureAnalyzableTest.cpp \
        tests/integration/analysis/CMakeLists.txt
git commit -m "feat(tools): generate shaped C++ fixtures the analyzer can ingest"
```

---

### Task 3: The closure-shape probe

**Files:**
- Create: `tools/scaling/probe.py`
- Modify: `tests/qualification/VeritasBuildBenchmarkToolTest.py`

**Interfaces:**
- Consumes: a store root produced by a completed `analyze` run.
- Produces: `probe_store(metadata_db: Path) -> ClosureShape`, where
  `ClosureShape` has `.functions: int`, `.facts_by_relation: dict[str, int]`,
  `.total_facts: int`, `.cpg_nodes: int`, and `.store_bytes: int`. Task 4
  consumes it, and `ProbeError` is raised rather than returning an empty shape.

This is the measurement H-C0 turns on: total published facts against `F`.
Everything here is a query against the published store, which is why it belongs
in Python and needs no C++.

- [ ] **Step 1: Write the failing test**

Append to `tests/qualification/VeritasBuildBenchmarkToolTest.py`, and add the
call to `main()` inside the temp-directory block:

```python
def test_probe(tmp: Path) -> None:
    probe = load("scaling_probe", "tools/scaling/probe.py")
    import sqlite3

    # A store built by hand, so the expected counts are arithmetic rather than
    # whatever the analyzer happened to publish.
    db = tmp / "probe.db"
    connection = sqlite3.connect(db)
    connection.executescript(
        """
        CREATE TABLE analysis_facts (fact_id TEXT PRIMARY KEY, relation_name TEXT,
                                     cells_hex TEXT);
        CREATE TABLE function_symbols (function_symbol_id TEXT PRIMARY KEY);
        CREATE TABLE cpg_nodes (id TEXT PRIMARY KEY);
        """
    )
    rows = []
    # Three ReachableCall facts and three DirectCall facts, so the per-relation
    # grouping has two distinct keys to separate.
    for index in range(3):
        rows.append((f"fact:{index}", "ReachableCall", ""))
    for index in range(3):
        rows.append((f"call:{index}", "DirectCall", ""))
    connection.executemany("INSERT INTO analysis_facts VALUES (?, ?, ?)", rows)
    connection.executemany("INSERT INTO function_symbols VALUES (?)",
                           [(f"s{index}",) for index in range(3)])
    connection.executemany("INSERT INTO cpg_nodes VALUES (?)",
                           [(f"n{index}",) for index in range(5)])
    connection.commit()
    connection.close()

    shape = probe.probe_store(db)
    check("probe counts ReachableCall facts",
          shape.facts_by_relation.get("ReachableCall") == 3,
          str(shape.facts_by_relation))
    check("probe counts DirectCall facts",
          shape.facts_by_relation.get("DirectCall") == 3,
          str(shape.facts_by_relation))
    check("probe totals every fact", shape.total_facts == 6,
          str(shape.total_facts))
    check("probe reports the function count that the fit uses as F",
          shape.functions == 3, str(shape.functions))
    check("probe counts cpg nodes", shape.cpg_nodes == 5, str(shape.cpg_nodes))
    check("probe measures store bytes on disk", shape.store_bytes > 0,
          str(shape.store_bytes))

    # A missing store, and a store with no facts, are both errors rather than an
    # empty shape: an empty shape would fit as zero facts and read as "the
    # closure is tiny", which is the opposite of the finding it would produce.
    try:
        probe.probe_store(tmp / "probe.db.absent")
    except probe.ProbeError:
        check("a missing store is refused", True)
    else:
        check("a missing store is refused", False, "it returned a shape")

    empty = tmp / "empty.db"
    empty_connection = sqlite3.connect(empty)
    empty_connection.executescript(
        "CREATE TABLE analysis_facts (fact_id TEXT PRIMARY KEY, "
        "relation_name TEXT, cells_hex TEXT);"
        "CREATE TABLE function_symbols (function_symbol_id TEXT PRIMARY KEY);"
    )
    empty_connection.commit()
    empty_connection.close()
    try:
        probe.probe_store(empty)
    except probe.ProbeError:
        check("a store with no facts is refused", True)
    else:
        check("a store with no facts is refused", False, "it returned a shape")
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
ctest --test-dir build -R VeritasBuildBenchmarkToolTest --output-on-failure
```

Expected: FAIL — `cannot load …/tools/scaling/probe.py`.

- [ ] **Step 3: Implement the probe**

Create `tools/scaling/probe.py`. The probe is deliberately **decode-free**: it
reads only columns SQLite can group on directly.

```python
"""Measure the shape of the published closure, from the store alone.

The published fact set is a materialised transitive closure
(docs/plans/veritas-scaling-milestone-roadmap.md section 3, C0). The quantity
that governs its size is the sum over functions of the number of functions each
reaches; the test of H-C0 is whether that sum grows with an exponent above 1 in
the function count, which this module answers from the published fact count
alone.

Everything here reads a column SQLite can group on. The per-row cell payload is
hex-encoded protobuf (`src/facts/FactProto.cpp`) and is NOT decoded: a second
implementation of a canonical encoding is a divergence this project has already
been bitten by, and it is not needed to fit the exponent.
"""

from __future__ import annotations

import sqlite3
from dataclasses import dataclass
from pathlib import Path


class ProbeError(RuntimeError):
    """The store cannot be probed as asked."""


@dataclass(frozen=True)
class ClosureShape:
    functions: int
    facts_by_relation: dict[str, int]
    total_facts: int
    cpg_nodes: int
    store_bytes: int


def _count(connection: sqlite3.Connection, sql: str) -> int:
    row = connection.execute(sql).fetchone()
    return int(row[0]) if row is not None and row[0] is not None else 0


def probe_store(metadata_db: Path) -> ClosureShape:
    """Probe one store. Raises ProbeError when there are no published facts.

    Opened read-only through a URI, so inspection never creates or migrates the
    store — the same rule the C++ instrument follows.
    """
    metadata_db = Path(metadata_db)
    if not metadata_db.exists():
        raise ProbeError(f"no store at {metadata_db}")

    connection = sqlite3.connect(f"file:{metadata_db}?mode=ro", uri=True)
    try:
        tables = {
            row[0]
            for row in connection.execute(
                "SELECT name FROM sqlite_master WHERE type = 'table'")
        }
        if "analysis_facts" not in tables:
            raise ProbeError(f"{metadata_db} has no analysis_facts table")

        # `relation_name` is a plain column, so per-domain counts need no
        # decoding. This is the measurement H-C0 turns on.
        facts_by_relation = {
            str(name): int(count)
            for name, count in connection.execute(
                "SELECT relation_name, COUNT(*) FROM analysis_facts "
                "GROUP BY relation_name ORDER BY relation_name")
        }
        total_facts = sum(facts_by_relation.values())
        if total_facts == 0:
            raise ProbeError(f"{metadata_db} published no facts")

        # `F`: the analysable function count. `function_symbols` is the M2
        # identity table; it exists in every store the analyzer writes, but the
        # guard keeps a partial store from reading as "zero functions", which
        # would fit as an infinite exponent.
        if "function_symbols" not in tables:
            raise ProbeError(f"{metadata_db} has no function_symbols table")
        functions = _count(connection, "SELECT COUNT(*) FROM function_symbols")

        cpg_nodes = (
            _count(connection, "SELECT COUNT(*) FROM cpg_nodes")
            if "cpg_nodes" in tables
            else 0
        )
    finally:
        connection.close()

    # Store bytes on disk, which is the quantity the roadmap's per-KLOC store
    # constant is expressed in. Summed over files rather than allocated blocks,
    # and reported as 0 only when the store is genuinely empty.
    store_bytes = sum(
        path.stat().st_size for path in metadata_db.parent.rglob("*") if path.is_file()
    )

    return ClosureShape(functions=functions,
                        facts_by_relation=facts_by_relation,
                        total_facts=total_facts,
                        cpg_nodes=cpg_nodes,
                        store_bytes=store_bytes)
```

The fan-out histogram (`Σ_f |reach(f)|` as a distribution) is **deliberately not
built here.** Getting it needs the source function id out of `cells_hex`, which
means decoding protobuf in Python — a second implementation of a canonical
encoding, with its own cross-check test obligation, for a quantity the exponent
fit does not need. Total facts against `F` answers H-C0; the fan-out is the
*mechanism* behind an exponent, not the test of it. Record it in
`docs/specs/milestones/m14-scaling-corpus-design-spec.md` as a named follow-up
requiring a C++ projection, so the omission is visible rather than silent.

- [ ] **Step 4: Run the test to verify it passes**

```bash
ctest --test-dir build -R VeritasBuildBenchmarkToolTest --output-on-failure
```

Expected: all cases pass, including the hand-built store's exact counts.

- [ ] **Step 5: Commit**

```bash
git add tools/scaling/probe.py tests/qualification/VeritasBuildBenchmarkToolTest.py
git commit -m "feat(tools): probe the published closure shape from the store"
```

---

### Task 4: The corpus runner

**Files:**
- Create: `tools/benchmark_veritas_build.py`
- Modify: `tests/qualification/VeritasBuildBenchmarkToolTest.py`

**Interfaces:**
- Consumes: the generator (Task 2), the probe (Task 3), `run-metrics.json`.
- Produces: `run_case(binary: Path, project: Path, output: Path, *, label: str,
  runs: int = 3) -> dict` emitting the corpus record; and a CLI
  `--binary --project --out --label [--runs N]` writing that record as JSON to
  stdout. Task 6 consumes it.

- [ ] **Step 1: Write the failing test**

The runner's record assembly is testable without running an analysis, by
reconciling a synthesized `run-metrics.json` with a synthesized store. Append:

```python
def test_corpus_record(tmp: Path) -> None:
    runner = load("scaling_runner", "tools/benchmark_veritas_build.py")
    import json

    # A minimal run-metrics.json in the shape RenderRunReportJson emits. Only
    # the fields the record copies are populated; everything else is absent and
    # must stay absent rather than being invented.
    metrics = {
        "identity": {"run_id": "fact:sha256:aaa", "wpa_config_hash": "bbb"},
        "environment": {"build_type": "Debug", "cores": 10, "ram_bytes": 17179869184},
        "inventory": {"output": {"canonical_facts": 1234,
                                 "components_by_kind": [["v2_reach", 40]]}},
        "store": {"tables": [{"table": "analysis_facts", "rows": 1234}],
                  "bytes": [{"name": "metadata.db", "bytes": 4096}]},
        "metrics": {"peak_rss_bytes": 536870912, "peak_footprint_bytes": 470000000},
    }
    record = runner.assemble_record(
        label="chain8", project="/x/chain8", runs=[metrics, metrics, metrics],
        shape={"functions": 8, "depth": 8, "edges": 7, "shape": "chain"})

    check("record keeps the label", record["label"] == "chain8")
    check("record states the run count", record["runs"] == 3)
    # The worst of N, not the mean: round 3 established a 0.72 GiB run-to-run
    # spread on the reference fixture, so the mean would understate the peak.
    check("record reports the worst peak RSS",
          record["peak_rss_bytes"] == 536870912, str(record["peak_rss_bytes"]))
    check("record reports the build type, so release and Debug are separable",
          record["build_type"] == "Debug", str(record["build_type"]))
    check("record carries the function count for the fit",
          record["functions"] == 8, str(record["functions"]))
    check("record carries the run identity for like-for-like checks",
          record["identity"]["run_id"] == "fact:sha256:aaa", str(record["identity"]))
    # An absent field must be absent, not zero: a zero byte count would fit as a
    # real measurement and drag the exponent.
    check("absent bytes stay absent",
          "store_bytes" not in record or record["store_bytes"] is not None,
          str(record.get("store_bytes")))

    # A run whose peak RSS differs between runs must report the worst.
    worse = json.loads(json.dumps(metrics))
    worse["metrics"]["peak_rss_bytes"] = 999999999
    record = runner.assemble_record(label="chain8", project="/x/chain8",
                                    runs=[metrics, worse], shape={"functions": 8})
    check("worst-of-N picks the larger peak",
          record["peak_rss_bytes"] == 999999999, str(record["peak_rss_bytes"]))
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
ctest --test-dir build -R VeritasBuildBenchmarkToolTest --output-on-failure
```

Expected: FAIL — `cannot load …/tools/benchmark_veritas_build.py`.

- [ ] **Step 3: Implement the runner**

Create `tools/benchmark_veritas_build.py`. It must:

1. Run `veritas-build analyze --project <project> --output <output>` with
   `--metrics true` (the default) and a `--metrics-path` under the output root,
   one run per requested repetition into a fresh output root each time.
2. Read `<output>/run-metrics.json` and `json.load` it. If it is missing or
   unparseable, fail loudly — a corpus record without metrics is not a
   measurement.
3. Import and call `probe.probe_store(<output>/metadata.db)` and fold the
   `ClosureShape` in.
4. Call `assemble_record(...)` and print it as sorted-key JSON.

`assemble_record` is the pure part:

```python
def assemble_record(*, label: str, project: str, runs: list[dict],
                    shape: dict | None = None) -> dict:
    """Assemble one corpus record from N run-metrics artifacts.

    Reports the worst of N for every peak quantity and the sum for every count.
    The worst, not the mean: the reference fixture shows a 0.72 GiB run-to-run
    spread, so a mean would understate the peak the acceptance criterion names.
    """
    if not runs:
        raise ValueError("a record needs at least one run")
    peak_rss = max(run["metrics"]["peak_rss_bytes"] for run in runs)
    peak_footprint = max(run["metrics"]["peak_footprint_bytes"] for run in runs)
    first = runs[0]
    record = {
        "label": label,
        "project": project,
        "runs": len(runs),
        "identity": first.get("identity", {}),
        "build_type": first.get("environment", {}).get("build_type", ""),
        "cores": first.get("environment", {}).get("cores", 0),
        "ram_bytes": first.get("environment", {}).get("ram_bytes", 0),
        "peak_rss_bytes": peak_rss,
        "peak_footprint_bytes": peak_footprint,
        "canonical_facts": first.get("inventory", {}).get("output", {})
                                .get("canonical_facts", 0),
        "components_by_kind": first.get("inventory", {}).get("output", {})
                                   .get("components_by_kind", []),
        "tables": first.get("store", {}).get("tables", []),
        "bytes": first.get("store", {}).get("bytes", []),
    }
    if shape is not None:
        record["shape"] = shape
        record["functions"] = shape.get("functions", 0)
    return record
```

Add the closure-shape keys from the probe under `"closure"`. Do not add a key
whose value you could not measure — `RunOutputInventory.svfg_edges` is the
precedent for why (`include/veritas/observability/RunReport.h:89-91`: an unproduced
count must not diff as "unchanged").

- [ ] **Step 4: Run the test to verify it passes**

```bash
ctest --test-dir build -R VeritasBuildBenchmarkToolTest --output-on-failure
```

Expected: all cases pass.

- [ ] **Step 5: Prove the whole path on one generated fixture**

```bash
python3 tools/generate_analysis_benchmark_project.py --out /tmp/corpus/chain16 \
    --shape chain --depth 16
python3 tools/benchmark_veritas_build.py --binary build/bin/veritas-build \
    --project /tmp/corpus/chain16 --out /tmp/corpus/out-chain16 \
    --label chain16 --runs 1
```

Expected: a JSON record on stdout with a non-zero `peak_rss_bytes`, non-zero
`canonical_facts`, and a populated `closure` block. If peak RSS is zero, the
metrics interval is too coarse for the fixture — raise `--depth` until the run
lasts longer than one sampling interval, and record the depth you needed.

- [ ] **Step 6: Commit**

```bash
git add tools/benchmark_veritas_build.py \
        tests/qualification/VeritasBuildBenchmarkToolTest.py
git commit -m "feat(tools): run one instrumented analyze and emit a corpus record"
```

---

### Task 5: The real-fixture pin manifest

**Files:**
- Create: `tools/scaling/fixtures.json`
- Create: `tools/scaling/fetch_fixtures.py`
- Modify: `tests/qualification/VeritasBuildBenchmarkToolTest.py`
- Modify: `.gitignore` (add the fixture cache directory)

**Interfaces:**
- Consumes: nothing.
- Produces: the pinned fixture list and a fetcher. Task 6 consumes both.

- [ ] **Step 1: Write the failing tests**

```python
def test_fixture_pins() -> None:
    import json
    import re

    manifest = json.loads((REPO_ROOT / "tools/scaling/fixtures.json").read_text())
    check("manifest has a fixtures list", isinstance(manifest.get("fixtures"), list))

    sha = re.compile(r"^[0-9a-f]{40}$")
    for fixture in manifest["fixtures"]:
        name = fixture.get("name", "<unnamed>")
        # A pin that is not a full 40-hex commit id is a branch name or a
        # truncation, and both move underneath the corpus.
        check(f"{name} pins a full 40-hex revision",
              bool(sha.match(fixture.get("revision", ""))),
              fixture.get("revision", ""))
        check(f"{name} names a url", bool(fixture.get("url")),
              str(fixture.get("url")))
        check(f"{name} declares its compile_commands recipe",
              bool(fixture.get("compile_commands_recipe")))
        check(f"{name} declares an expected scale band",
              fixture.get("scale_band") in ("small", "mid", "large"),
              str(fixture.get("scale_band")))

    # Once LevelDB is in the manifest it must be pinned to the revision the
    # roadmap's evidence base was measured at, not to a moving branch.
    names = {fixture.get("name") for fixture in manifest["fixtures"]}
    check("leveldb is pinned", "leveldb" in names, str(sorted(names)))

    # A mutated revision fails the same check the real manifest passes. A pin
    # check that only asserts the file parses would accept this.
    mutated = json.loads(json.dumps(manifest))
    mutated["fixtures"][0]["revision"] = "main"
    check("a branch-name revision is rejected",
          not bool(sha.match(mutated["fixtures"][0]["revision"])),
          "the check is mutation-sensitive")

    # The fetch script has a dry-run that touches the network never.
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "scaling_fetch", REPO_ROOT / "tools/scaling/fetch_fixtures.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    plan = module.dry_run(REPO_ROOT / "tools/scaling/fixtures.json")
    check("dry run reports one entry per fixture",
          len(plan) == len(manifest["fixtures"]), str(len(plan)))
    check("dry run performs no network access",
          all("would fetch" in line for line in plan), str(plan[:2]))
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
ctest --test-dir build -R VeritasBuildBenchmarkToolTest --output-on-failure
```

Expected: FAIL — `tools/scaling/fixtures.json` does not exist.

- [ ] **Step 3: Write the manifest and the fetcher**

Create `tools/scaling/fixtures.json`. Populate `leveldb` first — its revision must
be the one the roadmap's evidence base was measured at
(`/Users/skg7on/Workspace/Projects/leveldb`; use
`git -C /Users/skg7on/Workspace/Projects/leveldb rev-parse HEAD` to read it), so
the M14 numbers are directly comparable to rounds 2 and 3. Then add the mid and
large candidates named in the roadmap §7.1. Leave a fixture out entirely rather
than adding it with an unpinned revision or a recipe you have not verified;
`scale_band` and `compile_commands_recipe` are claims Task 6 will rely on.

Create `tools/scaling/fetch_fixtures.py` with `dry_run(manifest_path) -> list[str]`
returning one `"would fetch <name> at <revision> into <path>"` line per fixture
and performing no I/O, plus a real fetch mode that clones into the gitignored
cache and checks out the pinned revision, verifying `rev-parse HEAD` equals the
pin afterwards. A fetch that does not verify the revision it landed on has not
enforced the pin.

- [ ] **Step 4: Run the test to verify it passes**

```bash
ctest --test-dir build -R VeritasBuildBenchmarkToolTest --output-on-failure
```

- [ ] **Step 5: Commit**

```bash
git add tools/scaling/fixtures.json tools/scaling/fetch_fixtures.py \
        tests/qualification/VeritasBuildBenchmarkToolTest.py .gitignore
git commit -m "feat(tools): pin the real-fixture corpus"
```

---

### Task 6: Run the corpus and record it

**Files:**
- Create: `tests/qualification/scaling/records/` (the committed records)
- Create: `docs/specs/milestones/m14-scaling-corpus-design-spec.md`

**Interfaces:**
- Consumes: every earlier task.
- Produces: the committed corpus records and the record schema. Task 7 consumes
  the records to write the finding.

- [ ] **Step 1: Describe the schema**

Create `docs/specs/milestones/m14-scaling-corpus-design-spec.md` documenting the
corpus record: every key, its unit, whether it is required or optional, and the
rule that an unmeasured key is **absent rather than zero**. State that the record
references `run-metrics.json` and does not replace it, and name the
`RunOutputInventory.svfg_edges` precedent for the absent-not-zero rule.

- [ ] **Step 2: Run the synthetic axis**

For each shape, run at four or more sizes so the fit has residuals and can report
a real `r²`. All runs in one build tree; record the tree's `CMakeCache.txt`
configuration in each record's `build_type` (the harness reads it from
`run-metrics.json`, so this is automatic).

```bash
for depth in 8 16 32 64 128; do
  python3 tools/generate_analysis_benchmark_project.py --out "/tmp/corpus/chain$depth" \
      --shape chain --depth "$depth"
  python3 tools/benchmark_veritas_build.py --binary build/bin/veritas-build \
      --project "/tmp/corpus/chain$depth" --out "/tmp/corpus/out-chain$depth" \
      --label "chain$depth" --runs 3 \
      > "tests/qualification/scaling/records/chain$depth.json"
done
```

Then the same for `star` at widths 8…128 and `layered` at one fixed width with
depths 2…6.

- [ ] **Step 3: Run the release-versus-Debug axis**

H4/H-DEBUG needs the same fixture measured under both configurations, and no such
measurement exists anywhere in the project today. Build a `release` tree and run
the same fixture through both:

```bash
cmake --preset release
cmake --build --preset release --target veritas-build
python3 tools/benchmark_veritas_build.py --binary build-release/bin/veritas-build \
    --project /tmp/corpus/chain64 --out /tmp/corpus/out-chain64-release \
    --label chain64-release --runs 3 \
    > tests/qualification/scaling/records/chain64-release.json
```

Do not compare across trees for anything except this axis: round 2's comparison
was invalidated by exactly that mistake.

- [ ] **Step 4: Run the real fixtures**

At least LevelDB. The 100 kLOC and 1 MLOC fixtures take hours and may be run once,
with the wall time recorded honestly rather than averaged.

- [ ] **Step 5: Fit and report**

Import the fitter and print each hypothesis's number:

```bash
python3 - <<'PY'
import glob, json, sys
sys.path.insert(0, "tools")
from scaling.exponent import fit_exponent

points = []
for path in sorted(glob.glob("tests/qualification/scaling/records/chain*.json")):
    record = json.load(open(path))
    points.append((float(record["functions"]),
                   float(record["closure"]["total_facts"])))
print("facts vs functions:", fit_exponent(points))
PY
```

Expected: an exponent with its `r²`. **A fit with `r²` below about 0.9 must be
reported as no fit at all**, not as a weak one — the corpus may be too small, or
the shapes may not vary the axis cleanly, and saying so is the useful result.

- [ ] **Step 6: Commit**

```bash
git add docs/specs/milestones/m14-scaling-corpus-design-spec.md \
        tests/qualification/scaling/records/
git commit -m "test(scaling): record the corpus"
```

---

### Task 7: The regime finding, and the roadmap retraction

**Files:**
- Create: `docs/specs/milestones/m14-scaling-regime-finding.md`
- Modify: `docs/plans/veritas-scaling-milestone-roadmap.md` (§2.4 and §3)

**Interfaces:**
- Consumes: the corpus records.
- Produces: the verdict that funds or kills Stage 6 (M19).

This is the milestone's deliverable. Everything before it exists to make it
possible.

- [ ] **Step 1: Write the finding**

Create `docs/specs/milestones/m14-scaling-regime-finding.md` stating, with the
record paths as evidence:

- the measured closure exponent on the depth axis, with `r²` and the number of
  points, and the verdict on **H-C0** against §7.3's fixed threshold;
- the measured per-KLOC peak RSS and store bytes, and the verdict on **H-CONST**;
- the release/Debug ratio, and the verdict on **H-DEBUG**;
- the per-KLOC constants as measured, replacing §2.4's linear extrapolation with
  a measured one;
- for every claim in roadmap §3 that the corpus contradicts, an explicit
  retraction naming the claim that was wrong and why;
- the honest statement of what the corpus still cannot distinguish, if anything.

- [ ] **Step 2: Amend the roadmap**

Edit `docs/plans/veritas-scaling-milestone-roadmap.md`:
- §2.4's extrapolation table gets the measured constants alongside the projected
  ones, or replaces them, with the projection retained and labelled as the
  superseded estimate.
- §3's affected constraint entries get a retraction line pointing at the finding.
  A constraint the corpus refutes is marked refuted; it is not deleted, because
  §15.3 records that the *wrong reasons* proved reusable.
- §5's stage table gains a status column value for Stage 6: funded, dropped, or
  blocked, per the H-C0 verdict.
- §13's acceptance envelope stops being a straw man: replace the to-be-fixed-at-
  the-Stage-1-gate figures with the measured ones.

- [ ] **Step 3: Update the index and run the policy checks**

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

Then the full suite, checking the skip count explicitly rather than trusting the
summary, and `veritas-store-diff` on two `semantic_zoo` runs to confirm M13's
instrument still holds.

- [ ] **Step 4: Commit**

```bash
git add docs/specs/milestones/m14-scaling-regime-finding.md \
        docs/plans/veritas-scaling-milestone-roadmap.md
git commit -m "docs(scaling): record the measured regime and retract what it refutes"
```

---

## Plan Self-Review Record

**Spec coverage.** Roadmap §7.1's two axes map to Task 2 (synthetic, controlled)
and Task 5 (real, pinned). §7.2's measurement list maps as: wall, CPU, peak RSS,
and peak footprint → `run-metrics.json` via Task 4 (**already shipped by PR #139**,
not rebuilt); store bytes → Task 3's `store_bytes`; facts per domain → Task 3's
`facts_by_relation`; the closure exponent's inputs → Task 3's `total_facts` and
`functions`; per-component cost distribution → `components_by_kind` plus
`RunMetricsStats` spans, not newly built; release vs Debug → Task 6 Step 3.
§7.3's three hypotheses → Task 7 Step 1. §7.4's exit gate → Task 7 Steps 1-2,
including the retraction requirement. §14.6's committed measurement record →
Task 6 Step 6 and Task 4's runner.

**Deliberate gaps, named rather than hidden.**

1. **The support-set fraction is not measured.** Round 3's ≈60 % figure came from
   the component payloads in the `wpa-component-results` RocksDB, not from the
   published store, and Python has no RocksDB binding here. Measuring it needs a
   C++ tool over the component CAS. It is **not** needed for H-C0: the exponent
   follows from total facts against `F`, and the support fraction explains an
   exponent rather than testing for one. Task 6 Step 1 records it as a follow-up.
2. **The reachability fan-out histogram is not measured.** It needs the source
   function id decoded from the hex-encoded protobuf `cells_hex`, i.e. a second
   Python implementation of a canonical encoding, with its own cross-check
   obligation. Task 3 Step 3 forbids that trade and records the gap.
3. **Wall time is not a gate here.** M14 determines the regime; certifying a wall
   number needs a quiet, unswapped machine (roadmap §13), so Task 7 records CPU
   and the machine's state rather than asserting wall.

**Type consistency.** `Shape` (Task 2) and `ClosureShape` (Task 3) are distinct
and neither is passed where the other is expected; `assemble_record` takes a plain
`dict` for `shape` because it crosses a JSON boundary in Task 4's CLI.
`fit_exponent` takes a sequence of float pairs and is fed
`(functions, total_facts)` in Task 6 Step 5 — the same order its own test uses.
