# M10A Recursive Domain Expansion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add four recursive WPA domains — `MayRead`, `GlobalFlow`,
`UnknownEffect`, and `SoundnessCoverage` — to the compiled-Soufflé engine with a
C++ conformance oracle, and qualify them against the M10B input surface.

**Architecture:** Each domain follows the delivered `ReachableCall`/`MayWrite`
pattern: a Datalog bundle compiled by `veritas_generate_souffle_program`, a
`relations.v2` registry entry, a `rules.v2` witness rule ID, a mirrored C++
rule in `CppRuleEvaluator`, and materializer emission for any new EDB. The C++
engine remains a differential oracle only.

**Tech Stack:** C++20, Soufflé 2.5 (vendored), `veritas::Status`/`StatusOr<T>`,
`relations.v2` registry, `CppRuleEvaluator`, `WpaInputMaterializer`,
`WpaDifferentialQualificationTest`, GoogleTest, CMake/CTest.

**Spec:** `docs/specs/milestones/m10a-recursive-domain-expansion-design-spec.md`

## Global Constraints

- Four domains only: `MayRead`, `GlobalFlow`, `UnknownEffect`,
  `SoundnessCoverage`. Range, capacity, and dominating-check facts are out of
  scope (deferred to the local-analysis layer; see spec section 9).
- Production recursive WPA is compiled Soufflé; C++ is a conformance oracle and
  explicitly selected emergency mode, never a fallback.
- New relations are appended to `relations.v2`; existing ordinals are never
  reused. `kRelationCountV2` rises from 19 to 25.
- `UnknownEffect` allows `MAY`/`UNKNOWN` only; no domain strengthens `MAY` to
  `MUST` or derives `MUST_NOT`.
- `SoundnessCoverage(complete=1)` is produced only from the absence of a
  positively-computed `CoverageGap`, never from a bare empty result.
- Every derived fact carries a finite witness path; witness rule IDs are
  durable and added to `rules.v2` in `RuleRegistry` order.
- Installed public headers expose no Soufflé/LLVM/SVF native types; RTTI and
  exceptions stay disabled in VERITAS code (the runner/functors are the only
  `-frtti -fexceptions` islands, unchanged).
- The build stays green: `RelationSchemaManifestTest` and
  `RuleRegistryManifestTest` must pass after every registry change.

---

## File Structure

| File | Responsibility |
| --- | --- |
| `include/veritas/facts/RelationSchema.h` | `RelationId` ordinals + `kRelationCountV2` |
| `src/facts/RelationSchema.cpp` | authoritative `relations.v2` registry rows |
| `logic/schema/relations.v2.manifest` | Datalog-side schema mirror |
| `logic/schema/relations.v2.dl` | Soufflé `.decl`/`.input` for the schema |
| `src/facts/RuleRegistry.cpp` | authoritative `rules.v2` registry |
| `logic/common/rules.v2.manifest` | Datalog-side rule mirror |
| `logic/memory_effects/may_read.v2.dl` | MayRead bundle |
| `logic/flow/global_flow.v2.dl` + `bundle.manifest` | GlobalFlow bundle |
| `logic/effects/unknown_effect.v2.dl` + `bundle.manifest` | UnknownEffect bundle |
| `logic/coverage/soundness_coverage.v2.dl` + `bundle.manifest` | SoundnessCoverage bundle |
| `cmake/VeritasSouffle.cmake` | `veritas_generate_souffle_program` calls + provenance paths |
| `src/wpa/SouffleRunner.cpp` | `--component` dispatch to the new bundles |
| `src/wpa/CppRuleEvaluator.cpp` | C++ conformance oracle for the four domains |
| `src/wpa/WpaInputMaterializer.cpp` | emit `LocalFlow`/`ParameterFlow`/`ReturnFlow`/`UnsupportedFeature` |
| `tests/qualification/wpa/*` | differential corpus + conformance cases |

---

### Task 1: Extend the `relations.v2` and `rules.v2` Registries

**Files:**
- Modify: `include/veritas/facts/RelationSchema.h`
- Modify: `src/facts/RelationSchema.cpp`
- Modify: `logic/schema/relations.v2.manifest`
- Modify: `logic/schema/relations.v2.dl`
- Modify: `src/facts/RuleRegistry.cpp`
- Modify: `logic/common/rules.v2.manifest`

