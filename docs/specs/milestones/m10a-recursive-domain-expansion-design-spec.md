# M10A Recursive Domain Expansion Design Spec

**Status:** Approved
**Milestone:** M10A
**Depends on:** M8R.3 relational WPA projection, M8R.4 production Soufflé executor, M8R.5 qualification corpus, M9 fact/provenance store
**Feeds:** M10B Evidence Builder input APIs, M10C Evidence IR semantic model

---

# 1. Purpose

M10A expands the compiled-Soufflé recursive WPA with four additional global
domains: `MayRead`, `GlobalFlow`, `UnknownEffect`, and `SoundnessCoverage`.
These are the interprocedural, whole-program facts that a local function
summary cannot by itself establish. M10B's `EvidenceQueryService` then queries
them — together with the durable M9 fact/witness store and the M6 thin CPG —
to produce the completeness-aware `EvidenceBuildInput` for the buffer-overflow
demonstration.

M10A does not introduce a new analysis engine, a new durable IR, or a new
evidence representation. It adds four rule bundles to the existing
Soufflé/C++-oracle WPA that M8R established, following the exact pattern of the
delivered `ReachableCall` and `MayWrite` domains.

M10A is a separate, prerequisite milestone. It is tracked independently of
M10B and must be qualified before M10B implementation begins.

---

# 2. Scope and Boundaries

## 2.1 In scope

- Four new recursive IDB relations: `MayRead`, `GlobalFlow`, `UnknownEffect`,
  `SoundnessCoverage`.
- Their Datalog derivations, semantic-key encodings, and witness/provenance
  rules.
- Their typed `relations.v2` registry entries (column specs, ownership,
  allowed epistemic states) and `RelationId` ordinals.
- Their successor-SCC support relations.
- Their C++ conformance-oracle counterparts and the differential
  qualification cases that prove engine agreement.
- Their external-function model inputs (via the existing `ModeledEffect`
  relation) where a model asserts a read, write, flow, or unknown effect.

## 2.2 Out of scope

- Value-range inference (for example, `packet.length ∈ [0, 65535]`),
  object-capacity derivation (`dest` has capacity 2048), and dominating-bounds
  check computation. These are **local** facts. Their natural home is the
  Function Summary IR range component and the local CFG analysis, not the
  recursive WPA. See section 9.
- The `EvidenceQueryService`, `EvidenceBuildInput`, `ClaimSeed`, or any
  Evidence IR type. M10B and M10C own those.
- Context-sensitive or path-sensitive points-to; a Soufflé-native points-to
  engine; or any change to SVF's role as the V1 points-to authority.
- Any change to the durable `summary.v2`, `relations.v2`, or `wpa-run.v1`
  contract version numbers already delivered by M8R.

---

# 3. Decisions

1. The four domains are added as additional Soufflé rule bundles over the
   existing `relations.v2` EDB. No new EDB relation is introduced except the
   two successor-SCC support relations each recursive domain requires.
2. Each domain has exactly one IDB relation, a semantic-key helper relation,
   and a set of `Witness` rules whose rule IDs mirror the delivered
   `wpa.*.v2` naming convention.
3. The C++ conformance engine remains a differential oracle, never a normal
   production owner. `WpaExecutorConformanceTest.EnginesProduceSameCanonicalFacts`
   and the `wpa-qualification` differential corpus are extended, not replaced.
4. Every derived fact retains its epistemic state. None of the four domains
   strengthens `MAY` to `MUST` or converts an unknown into a `MUST_NOT`.
5. `GlobalFlow` is a sound may-flow closure; it does not perform
   context-sensitive matching. `ParameterFlow` and `ReturnFlow` already carry
   the call-site binding, so the closure does not need call-site columns.
6. `UnknownEffect` is the single typed surface for "the analysis cannot soundly
   model this effect". M10B's `GetUnknowns` and the opaque-validator fixture
   read exactly this relation; nothing is represented as a silently dropped
   call.
