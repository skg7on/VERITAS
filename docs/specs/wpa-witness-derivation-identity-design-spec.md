# WPA Witness Derivation Identity — Design Spec

## Status

Draft — precedes the implementation.

## Problem

`ResultCanonicalizer::Canonicalize` models at most **one derivation per
(result, rule)**:

> A rule cannot bind two different inputs at the same argument position.

That invariant holds only when every body variable of a rule is a join
variable. The reachability and memory-effect bundles both contain rules with
*fresh* (`_`) and existential variables:

```dl
ReachableCall(f, h, e) :- DirectCall(_, f, g, _, e1),
                          ReachableCall(g, h, e2),
                          WeakenEpistemic(e1, e2, e).
```

For one result `ReachableCall(f, h, e)` there are many alternative derivations:

- many intermediate nodes `g`, and
- for each `g`, many `DirectCall` rows — the `_, _` call-site/dispatch columns
  are fresh and unconstrained.

Each alternative emits a witness at the **same ordinal** with a different input
key. The witness relation carries no derivation identity:

```dl
.decl Witness(result_key:symbol, rule_id:symbol, input_key:symbol,
              input_ordinal:unsigned)
```

so the canonicalizer cannot reconstruct which ordinal-0 edge pairs with which
ordinal-1 edge. It folds all of them into one bucket and fails with
**"witness binds two inputs at one ordinal"**. This is what blocks
`veritas-build analyze` on programs with rich call graphs (e.g. leveldb).

## Design

Make each Datalog firing self-identifying by adding a derivation key:

```dl
.decl Witness(result_key:symbol, rule_id:symbol, derivation_key:symbol,
              input_key:symbol, input_ordinal:unsigned)
```

`derivation_key` is the deterministic concatenation, in ordinal order, of the
derivation's input semantic keys:

- one-input rules: `derivation_key = input_key`
- two-input rules: `derivation_key = cat(input0_key, input1_key)`

The concatenation is unambiguous because each input key is already a
self-delimiting sequence of length-prefixed fields headed by a relation/arity
header (`veritas_key_header_v1`), so the boundary between two keys cannot move.

For the key to be byte-identical between Soufflé and the C++ oracle, every
witness rule for one derivation must bind the **same** body variables. This
removes the fresh `_, _` columns from the `DirectCall` atom so the ordinal-0 and
ordinal-1 rules agree on the call site and dispatch that produced the proof:

```dl
// ordinal 0
Witness(rk, rid, dk, ik, 0) :-
    DirectCall(cs, f, g, d, e1), ReachableCall(g, h, e2),
    WeakenEpistemic(e1, e2, e), ReachableKey(f, h, e, rk),
    DirectCallKey(cs, f, g, d, e1, ik),
    dk = cat(DirectCallKey(cs, f, g, d, e1), ReachableKey(g, h, e2)).

// ordinal 1 — identical body bindings, different input
Witness(rk, rid, dk, ik, 1) :-
    DirectCall(cs, f, g, d, e1), ReachableCall(g, h, e2),
    WeakenEpistemic(e1, e2, e), ReachableKey(f, h, e, rk),
    ReachableKey(g, h, e2, ik),
    dk = cat(DirectCallKey(cs, f, g, d, e1), ReachableKey(g, h, e2)).
```

## Canonicalizer

Group witnesses by `(result_key, rule_id, derivation_key)` instead of
`(result_key, rule_id)`. Each group is exactly one derivation with one input per
ordinal, so the existing cost relaxation and proof selection run unchanged. The
"two inputs at one ordinal" check becomes an invariant on the new grouping
rather than a reachable rejection path.

## Component impact

1. `logic/common/semantic_key.dl` — `Witness` declaration gains `derivation_key`.
2. `logic/reachability/reachability.v2.dl`,
   `logic/memory_effects/may_write.v2.dl` — bind all body variables and emit
   `derivation_key` (six rules: direct, transitive, support × two domains).
3. `src/wpa/RelationIo.cpp` — parse the five-column `Witness.csv`.
4. `include/veritas/facts/Witness.h` — add `derivation_key` to `WitnessEdge`.
5. `src/facts/ResultCanonicalizer.cpp` — group by derivation key.
6. `src/wpa/CppRuleEvaluator.cpp` — emit matching derivation keys.
7. `src/summarydb/schema/v3.sql` — add the column to the witness table.
8. `src/facts/ProvenanceStore.cpp`, `FactStore.cpp`, `AnalysisFactBus.cpp` —
   thread the field through.

## Conformance

The C++ emergency engine and compiled Soufflé must emit byte-identical
derivation keys. `WpaExecutorConformanceTest.EnginesProduceSameCanonicalFacts`
and the `wpa-qualification` differential corpus remain the gate: a change here
is only correct if both engines still agree on canonical facts, witnesses, and
hashes.

## Verification

- Full build and test suite.
- `WpaExecutorConformanceTest` still passes.
- leveldb end-to-end: the WPA stage canonicalizes without
  "witness binds two inputs at one ordinal".
