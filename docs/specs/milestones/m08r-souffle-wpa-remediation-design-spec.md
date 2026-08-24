# M8R Souffle WPA Remediation Bridge Design Spec

**Status:** Approved target; implementation and qualification pending
**Milestone:** M8R.1-M8R.5
**Historical predecessor:** M8 (implemented; unchanged)
**Entry gate for:** M9
**Authoritative design:** [`m08r-souffle-wpa-architecture-refinement-design-spec.md`](m08r-souffle-wpa-architecture-refinement-design-spec.md)
**Implementation plan:** [`m08r-souffle-wpa-remediation-implementation-plan.md`](../../plans/milestones/m08r-souffle-wpa-remediation-implementation-plan.md)

---

# 1. Purpose and status

M8 is an immutable implemented milestone: it delivered a C++ recursive WPA
engine and an optional file-based Souffle comparison path. This specification
does not relabel that history. It defines the approved remediation bridge that
must be delivered after M8 and before M9.

The M8R target has five ordered gates. None is considered delivered merely
because it is described here. Implementation must fill the delivery record in
section 8 with reviewed commits and exact executable test labels. In
particular, this document does **not** claim that M8R.4 production Souffle has
shipped.

# 2. Architectural boundary and ownership

The durable WPA boundary is versioned Function Summary IR. `summary.v2`
preserves normalized calls, abstract memory, value flow, aliases, uncertainty,
models, dependencies, and provenance inputs. Detailed `relations.v2` rows are a
typed, run-local execution projection reconstructed from summaries; they are
not a fourth durable VERITAS IR.

Ownership is fixed as follows:

* Pinned SVF owns V1 `AndersenWaveDiff` points-to results, alias
  classification, SVFG construction, and indirect-call candidates.
* VERITAS owns stable semantic identity, normalization, abstract memory,
  uncertainty and negative information, schemas, run identity, witnesses, and
  publication.
* Compiled Souffle owns normal production recursive summary-level WPA.
* C++ consumes the same byte-identical engine-neutral
  `WpaLogicalComponentInput` only as the conformance oracle or when the operator
  explicitly selects `cpp-emergency`.

There is no automatic fallback from Souffle to C++. Production and C++
conformance/emergency executions use distinct valid envelopes, engine
identities, and `RunId` values.

# 3. Versioned contracts

The remediation introduces three linked contracts:

```text
summary.v2      durable normalized function semantics
relations.v2    typed run-local WPA execution relations
wpa-run.v1      immutable execution manifest plus separately mutable lifecycle state
```

Existing `summary.v1` artifacts remain byte-immutable and readable. Native
reanalysis never rewrites or republishes V1 bytes; after M8R.2 it emits
`summary.v2` only. A tagged V1 compatibility projection may supply explicit
legacy/unknown semantics to a V2 WPA run, but it cannot fabricate V2 precision.

Every run manifest identifies the revision, build variant, summary and relation
schemas, rule and model bundles, SVF and WPA configurations, engine, and exact
engine/toolchain identity. Every `EngineToolchainIdentity` covers the engine
identity plus a required canonical engine-specific provenance payload and hash.
For production Souffle, VERITAS parses the configured install-provenance
manifest, requires Souffle 2.5 at exact source revision
`5682a9f12e2668ecdd26348fe63cc508bc0fcf47`, hashes the configured executable,
and rejects any mismatch with the executable digest recorded by the manifest.
The Souffle payload includes the verified manifest identity/content digest,
source revision, executable digest, generated-bundle digest, and
generator/compiler/link toolchain provenance. A C++ conformance or
`cpp-emergency` payload instead records the exact C++ build identity and cannot
claim, reuse, or impersonate Souffle provenance. Until a separately qualified
upgrade retires the upstream ARM concurrency issue, generated Souffle programs
run with one evaluation thread.

Stable IDs are durable. Typed unsigned dense IDs are assigned per run for hot
joins and never escape their `AnalysisRun`. Missing mappings, duplicate dense
IDs, stable-ID conflicts, and cross-domain ID use fail validation.

Semantic value and epistemic state remain independent. All six platform states
(`MUST`, `MAY`, `MUST_NOT`, `INFERRED`, `ASSUMED`, `UNKNOWN`) and all four alias
kinds survive the boundary, including the distinct negative statement
`NO_ALIAS + MUST`. Unsupported values become explicit errors or explicit
unknown relations according to the versioned rule-bundle contract; they are
never silently dropped.