7. `SoundnessCoverage` is the only domain that may feed a registered
   closed-world negative-evidence rule. It is derived only from the absence of
   a positively-computed coverage gap (section 7.4), never from a bare empty
   result.

---

# 4. Non-goals

This milestone does not:

- compute value ranges, capacities, or dominating-check facts;
- change the durable relation, summary, or run schema versions;
- add nullable or sentinel columns to any relation;
- publish Soufflé-native tuple identities or engine-specific records;
- make LLVM-level facts durable;
- move recursive analysis into M10B's query layer;
- implement context-sensitive flow or points-to;
- silently fall back from Soufflé to C++.

---

# 5. Context: the Existing Rule-Bundle Pattern

M10A reuses the delivered pattern in
`logic/reachability/reachability.v2.dl` and
`logic/memory_effects/may_write.v2.dl`. A domain consists of:

1. a `.decl` IDB relation plus an `.output` to a canonical CSV;
2. three derivations — a base/direct rule, a transitive rule that composes a
   `DirectCall` edge with the domain's own result, and a successor-SCC support
   rule that cites the component's successor facts;
3. a semantic-key helper relation that reconstructs a stable content hash from
   the dense ids via the `@veritas_key_*` functors;
4. `Witness(result_key, rule_id, derivation_key, input_key, ordinal)` rules
   that give every published fact a finite path to rooted inputs.

The dense/stable `*Map` relations, the `WeakenEpistemic` helper, and the
`DirectCall`/`DirectRead`/`DirectWrite`/`Alias`/flow EDB are already delivered
and are inputs to these domains. `GlobalFlow`, `UnknownEffect`, and
`SoundnessCoverage` additionally consume the flow and model EDB relations that
exist in the registry but are not yet referenced by any rule bundle:
`LocalFlow`, `ParameterFlow`, `ReturnFlow`, `ModeledEffect`, and
`UnsupportedFeature`.

---

# 6. Relation Registry Additions

The `relations.v2` registry gains six entries: four IDB relations and two EDB
support relations. Column domains use the existing `ColumnDomain` values;
`kString` carries stable reason/kind codes, never semantic identity.

| Relation | Ownership | Columns | Allowed epistemic |
| --- | --- | --- | --- |
| `MayRead` | IDB | `function_id` `kFunctionId`, `memory_id` `kMemoryId`, `epistemic` `kEpistemic` | `MUST`, `MAY`, `INFERRED`, `ASSUMED`, `UNKNOWN` (no `MUST_NOT`) |
| `SupportMayRead` | EDB | `function_id` `kFunctionId`, `memory_id` `kMemoryId`, `epistemic` `kEpistemic` | as `MayRead` |
| `GlobalFlow` | IDB | `source_id` `kValueId`, `sink_id` `kValueId`, `epistemic` `kEpistemic` | no `MUST_NOT` |
| `UnknownEffect` | IDB | `function_id` `kFunctionId`, `subject` `kString`, `reason` `kString`, `epistemic` `kEpistemic` | `MAY`, `UNKNOWN` only |
| `SoundnessCoverage` | IDB | `scope_id` `kString`, `coverage_kind` `kString`, `complete` `kUint64`, `epistemic` `kEpistemic` | `MUST`, `UNKNOWN` |
| `SupportGlobalFlow` | EDB | `source_id` `kValueId`, `sink_id` `kValueId`, `epistemic` `kEpistemic` | no `MUST_NOT` |

`UnknownEffect` and `SoundnessCoverage` are the only new relations that
deviate from a pure "mirror an existing domain" shape; both are motivated in
section 7. `UnknownEffect` is the only relation whose allowed states exclude
`MUST`; an unknown is never a proof.

Support relations mirror their IDB counterpart's columns and are EDB: a
component cites successor facts and never claims ownership of them. `MayRead`
needs `SupportMayRead`; `GlobalFlow` needs `SupportGlobalFlow` because its
closure can extend through a successor SCC's value-flow summary. `UnknownEffect`
and `SoundnessCoverage` do not require support relations in V1 — their
propagation is call-graph-upward and their scopes are function-local to the
component being evaluated.