**Interfaces:**
- Produces six new `RelationId` values: `kMayRead`, `kSupportMayRead`,
  `kGlobalFlow`, `kSupportGlobalFlow`, `kUnknownEffect`, `kSoundnessCoverage`
  (append after `kSupportMayWrite`, before the count).
- Produces `kRelationCountV2 = 25`.
- Produces the sixteen new witness rule IDs listed in Task 2–5 (append after
  `wpa.memory.may_write.transitive.v2`): three `wpa.memory.may_read.*`, five
  `wpa.flow.global.*`, four `wpa.effect.unknown.*`, and four
  `wpa.coverage.*`.

- [ ] **Step 1: Write the failing registry-mirror assertions**

Add to `RelationSchemaManifestTest` a check that the manifest declares exactly
25 relations and that `MayRead`, `SupportMayRead`, `GlobalFlow`,
`SupportGlobalFlow`, `UnknownEffect`, and `SoundnessCoverage` each exist with
their spec section 6 column shape. Add to `RuleRegistryManifestTest` a check
that `wpa.memory.may_read.direct.v2`, `wpa.flow.global.local.v2`,
`wpa.effect.unknown.call.v2`, and `wpa.coverage.complete.v2` are present.

- [ ] **Step 2: Run the mirror tests to confirm they fail**

Run:
```bash
cmake --build --preset default
ctest --test-dir build -R "RelationSchemaManifestTest|RuleRegistryManifestTest" --output-on-failure
```
Expected: FAIL — the manifest and registry do not yet declare the relations.

- [ ] **Step 3: Append the `RelationId` ordinals**

In `include/veritas/facts/RelationSchema.h`, append after `kSupportMayWrite`:

```cpp
  kMayRead,
  kSupportMayRead,
  kGlobalFlow,
  kSupportGlobalFlow,
  kUnknownEffect,
  kSoundnessCoverage,
```

and change `kRelationCountV2` to `25`.

- [ ] **Step 4: Add the registry rows**

In `src/facts/RelationSchema.cpp`, append six rows mirroring the spec section 6
table. `MayRead`/`SupportMayRead` copy the `MayWrite` column shape;
`GlobalFlow`/`SupportGlobalFlow` use `{source_id kValueId, sink_id kValueId,
epistemic kEpistemic}` with `WithoutMustNot()`; `UnknownEffect` uses
`{function_id kFunctionId, subject kString, reason kString, epistemic
kEpistemic}` with a `MayUnknownOnly()` helper (add it beside `WithoutMustNot()`);
`SoundnessCoverage` uses `{scope_id kString, coverage_kind kString, complete
kUint64, epistemic kEpistemic}` with `{kMust, kUnknown}`.

- [ ] **Step 5: Mirror the schema in Datalog**

In `logic/schema/relations.v2.manifest`, append six `<TAB>`-separated rows in
the same order (columns, ownership, allowed epistemic). In
`logic/schema/relations.v2.dl`, add the corresponding `.decl`/`.input`
declarations for `MayRead`, `SupportMayRead`, `GlobalFlow`,
`SupportGlobalFlow`, `UnknownEffect`, and `SoundnessCoverage`, plus the
`LocalFlow`, `ParameterFlow`, `ReturnFlow`, `ModeledEffect`, and
`UnsupportedFeature` `.decl`/`.input` lines they consume (these five are
registered but not yet declared for Soufflé input).

- [ ] **Step 6: Append the witness rule IDs**

In `src/facts/RuleRegistry.cpp` and `logic/common/rules.v2.manifest`, append
the nine rule IDs with their priority and derived relation (the direct rules
get priority 10, support 20, transitive 30; `wpa.coverage.*` uses 10/20 and
`wpa.effect.unknown.*` uses 10/30). Keep the rule IDs in the exact strings the
bundles will cite.

- [ ] **Step 7: Run the mirror tests to confirm they pass**

Run the Step 2 command. Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add include/veritas/facts/RelationSchema.h src/facts/RelationSchema.cpp \
  logic/schema/relations.v2.manifest logic/schema/relations.v2.dl \
  src/facts/RuleRegistry.cpp logic/common/rules.v2.manifest
