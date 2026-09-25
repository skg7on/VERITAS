# M10B Sink Call-Site Resolution Amendment Design Spec

**Status:** Proposed — not yet applied
**Milestone:** M10B (amends §2 and §6 of the M10B design spec)
**Depends on:** an M10A run-bound value-range relation, for §4.3 and §4.4 only.
§4.1, §4.2, and §6 need nothing from M10A and can land independently.
**Feeds:** the M10B/M10C API-to-Evidence-IR test contract

---

# 1. Purpose and Status

This document **proposes** an amendment to
[M10B Evidence Builder Input APIs and First Demo Design Spec](m10b-evidence-builder-input-apis-demo-design-spec.md).

The M10B design spec and the companion
[API-to-Evidence-IR test contract](m10b-m10c-api-to-evidence-ir-test-design-spec.md)
are **not edited by this document**. They change only if this proposal is
approved. Nothing here is implementable ahead of §6, which is separable and does
not depend on M10A.

The proposal changes one thing: `veritas-query evidence overflow --sink memcpy`
resolves its buffer-overflow claim seed **per sink call site** instead of per
sink. Today it requires the sink to have exactly one owning function, so the
query is unusable as soon as two functions in the program call the sink.

# 2. The Current Contract, and Why the Guard Is Load-Bearing

`ResolveOverflowClaimSeed` (`include/veritas/evidence/OverflowClaimSeed.h`)
computes the seed's three refs from **two different scopes**:

| Ref | Derivation | Scope |
| --- | --- | --- |
| `sink_ref` | the terminal of the "exit" closure — CPG values that are the source of a `GlobalFlow` fact whose target is not a CPG node (l.126–180) | **whole program**; the caller is never consulted |
| `source_ref` | the root of that terminal's ancestor chain within the exit closure (l.182–205) | **whole program** |
| `subject_ref` | the memory object the sink's **calling function** writes, via its `kWrites` edges (l.207–232) | **one function** |

The seed is therefore coherent only when exactly one function calls the sink.
With several, it would pair a whole-program flow path with one function's write —
and the path need not even reach that function's call. The guard at l.216 is
load-bearing, not conservative caution.

It is also the first thing a real project hits. On a `leveldb` revision whose
analysis completes and publishes normally:

```console
$ veritas-query evidence overflow --sink memcpy --format json --db <store>
veritas-query: the memcpy call has no single owning function
```

The store's CPG holds one `llvm.memcpy.p0.p0.i64` node (node_kind 7) reached by
**167 may-call edges from 167 distinct source nodes**. The single sink anchor
resolves (l.120 passes); the owning function does not (l.216 fails).

## 2.1 The flow side is not sink-specific either

The `sink_ref` comment above l.126 says the exit closure is "the deepest CPG value
that leaves the analyzed code **into the unmodeled sink's argument list**". The
code tests only that the `GlobalFlow` target is absent from the CPG node set
(l.142). Those two are the same thing **only when the sink is the program's sole
non-CPG flow target.**

That is a fixture property, not a program property. The same store shows why:

| Observation | Value |
| --- | --- |
| CPG nodes of kind 7 (unmodeled external) | 348 |
| distinct external labels among them | 121 |
| `kFlowsTo` edges into any kind-7 node | 0 |

The sample labels include `_read`, `_write`, `_open`, `_mmap`, `_fsync`, `_stat`,
`std::string::find`, `std::string::rfind`, and `<unknown>`. Externals carry no
CPG parameter nodes (0 incoming `kFlowsTo` edges), so a value flowing into *any*
of them is an exit value under l.142 — `_write`'s operand every bit as much as
`sink`'s. The terminal is then chosen by `CanonicalMin` (l.180).

So with more than one unmodeled external in the program, `sink_ref` can name an
operand that **never reaches the sink at all**. The header's own caveat — "the
sink's size, destination, and source formals cannot be told apart by name; the
choice is therefore canonical, not semantic" — covers telling the sink's own
formals apart. It does not cover admitting another external's operands into the
candidate set, which is what the code does.