---

# 7. Domain Specifications

## 7.1 MayRead

`MayRead` is the transitive may-read set of a function: the memory locations
whose contents the function may read, directly or through any callee.

```text
MayRead(function_id, memory_id, epistemic)
```

Derivations (mirroring `MayWrite`):

- **direct** — a member's own read, at its own warrant; the byte-range columns
  of `DirectRead` are projected away:

  ```text
  MayRead(f, x, e) :- DirectRead(f, x, _, _, _, e).
  ```

- **transitive** — a read reached through a call, weakening the epistemic state:

  ```text
  MayRead(f, x, e) :-
    DirectCall(_, f, g, _, e1),
    MayRead(g, x, e2),
    WeakenEpistemic(e1, e2, e).
  ```

- **support** — a read reached through a successor SCC's result:

  ```text
  MayRead(f, x, e) :-
    DirectCall(_, f, g, _, e1),
    SupportMayRead(g, x, e2),
    WeakenEpistemic(e1, e2, e).
  ```

Witness rule IDs: `wpa.memory.may_read.direct.v2`,
`wpa.memory.may_read.transitive.v2`, `wpa.memory.may_read.support.v2`.

`MayRead` carries no byte range for the same reason `MayWrite` carries none:
memory identity excludes the range, so "does `f` read object `o`?" has a
single answer. A modeled external read (section 8) seeds the direct rule.

## 7.2 GlobalFlow

`GlobalFlow` is the recursive may-flow closure over value identity. It unifies
three already-registered EDB edges into one transitive relation:

```text
GlobalFlow(source_id, sink_id, epistemic)
```

Base edges:

```text
GlobalFlow(s, d, e) :- LocalFlow(_, s, d, _, e).
GlobalFlow(a, f, e) :- ParameterFlow(_, a, f, e).
GlobalFlow(r, c, e) :- ReturnFlow(_, r, c, e).
```

Transitive closure:

```text
GlobalFlow(s, d, e) :-
  GlobalFlow(s, m, e1),
  GlobalFlow(m, d, e2),
  WeakenEpistemic(e1, e2, e).
```