git commit -m "feat(M10A): register MayRead, GlobalFlow, UnknownEffect, SoundnessCoverage relations"
```

---

### Task 2: The MayRead Domain

**Files:**
- Create: `logic/memory_effects/may_read.v2.dl`
- Modify: `cmake/VeritasSouffle.cmake`
- Modify: `src/wpa/SouffleRunner.cpp`
- Modify: `src/wpa/CppRuleEvaluator.cpp`
- Test: `tests/qualification/wpa/WpaDifferentialQualificationTest.cpp`

**Interfaces:**
- Consumes: `DirectRead`, `DirectCall`, `SupportMayRead`, the `*Map` relations.
- Produces: `MayRead(function_id, memory_id, epistemic)` with rule IDs
  `wpa.memory.may_read.direct.v2`, `wpa.memory.may_read.support.v2`,
  `wpa.memory.may_read.transitive.v2`.

- [ ] **Step 1: Write the failing differential case**

Add a `MayRead` input permutation to the `WpaDifferentialQualificationTest`
parameter set: a self-recursive reader and a two-function read-through-call
chain, asserting the C++ oracle and Soufflé produce identical canonical
`MayRead` facts with the same epistemic weakening.

- [ ] **Step 2: Run it to confirm failure**

Run: `ctest --test-dir build -R WpaDifferentialQualificationTest --output-on-failure`
Expected: FAIL — no `MayRead` rule is evaluated yet.

- [ ] **Step 3: Write the Datalog bundle**

Create `logic/memory_effects/may_read.v2.dl` as the exact mirror of
`logic/memory_effects/may_write.v2.dl` with these substitutions: the IDB is
`MayRead`, the direct rule reads `DirectRead`, the transitive and support rules
compose `DirectCall` with `MayRead`/`SupportMayRead`, and the three witness
rule IDs are `wpa.memory.may_read.direct.v2`,
`wpa.memory.may_read.support.v2`, `wpa.memory.may_read.transitive.v2`. Key and
witness rules mirror `may_write.v2.dl` with `DirectRead`/`MayRead` names.

- [ ] **Step 4: Compile and dispatch the bundle**

In `cmake/VeritasSouffle.cmake`, add:
```cmake
veritas_generate_souffle_program(MayReadV2 veritas_mayread v2_mayread
  ${CMAKE_SOURCE_DIR}/logic/memory_effects/may_read.v2.dl)
```
link `veritas_mayread` into `veritas_souffle_runner`, add its generated path to
`souffle-provenance.json`, and in `SouffleRunner.cpp` add the `--component`
case that dispatches `may_read` to the `MayReadV2` program.

- [ ] **Step 5: Extend the C++ oracle**

In `src/wpa/CppRuleEvaluator.cpp`, generalize the existing
`ReachableCall`/`MayWrite` join loop so `MayRead` is a third instantiation of
the memory domain with `DirectRead` as the base relation and `SupportMayRead`
as support. Reuse the existing `Weaken` helper and `kMayRead`/`kSupportMayRead`
relation ids from Task 1.

- [ ] **Step 6: Run the differential case to confirm it passes**

Run the Step 2 command. Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add logic/memory_effects/may_read.v2.dl cmake/VeritasSouffle.cmake \
  src/wpa/SouffleRunner.cpp src/wpa/CppRuleEvaluator.cpp \
  tests/qualification/wpa/WpaDifferentialQualificationTest.cpp
git commit -m "feat(M10A): add MayRead recursive domain"
```

---

### Task 3: The GlobalFlow Domain

**Files:**
- Create: `logic/flow/global_flow.v2.dl`
- Create: `logic/flow/bundle.manifest`
- Modify: `cmake/VeritasSouffle.cmake`
- Modify: `src/wpa/SouffleRunner.cpp`
- Modify: `src/wpa/CppRuleEvaluator.cpp`
- Modify: `src/wpa/WpaInputMaterializer.cpp`
- Test: `tests/qualification/wpa/WpaDifferentialQualificationTest.cpp`