This matters more than the caller count: it is a correctness gap in the seed's
identity, not a usability limit. It is also invisible today, because the guard at
l.120 keeps the query on single-external fixtures where the two scopes coincide.

# 3. Why the Obvious Widenings Are Rejected

| Alternative | Why it is rejected |
| --- | --- |
| Take `CanonicalMin(callers)`, matching the `CanonicalMin` selection already used for terminals, roots, and subjects (l.180, l.205, l.232) | `subject_ref` is caller-dependent and feeds `finding_id` (l.234–244). Picking a canonical caller would attribute a whole-program path to an arbitrary one of 167 functions — and, worse, **adding an unrelated caller whose digest sorts lower would silently change the reported finding.** Deterministic for a fixed input, but non-monotonic under edits, which is worse than a refusal. |
| Fan out over every caller as-is | With no run-bound range or capacity fact, nothing distinguishes a genuine unestablished extent from a fixed-size struct copy. `leveldb` would emit 167 `HIGH` buffer-overflow claims, nearly all of them benign `memcpy` of small objects. That is the cry-wolf failure mode, and it is the same reason the positive `QRY-001` oracles are the deferred ones. |
| Add a `--function` / `--call-site` selector to the CLI | The M10B plan declines public scope selection on this command (Step 3: "Fixture/store selection belongs to integration setup and is not exposed as a public fixture-selection option"). It would also grow the CLI without resolving §2's scope incoherence — a selector picks a caller, it does not make the flow call-site-local. |

§2.3 of the companion test contract already assigns the missing producer:
"a value-range/capacity relation in M10A". The deferral is a **missing relation**,
not a relaxed oracle. Any widening that discriminates real candidates from benign
ones needs that relation first.

# 4. Proposed Amendment

## 4.1 The anchor becomes the sink call-site set

`ResolveOverflowClaimSeed` (l.102–124) keeps its label match and continues to
require a single sink **callee** node, preserving today's
`ambiguous <sink> call` refusal. It then enumerates that callee's distinct
caller nodes instead of requiring exactly one.

## 4.2 Flow resolution becomes call-site-local

This is the substantive change, and §2.1 is why it is required rather than merely
tidier. The exit closure (l.126–149) currently scans every `GlobalFlow` fact in
the run and accepts a non-CPG target as "the sink's argument list". The amendment
restricts the candidate set to the values that reach the **call site's argument
list**, so `sink_ref` and `source_ref` become call-site-local and join
`subject_ref` in one scope.

Anchoring the closure to the call site is what makes the `sink_ref` comment true
as written. Without it, naming a seed's `sink_ref` "the operand the sink
consumes" requires the program to have exactly one non-CPG flow target — an
assumption the resolver cannot check and does not state.

Either form is admissible: the resolver can walk the call site's argument values
directly, or the WPA can derive a call-site-anchored operand relation. The second
is more consistent with the backbone's provenance principle, and §8.1 asks which
belongs here.

## 4.3 A call site qualifies only on evidence

A candidate call site yields a seed only when a **run-bound value-range or
capacity fact** shows the consumed value's extent is not established at the
call. §2.3 records that `DirectRead`/`DirectWrite` byte ranges are EDB base
facts "not run-bound and so invisible to queries", so this predicate cannot be
written today. It requires the M10A value-range relation that §2.3 already
assigns to M10A. Until it exists, the refusal stays — but it will be a refusal to
**qualify** a call site, reported per §6, rather than a refusal to resolve the
sink.

## 4.4 One claim per qualifying call site

The resolver is renamed to the plural `ResolveOverflowClaimSeeds` and returns the
qualifying seeds in canonical StableId order. This is a fan-out, not a pick, so
the "never guesses" rule at the top of `OverflowClaimSeed.h` is preserved: every
returned seed is exact, and an empty set is still a hard error.
`EvidenceQueryService::BuildEvidenceInput` already takes one `ClaimSeed` and one
budget per call (`include/veritas/evidence/EvidenceQueryService.h:82`), so the
builder needs no signature change — it is invoked once per seed.