Successor support (a successor SCC's flow summary extends the closure):

```text
GlobalFlow(s, d, e) :-
  GlobalFlow(s, m, e1),
  SupportGlobalFlow(m, d, e2),
  WeakenEpistemic(e1, e2, e).
```

Witness rule IDs: `wpa.flow.global.local.v2`, `wpa.flow.global.parameter.v2`,
`wpa.flow.global.return.v2`, `wpa.flow.global.transitive.v2`,
`wpa.flow.global.support.v2`.

`LocalFlow`'s `function_id` is projected away: value identity is stable across
functions, and the call-site context needed for the may-flow conclusion is
already captured by the `ParameterFlow`/`ReturnFlow` bindings. The result is
the whole-program "`packet.length` → decode argument → copy length → `memcpy`
size" chain that M10B's `GetValueFlow` reports.

## 7.3 UnknownEffect

`UnknownEffect` is the single typed surface for an effect the analysis cannot
soundly model. Every unresolved call, unmodeled external, or unsupported
feature becomes a row here rather than being silently dropped:

```text
UnknownEffect(function_id, subject, reason, epistemic)
```

- `function_id` — the function whose summary carries the unknown effect;
- `subject` — the subject of the unknown: a stable call-site key, an
  unmodeled callee symbol, or a stable `whole_function` marker;
- `reason` — a stable reason code from a fixed vocabulary:
  `unresolved_indirect_call`, `unmodeled_external`, `unsupported_feature`;
- `epistemic` — `MAY` or `UNKNOWN` only.

Sources:

```text
UnknownEffect(f, cs, reason, e) :- UnknownCall(cs, f, reason, e).
UnknownEffect(f, g, "unmodeled_external", MAY) :-
  DirectCall(cs, f, g, EXTERNAL, e),
  !ModeledFor(g).
```

`UnsupportedFeature(node_id, feature_kind, soundness_policy)` records an
analysis-wide soundness limitation whose `node_id` is a stable node reference,
not a function id. V1 folds it into `UnknownEffect` by resolving `node_id` to
its owning function through the component's node→function mapping, so the
unsupported feature becomes a function-scoped unknown:

```text
UnknownEffect(f, subj, "unsupported_feature", UNKNOWN) :-
  NodeFunction(f, node), UnsupportedFeature(node, subj, _).
```

`NodeFunction` is the component-local helper that binds a stable node to the
member function that owns it; its population is fixed in the implementation
plan.

Transitive propagation up the call graph:

```text
UnknownEffect(f, subj, r, e) :-
  DirectCall(_, f, g, _, e1),
  UnknownEffect(g, subj, r, e2),
  WeakenEpistemic(e1, e2, e).
```

`ModeledFor(g)` is a positive helper that holds when `g` has a `ModeledEffect`
row; the external-without-model rule uses stratified negation over it. A
function with an unknown effect therefore propagates that unknown to every
transitive caller, which is what lets M10B's opaque-validator fixture report an
unknown for `vendor_validate` at the sink rather than assuming a postcondition.

Witness rule IDs: `wpa.effect.unknown.call.v2`,
`wpa.effect.unknown.external.v2`, `wpa.effect.unknown.feature.v2`,
`wpa.effect.unknown.transitive.v2`.

## 7.4 SoundnessCoverage

`SoundnessCoverage` records whether a scope was soundly covered — whether the
analysis can prove it enumerated every path the scope admits without hitting an
unknown. It is the only domain that may feed a registered closed-world
negative-evidence rule.

```text
SoundnessCoverage(scope_id, coverage_kind, complete, epistemic)
```

- `scope_id` — a stable scope reference, for example the dominating-check
  scope keyed by sink and call site;
- `coverage_kind` — a stable kind code; V1 uses
  `dominating_check_absence`;
- `complete` — `1` when the scope is soundly covered, `0` when a coverage gap
  is positively detected;
- `epistemic` — `MUST` when the complete/incomplete conclusion is
  deterministically established, `UNKNOWN` otherwise.

Computation is two-stratum. First a positive gap relation:

```text
CoverageGap(scope_id, kind) :-
  ScopeFunction(scope_id, kind, f),
  UnknownEffect(f, _, _, _).
CoverageGap(scope_id, kind) :-
  ScopeFunction(scope_id, kind, f),
  DirectCall(_, f, g, _, e),
  UnknownEffect(g, _, _, _).
```

Then completeness via stratified negation: a scope is soundly covered exactly
when the scope is a known, fully-enumerated scope with no gap.

```text
SoundnessCoverage(scope, kind, 1, MUST) :-
  KnownScope(scope, kind), !CoverageGap(scope, kind).
SoundnessCoverage(scope, kind, 0, MUST) :-
  CoverageGap(scope, kind).
```

`ScopeFunction`/`KnownScope` are the positive scope-enumeration helpers
produced by the component from the claim scope; the exact column shape is
fixed in the implementation plan, but the semantic invariant is fixed here: a
`complete=1` row is **only** produced by the absence of a positively-computed
gap, and a bare empty result never manufactures coverage.

This is the certificate that M10B's
`evidence.closed_world.dominating_check_absence.v1` rule requires: a
complete-empty dominating-check query is eligible for negative evidence only
when its query provenance references a `SoundnessCoverage(complete=1, MUST)`
fact covering the scope.

Witness rule IDs: `wpa.coverage.gap.own.v2`,
`wpa.coverage.gap.transitive.v2`, `wpa.coverage.complete.v2`,
`wpa.coverage.incomplete.v2`.

---

# 8. External-Function Models

M10A consumes the delivered `ModeledEffect` EDB in two distinct ways, kept
disjoint by a single materialization rule:

- the materializer folds a model's read assertion into a `DirectRead` row, its
  write assertion into a `DirectWrite` row, and its flow assertion into a
  function-scoped `LocalFlow` row; the `MayRead` direct rule and the
  `GlobalFlow` base rule therefore cover modeled effects with no dedicated
  "modeled" rule;
- the materializer also emits the `ModeledEffect` row itself, so the
  `ModeledFor` helper in section 7.3 can distinguish a modeled external from an
  unmodeled one.

An unmodeled external (`vendor_validate`, no `ModeledEffect` row) is reported
as `unmodeled_external`; a modeled external (`memcpy`) is never, because its
effects arrive as ordinary `DirectRead`/`DirectWrite`/`LocalFlow` rows plus the
`ModeledEffect` row that satisfies `ModeledFor`.

Model inputs are the existing `ModelBundle`; M10A adds no model schema and no
new model vocabulary.

---

# 9. Deferred: Local Range, Capacity, and Dominating-Check Facts

The M10B buffer-overflow demonstration also requires three **local** facts that
the recursive WPA does not and should not compute:

```text
range(packet.length) = [0, 65535]      value-range inference
capacity(destination)  = 2048           object-capacity / array-size derivation
no dominating bounds check              CFG dominance over the sink
```

`RangeFact` and `DominatorSummaryFact` already exist as declared Summary IR
components (`proto/veritas/summary/v1/summary.proto`), but no producer
currently populates them, and no integer range inference exists anywhere in
the pipeline. These gaps are **not** part of M10A; they belong to the local
extraction and Summary IR layer (M5-era) and are tracked separately. M10B's
real-fixture query adapters assume those local facts are published as M9 facts
by the time M10B Task 3 begins; the tracking issue for that gap is filed
alongside this milestone.

---

# 10. Conformance and Qualification

Each domain is delivered with:

- a compiled-Soufflé implementation (the production path);
- a C++ conformance-oracle counterpart behind `CppConformanceExecutor`;
- differential cases in the `wpa-qualification` corpus proving the two engines
  publish the same canonical facts over byte-identical logical input.

The M8R.5 corpus is extended with, for each domain:

- the direct, transitive, and support derivations in isolation;
- a self-recursive and a mutually-recursive shape;
- an epistemic-weakening case (`MUST` caller over a `MAY` callee);
- an unknown/unsupported feature propagation case for `UnknownEffect`;
- a gap-positive and a gap-absent scope for `SoundnessCoverage`.

Engine agreement is guarded by the existing
`WpaExecutorConformanceTest.EnginesProduceSameCanonicalFacts` and
`WpaDifferentialQualificationTest.SouffleEqualsCppOracle`. No new engine, no
new fallback, and no change to the production/conformance split is introduced.

---

# 11. Acceptance Criteria

M10A is qualified when:

- all four domains and their two support relations are registered in
  `relations.v2` and pass `RelationSchemaManifestTest`;
- each domain's compiled-Soufflé result and C++ oracle agree on the extended
  `wpa-qualification` corpus;
- `UnknownEffect` reports `unmodeled_external` for an unmodeled callee and
  `unresolved_indirect_call` for an unresolved call, and propagates both up the
  call graph without epistemic strengthening;
- `GlobalFlow` yields the whole-program `packet.length → … → memcpy size`
  closure from `LocalFlow`/`ParameterFlow`/`ReturnFlow` inputs;
- `SoundnessCoverage` produces `complete=1` only from the absence of a
  positively-computed gap, and never from a bare empty result;
- every published fact resolves to a finite witness path and retains its
  epistemic state;
- the clean build, the full repository suite, formatting, and license checks
  pass.

---

# 12. Handoff to M10B

M10A publishes the four domains through the M9 fact/witness store exactly as
`ReachableCall` and `MayWrite` are published today. M10B's
`EvidenceQueryService` then:

- `GetValueFlow` reads `GlobalFlow` (plus the M6 thin CPG for path shape);
- `GetUnknowns` reads `UnknownEffect`;
- `GetDominatingChecks` consults `SoundnessCoverage` for the closed-world
  certificate when a complete-empty result is produced.

M10B performs no recursive reachability, alias analysis, range propagation, or
dominance computation of its own; those are local (section 9) or M10A-owned
(this section) inputs.