**Interfaces:**
- Consumes: `LocalFlow`, `ParameterFlow`, `ReturnFlow`, `SupportGlobalFlow`.
- Produces: `GlobalFlow(source_id, sink_id, epistemic)` with rule IDs
  `wpa.flow.global.local.v2`, `wpa.flow.global.parameter.v2`,
  `wpa.flow.global.return.v2`, `wpa.flow.global.transitive.v2`,
  `wpa.flow.global.support.v2`.

- [ ] **Step 1: Write the failing differential case**

Add a three-function chain to the differential corpus: a local flow in `A`,
`A` calling `B` (parameter flow), `B` returning into `C`, and a transitive
`A → C` conclusion. Assert both engines derive the same `GlobalFlow` closure
and that a `MUST` caller over a `MAY` callee weakens to `MAY`.

- [ ] **Step 2: Run it to confirm failure**

Run: `ctest --test-dir build -R WpaDifferentialQualificationTest --output-on-failure`
Expected: FAIL — `GlobalFlow` is not evaluated.

- [ ] **Step 3: Materialize the flow EDB**

In `src/wpa/WpaInputMaterializer.cpp`, emit `LocalFlow`, `ParameterFlow`, and
`ReturnFlow` rows from the normalized value flows and call bindings already in
the component input. `LocalFlow` projects `function_id`; `ParameterFlow` maps
an actual argument value to the callee's formal value at the call site;
`ReturnFlow` maps the callee return value to the caller's result value. Use the
same `EncodeSemanticRow` path as `DirectCall`/`DirectWrite`.

- [ ] **Step 4: Write the Datalog bundle**

Create `logic/flow/global_flow.v2.dl` declaring:
```text
.decl GlobalFlow(source_id:ValueId, sink_id:ValueId, epistemic:Epistemic)
```
with the three base rules over `LocalFlow`/`ParameterFlow`/`ReturnFlow`, the
transitive rule over `GlobalFlow × GlobalFlow`, and the support rule over
`GlobalFlow × SupportGlobalFlow`, each weakening with `WeakenEpistemic`. Emit
`GlobalFlowKey`, `LocalFlowKey`, `ParameterFlowKey`, `ReturnFlowKey`,
`SupportGlobalFlowKey`, and the five `Witness` rule groups with the rule IDs
above. Create `logic/flow/bundle.manifest` declaring `component=flow`,
`entry=global_flow.v2.dl`, the input/output relations, and the five rules.

- [ ] **Step 5: Compile and dispatch the bundle**

Mirror Task 2 Step 4 for `GlobalFlowV2`/`v2_globalflow`, linking it into the
runner and dispatching the `flow` component.

- [ ] **Step 6: Extend the C++ oracle**

In `src/wpa/CppRuleEvaluator.cpp`, implement `GlobalFlow` as a fixed-point
transitive closure over the three base edge kinds, using `ValueId` (not
`FunctionId`) endpoints and the `Weaken` helper. Seed from `LocalFlow`,
`ParameterFlow`, and `ReturnFlow`; iterate until no new pair is added; cite the
correct rule ID per seed/step.

- [ ] **Step 7: Run the differential case to confirm it passes**

Run the Step 2 command. Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add logic/flow/global_flow.v2.dl logic/flow/bundle.manifest \
  cmake/VeritasSouffle.cmake src/wpa/SouffleRunner.cpp \
  src/wpa/CppRuleEvaluator.cpp src/wpa/WpaInputMaterializer.cpp \
  tests/qualification/wpa/WpaDifferentialQualificationTest.cpp