`finding_id` keeps its current shape, derived from the seed's three refs, which
are now all call-site-local. Distinct call sites produce distinct findings.

## 4.5 Failure semantics are preserved

Empty candidate set → `NotFound`, as today. A call site whose consumed value has
an established extent is **excluded with a reason**, never merged or dropped
silently — this is the same "never manufacture a fact" rule the M10B spec already
applies to its fact sets.

# 5. Compatibility

- **On every M10B fixture the amendment is a no-op.** `QRY-001`–`QRY-010` use
  purpose-built single-external fixtures, where the call-site set has one member
  and the program has one non-CPG flow target, so the global exit closure and the
  call-site-local one coincide (§2.1). The chosen refs are unchanged, so
  `finding_id` is unchanged and the checked-in golden slice
  (`tests/golden/evidence/overflow_unsafe.slice.json`) needs no re-derivation.
- **The CLI surface does not change.** `--sink`, `--format`, and the budget flags
  keep their meaning. No new flag is introduced (§3).
- **Multi-call-site stores change shape, not semantics.** Where the query today
  refuses, it will return more than one claim. The slice document must carry them
  as an ordered collection; how it is spelled is a §6 amendment to the M10B
  design spec, and it must not alter the single-claim spelling that the golden
  test pins.
- **Deferral bookkeeping.** §2.3 of the companion test contract gains this
  amendment as a dependent: the "which call sites qualify" decision moves out of
  `OverflowClaimSeed.h` and into the M10A range relation.

# 6. Near-Term Change, Independent of M10A

One part of this proposal is implementable now and needs nothing from M10A: make
the refusal actionable. Both refusals should name what they found:

```text
the memcpy call has no single owning function (167 candidate callers;
  canonical minimum <caller-stable-id>)
```

and the same for the multi-anchor refusal at l.120. The mitigation costs no
semantics — it reports the set it already computed — and it converts a dead end
into a statement the operator can act on. It belongs with the M10B demo work as a
change to `OverflowClaimSeed.h` and the CLI, with coverage added as one new row
in the companion test contract's `QRY` catalog; assigning that ID belongs to the
contract owner, since the catalog is the authority on case numbering.

# 7. Non-Goals

- No change to the `--sink` vocabulary, which stays the closed list `{"memcpy"}`.
- No weakening of the "never guesses" rule; §4.4 replaces one exact seed with
  several exact seeds, never with an arbitrary one.
- No public selector for scope, store, or fixture.
- No change to `EvidenceQueryBudget` or to per-query completeness and truncation
  semantics, which stay per-query as `QRY-004` and `QRY-010` require.

# 8. Open Questions for Review

1. **Where does the qualifying predicate belong?** §4.3 puts it in the resolver.
   It could instead be a Datalog-derived fact over the M10A range relation, which
   would make qualification explainable and cacheable like every other derived
   fact. The resolver-side form is cheaper; the fact-side form is more consistent
   with the backbone's "every derived fact has provenance" principle. The same
   question applies to §4.2's call-site-anchored operand relation.
2. **Is a fan-out over call sites the right product surface for an agent**, or
   should the seed set be ordered by a severity or confidence signal so a
   consumer can read the first claim and stop?
3. **Does the slice document's multi-claim spelling belong in M10B**, or should
   M10B keep one claim per invocation and the M10C case builder fan out? The
   former changes the demo's query surface; the latter keeps the demo untouched
   and defers the collection shape to the consumer.
4. **Should §2.1 be scoped as its own defect rather than folded into this
   amendment?** It is a correctness gap in `sink_ref`'s identity that exists
   independently of the caller count: a store with a single caller but several
   unmodeled externals also resolves `sink_ref` off the sink. Reviewers may want
   it tracked, and fixed, separately from the fan-out.