Memory identity is:

```text
MemoryLocation = AbstractObject + AccessPath + ByteRange
```

`RangeKind` is explicitly `KNOWN` or `UNKNOWN`. Known signed offsets and
unsigned sizes remain lossless, including known zero values. An unknown range
uses canonical zero payload cells with no inferred range meaning; half-known
ranges and non-zero unknown payloads are invalid.

# 4. Component execution and reuse

VERITAS owns call-graph construction and reverse-topological SCC scheduling.
Each SCC/domain execution materializes one canonical engine-neutral logical
input containing member identities and facts, stable/dense mappings, outgoing
calls, successor support facts, models, schemas, bundles, and semantic
configuration. Successor facts carry stable support-fact identities; a
component publishes only facts owned by its current SCC.

`LogicalInputHash` covers canonical semantic input but excludes revision,
`RunId`, engine identity, and tuple order. `FixpointHash` covers the complete
canonical result and selected witnesses. `ExternalHash` covers only
predecessor-visible semantics, so a witness-only change may alter
`FixpointHash` but not canonical fact/root IDs or `ExternalHash` and does not
schedule predecessors.

Successful immutable component results may be reused across revisions only
under the content-addressed key:

```text
(LogicalInputHash, EngineToolchainIdentity)
```

Lookup revalidates object content, rooted inputs, schemas, bundles, and exact
executor/toolchain provenance. Each analysis run records its own reference;
run histories are never merged, and C++ output can never satisfy a production
Souffle run.

# 5. Witness, failure, and publication contracts

Every published derived fact has a deterministic finite proof rooted in stable
input fact identities. Engines emit generic immediate witness edges over a
versioned, injective, type-tagged, length-prefixed semantic-key codec. The
canonicalizer rejects ambiguous raw-delimiter concatenation, orphaned inputs,
cycles, malformed keys, and witnesses that are not closed over the published
facts and rooted inputs. It selects a proof by derived-edge count, versioned
rule priority, then lexicographic stable input identity. Souffle interactive
provenance remains a debugging aid, not the durable witness protocol.

A component publishes only after execution, schema validation, stable-ID
reconstruction, duplicate checking, and witness closure all succeed. Missing
or incompatible compiled bundles, engine failure, timeout, resource exhaustion,
schema or identity error, and malformed witnesses produce no replacement
component. Durable diagnostics mark the run incomplete; the last successful
result remains queryable only as stale history.

M9 receives one immutable `AnalysisFactBatch` from a successful `WpaRunResult`.
The batch carries expected and completed component keys and hashes, rooted input
fact IDs, canonical facts, witnesses, and diagnostics. The Fact Bus rejects
expected/completed mismatch or an unrooted witness leaf. Multi-sink delivery is
idempotent at least once by canonical `(RunId, BatchId)` identity, records
per-sink progress, and permits retry without duplicate logical publication.
Canonical facts retain their witness-independent `MakeFact` IDs across the
handoff; M9 persists separate `(RunId, FactID)` occurrence and witness bindings
and never re-identifies them.

# 6. Ordered remediation gates

| Gate | Required result | Completion condition |
| --- | --- | --- |
| M8R.1 Semantic Fact Contract | `AnalysisRun`, relation registry, typed semantic values, stable/dense mapping, complete uncertainty, V1 adapters | Every supported value round-trips; identities do not collide; existing `summary.v1` artifacts remain immutable/readable; supported M8 fixtures remain semantically equivalent. |
| M8R.2 SVF and Memory Refinement | normalized indirect/virtual/callback targets, abstract objects/paths/ranges, alias kinds, models, native `summary.v2` | Stable `MAY` targets enter the graph; unknowns remain explicit; placeholders are not durable identity. |
| M8R.3 Relational WPA Projection | summary-to-EDB materializer, SCC inputs/support, manifests, `ReachableCall`/`MayWrite`, generic witnesses, matched engine inputs | Both engines consume byte-identical logical input and supported M8 semantics agree with finite rooted witnesses. |
| M8R.4 Production Souffle WPA | required compiled Souffle, exact provenance, production executor, incremental reuse/failure state, explicit emergency mode | Standard analysis uses Souffle; missing/incompatible Souffle fails; C++ is never selected implicitly. |
| M8R.5 Qualification and M9 Handoff | conformance/failure/performance corpus, `AnalysisFactBatch`, Fact Bus, synchronized docs and executable M9 gate | All ten M9 entry criteria pass exactly, with no missing, extra, disabled, skipped, failed, or errored tests. |