git commit -m "feat(M10A): add GlobalFlow recursive domain"
```

---

### Task 4: The UnknownEffect Domain

**Files:**
- Create: `logic/effects/unknown_effect.v2.dl`
- Create: `logic/effects/bundle.manifest`
- Modify: `cmake/VeritasSouffle.cmake`
- Modify: `src/wpa/SouffleRunner.cpp`
- Modify: `src/wpa/CppRuleEvaluator.cpp`
- Modify: `src/wpa/WpaInputMaterializer.cpp`
- Test: `tests/qualification/wpa/WpaDifferentialQualificationTest.cpp`

**Interfaces:**
- Consumes: `UnknownCall`, `DirectCall`, `ModeledEffect`, `UnsupportedFeature`.
- Produces: `UnknownEffect(function_id, subject, reason, epistemic)` with rule
  IDs `wpa.effect.unknown.call.v2`, `wpa.effect.unknown.external.v2`,
  `wpa.effect.unknown.feature.v2`, `wpa.effect.unknown.transitive.v2`.

- [ ] **Step 1: Write the failing differential case**

Add an unmodeled external (`vendor_validate` with `DirectCall` dispatch
`EXTERNAL` and no `ModeledEffect`) and an `UnknownCall`, asserting both engines
emit `unmodeled_external` and `unresolved_indirect_call` respectively, and that
a caller of the unmodeled function inherits the unknown up the call graph.

- [ ] **Step 2: Run it to confirm failure**

Run: `ctest --test-dir build -R WpaDifferentialQualificationTest --output-on-failure`
Expected: FAIL — `UnknownEffect` is not evaluated.

- [ ] **Step 3: Materialize the unsupported-feature EDB**

In `src/wpa/WpaInputMaterializer.cpp`, emit `UnsupportedFeature(node_id,
feature_kind, soundness_policy)` rows from the component's `NormalizedUnknown`
records, and a `NodeFunction` helper relation binding each `node_id` to its
owning `FunctionId`.

- [ ] **Step 4: Write the Datalog bundle**

Create `logic/effects/unknown_effect.v2.dl` with the `ModeledFor` helper (a
function `g` with a `ModeledEffect` row), the three source rules from the spec
section 7.3 (`UnknownCall`, `DirectCall` with `EXTERNAL` dispatch negated by
`ModeledFor`, `UnsupportedFeature` joined through `NodeFunction`), and the
transitive rule composing `DirectCall` with `UnknownEffect`. Emit the four
`Witness` rule groups and the `UnknownEffectKey` helper. Create
`logic/effects/bundle.manifest` accordingly.

- [ ] **Step 5: Compile and dispatch the bundle**

Mirror Task 2 Step 4 for `UnknownEffectV2`/`v2_unknown`, dispatching the
`effects` component.

- [ ] **Step 6: Extend the C++ oracle**

In `src/wpa/CppRuleEvaluator.cpp`, implement `UnknownEffect` with the same
sources and the upward call-graph fixpoint, using a `ModeledFor` set built from
`ModeledEffect` rows and rejecting any `MUST`/`MUST_NOT` epistemic (assert the
`MayUnknownOnly` domain).

- [ ] **Step 7: Run the differential case to confirm it passes**

Run the Step 2 command. Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add logic/effects/unknown_effect.v2.dl logic/effects/bundle.manifest \
  cmake/VeritasSouffle.cmake src/wpa/SouffleRunner.cpp \
  src/wpa/CppRuleEvaluator.cpp src/wpa/WpaInputMaterializer.cpp \
  tests/qualification/wpa/WpaDifferentialQualificationTest.cpp
git commit -m "feat(M10A): add UnknownEffect recursive domain"
```

---

### Task 5: The SoundnessCoverage Domain

**Files:**
- Create: `logic/coverage/soundness_coverage.v2.dl`
- Create: `logic/coverage/bundle.manifest`
- Modify: `cmake/VeritasSouffle.cmake`
- Modify: `src/wpa/SouffleRunner.cpp`
- Modify: `src/wpa/CppRuleEvaluator.cpp`
- Test: `tests/qualification/wpa/WpaDifferentialQualificationTest.cpp`

**Interfaces:**
- Consumes: `UnknownEffect`, `DirectCall`, and the component's scope-enumeration
  helper `ScopeFunction`/`KnownScope`.
- Produces: `SoundnessCoverage(scope_id, coverage_kind, complete, epistemic)`
  with rule IDs `wpa.coverage.gap.own.v2`, `wpa.coverage.gap.transitive.v2`,
  `wpa.coverage.complete.v2`, `wpa.coverage.incomplete.v2`.

- [ ] **Step 1: Write the failing differential case**

Add two scopes: one containing an `UnknownEffect` (expect `complete=0, MUST`)
and one with no unknown (expect `complete=1, MUST`). Assert that a bare empty
scope enumeration with no `KnownScope` row never yields `complete=1`.

- [ ] **Step 2: Run it to confirm failure**

Run: `ctest --test-dir build -R WpaDifferentialQualificationTest --output-on-failure`
Expected: FAIL — `SoundnessCoverage` is not evaluated.

- [ ] **Step 3: Write the Datalog bundle**

Create `logic/coverage/soundness_coverage.v2.dl` with the two-stratum
computation from the spec section 7.4: a positive `CoverageGap` stratum
(own `UnknownEffect` and transitive through `DirectCall`), then the
`SoundnessCoverage` stratum using stratified negation over `CoverageGap` for
the `complete=1` rule and the positive gap for `complete=0`. Emit the four
`Witness` rule groups. Create `logic/coverage/bundle.manifest` with
`component=coverage`.

- [ ] **Step 4: Compile and dispatch the bundle**

Mirror Task 2 Step 4 for `SoundnessCoverageV2`/`v2_coverage`, dispatching the
`coverage` component.

- [ ] **Step 5: Extend the C++ oracle**

In `src/wpa/CppRuleEvaluator.cpp`, implement the two-stratum logic in C++: first
collect `CoverageGap` scopes from `UnknownEffect` (own and transitively through
`DirectCall`), then emit `SoundnessCoverage` rows, only asserting `complete=1`
for `KnownScope` scopes with no gap.

- [ ] **Step 6: Run the differential case to confirm it passes**

Run the Step 2 command. Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add logic/coverage/soundness_coverage.v2.dl logic/coverage/bundle.manifest \
  cmake/VeritasSouffle.cmake src/wpa/SouffleRunner.cpp \
  src/wpa/CppRuleEvaluator.cpp \
  tests/qualification/wpa/WpaDifferentialQualificationTest.cpp
git commit -m "feat(M10A): add SoundnessCoverage recursive domain"
```

---

### Task 6: Qualify the Four Domains Against the Corpus

**Files:**
- Modify: `tests/qualification/wpa/WpaDifferentialQualificationTest.cpp`
- Modify: `tests/qualification/wpa/WpaDeterminismQualificationTest.cpp`

**Interfaces:**
- Verifies: engine agreement and determinism for all four domains over the
  extended `wpa-qualification` corpus (direct, transitive, support, recursive,
  epistemic-weakening, unknown-propagation, and gap-present/absent shapes).

- [ ] **Step 1: Extend the corpus permutations**

Add the remaining spec section 10 corpus shapes to both test files: a mutual
recursion for `MayRead` and `GlobalFlow`, an `UnknownEffect` propagation chain,
and the `SoundnessCoverage` gap-present/absent pair, each asserted under
insertion-order reversal and repeated clean stores.

- [ ] **Step 2: Run the full qualification suite**

Run:
```bash
ctest --test-dir build -L wpa-qualification --output-on-failure
```
Expected: PASS with no `GTEST_SKIP` (a skip still reports passed to CTest, so
confirm the case bodies actually executed).

- [ ] **Step 3: Commit**

```bash
git add tests/qualification/wpa
git commit -m "test(M10A): qualify the four recursive domains"
```

---

### Task 7: Clean Build, Full Suite, and Final Branch State

**Files:** verify only.

- [ ] **Step 1: Clean build**

```bash
rm -rf build
cmake --preset default -DLLVM_PROJECT_BUILD_DIR=/path/to/llvm-project/build
cmake --build --preset default
```
Expected: zero errors; `veritas-souffle-worker` and the four generated bundles
compile.

- [ ] **Step 2: Full repository suite**

```bash
ctest --test-dir build --output-on-failure
```
Expected: 100% pass, including `RelationSchemaManifestTest`,
`RuleRegistryManifestTest`, `WpaExecutorConformanceTest`, and the differential
corpus.

- [ ] **Step 3: Formatting, license, and branch-state checks**

```bash
git diff --check main...HEAD
git status --porcelain
git diff --stat main...HEAD
git log --oneline main..HEAD
```
Inspect every new `.dl`, `.manifest`, C++, and CMake file for the Apache-2.0
header (`.dl` and `.manifest` are comment-capable; `.manifest` files are
already headed). Expected: clean worktree, only the reviewed M10A changes.

- [ ] **Step 4: Commit any final fixes and push the branch for review**

No further commit is expected if all prior tasks were clean.