# 7. M9 entry criteria

M9 may begin only after a production-Souffle build proves all ten executable
criteria:

1. Existing `summary.v1` artifacts remain immutable and readable; native
   reanalysis emits only `summary.v2`, which preserves dispatch, abstract
   memory, alias kind, uncertainty, and provenance inputs.
2. SVF indirect-call candidates participate in the summary call graph.
3. Distinct unnamed values and allocations retain distinct stable identities.
4. `relations.v2` uses typed schemas and run-local dense IDs.
5. Compiled Souffle is the normal production recursive executor.
6. C++ uses byte-identical logical input under a distinct run envelope and
   passes the overlapping conformance corpus.
7. Every result has a finite rooted witness and the batch proves component
   completion and rooted-input closure.
8. Failed or incomplete runs cannot replace successful results.
9. Run identity includes every schema, bundle, configuration, engine, and exact
   toolchain input.
10. Architecture, milestone, README, and operational documentation agree.

The executable gate must reject missing or extra expected tests as well as
disabled, skipped, failed, or errored tests. A green subset or a documentation
assertion is not permission to start M9.

| Criterion | Required CTest label |
| ---: | --- |
| 1 | `summary-v2` |
| 2 | `indirect-calls` |
| 3 | `stable-identity` |
| 4 | `relations-v2` |
| 5 | `souffle-production` |
| 6 | `engine-conformance` |
| 7 | `witness-closure` |
| 8 | `failure-atomicity` |
| 9 | `run-identity` |
| 10 | `documentation-consistency` |

The gate implementation must record the exact expected test-name set behind
each label and compare it with executed JUnit membership. Labels alone are not
delivery evidence.

The implementation plan's
[Task 16](../../plans/milestones/m08r-souffle-wpa-remediation-implementation-plan.md#task-16-build-the-differential-determinism-migration-failure-and-performance-corpus)
defines the executable corpus and per-label membership authority; its
[Task 18](../../plans/milestones/m08r-souffle-wpa-remediation-implementation-plan.md#task-18-synchronize-architecturemilestones-and-enforce-the-m9-entry-gate)
enforces the documentation and M9-entry synchronization.

# 8. Delivery record (to be completed by implementation)

This table records target status only. Implementers must replace `TBD` with
reviewed evidence; documentation authors must not infer delivery from partial
work.

| Gate | Current status | Delivered commit | Exact executable test labels / gate evidence |
| --- | --- | --- | --- |
| M8R.1 | Pending | TBD by implementation | TBD by implementation |
| M8R.2 | Pending | TBD by implementation | TBD by implementation |
| M8R.3 | Pending | TBD by implementation | TBD by implementation |
| M8R.4 | Pending; not shipped | TBD by implementation | TBD by implementation |
| M8R.5 | Pending | TBD by implementation | `summary-v2`, `indirect-calls`, `stable-identity`, `relations-v2`, `souffle-production`, `engine-conformance`, `witness-closure`, `failure-atomicity`, `run-identity`, `documentation-consistency`; exact test membership and gate command TBD by implementation |

# 9. Future milestone topology

After M8R.5 qualifies the M9 handoff:

* M9 persists runs, canonical facts, rooted witnesses, diagnostics, hashes, and
  stale state; `explainFact` reads persisted witnesses instead of recomputing
  WPA facts.
* M10A expands recursive production domains with `MayRead`, `GlobalFlow`,
  `UnknownEffect`, and `SoundnessCoverage` plus independent models and
  conformance suites.
* M10B builds Evidence Builder APIs and the first security demo over M9 facts
  and M10A relations.
* M11 preserves the native Summary IR/execution path for external LLVM IR;
  M12 preserves the same trust boundary by storing external provider graphs and
  facts as separate epistemic-lowered SummaryDB projections that never become
  native WPA inputs.
* M13 evaluates a Souffle-native points-to/call-graph kernel against pinned SVF
  only under separately approved correctness, precision, performance, and
  model-coverage thresholds. M13 is independent of the M9-M12 critical path.
