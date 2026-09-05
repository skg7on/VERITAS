# M8R–M9 Contract Reconciliation Design

**Status:** Approved in architecture review on 2026-09-05
**Scope:** Post-merge reconciliation for PRs #87, #89, #91, #94, and #97
**Affected milestones:** M8R.4, M8R.5, and M9

## 1. Purpose

The recent M8R and M9 pull requests delivered the intended component types,
executors, run repository, Fact Bus, Fact Store, provenance store, and explain
CLI. The normal production path does not yet connect those pieces with all of
the guarantees stated in the milestone specifications. The executable M9 gate
also passes without exercising several of those guarantees.

This repair makes the shipped implementation and its qualification evidence
match the written contracts. It preserves the witness-independent
`relations.v2` `FactID` and the rule that compiled Soufflé is the default
production executor. It does not introduce an M10 analysis domain or change
the semantic meaning of existing facts.

## 2. Audit Findings

The repair covers every confirmed finding from the post-merge review:

1. `ProjectAnalyzer` discards the successful `WpaRunResult`; normal analysis
   publishes no M9 batch and therefore produces no facts that
   `veritas-explain` can read.
2. `WpaOrchestrator` indexes completed facts only by SCC. The second component
   overwrites the first, so a predecessor can lose same-domain successor
   support when Reachability and MemoryEffects run together.
3. The production analyzer does not attach `SccStateRepository`, disabling the
   documented M7 external-change scheduling path.
4. Production run manifests use placeholder SVF/WPA hashes and label-only
   toolchain identities. The generated Soufflé provenance manifest is checked
   by tests but not by the runtime. `run_cpp_conformance_oracle` is unused.
5. Result-cache keys are delimiter-concatenated, do not explicitly include the
   engine, and cached objects are returned without revalidating their content
   against the requested component and run contract.
6. `AnalysisFactBatch` identity omits completed-result hashes and diagnostics.
   Fact Bus validation does not verify the supplied `BatchId`, completion
   payloads, witness outputs, acyclicity, root reachability, or persisted run
   manifest.
7. `FactStore` violates the `AnalysisFactSink` contract because redelivery of
   the same `(RunId, BatchId)` creates another occurrence binding.
8. `ProvenanceStore::Explain` can select a historical binding. New witnesses
   use the semantic `FactID` as their witness ID, preventing distinct
   derivations of the same fact from being retained.
9. Root provenance is reduced to placeholder strings and then discarded at the
   run-to-batch boundary. Source anchors, summary references, and most witness
   metadata therefore remain empty.
10. The raw witness protocol has no derivation identity. Edges from alternative
    multi-input proofs cannot be grouped reliably, which blocked full
    `semantic_zoo` differential and determinism coverage in PR #91.
11. The M8R/M9 plan indexes, the M9 implementation plan, the qualification
    corpus status, and the SummaryDB guide describe a pre-merge state or a
    `FactID` model that contradicts the approved M9 design.

PR #94's legacy `FixpointEngine`/`FactTuple` retirement is consistent with the
current production graph and needs no compatibility restoration.

## 3. Architectural Outcome

One analysis output root owns one SummaryDB metadata database. A successful
production analysis follows this flow:

```text
ProjectAnalyzer
  -> canonical run identities and verified toolchain provenance
  -> WpaRunRepository + SccStateRepository on <output_root>/metadata.db
  -> WpaOrchestrator for Reachability and MemoryEffects
  -> optional, separately identified C++ conformance run and comparison
  -> canonical AnalysisFactBatch
  -> AnalysisFactBus validation and durable fan-out progress
  -> idempotent FactStore transaction
  -> ProvenanceStore / veritas-explain
```

Summary publication remains independent of WPA fact publication. A WPA,
conformance, batch-validation, or fact-store failure publishes no new M9 fact
occurrence and leaves earlier successful facts as history.

## 4. Run and Toolchain Identity

### 4.1 Canonical configuration hashes

The SVF configuration hash is SHA-256 over
`SvfConfig::CanonicalAnalyzerConfig()`. The WPA configuration hash is SHA-256
over a new length-prefixed, versioned canonical representation containing:

- component set;
- timeout, memory, and thread limits;
- relation and rule-bundle execution settings; and
- any executor setting that can change evaluation behavior.

The request to run an additional conformance oracle is orchestration policy,
not primary-run semantics, and does not change the production RunId. The
conformance execution receives its own engine identity, toolchain identity,
and RunId.

### 4.2 Verified Soufflé identity

The build supplies the analyzer with paths to `souffle-provenance.json`, the
Soufflé executable, the worker, the functor library, and generated bundles.
A dedicated loader:

1. parses the manifest;
2. requires Soufflé 2.5 at revision
   `5682a9f12e2668ecdd26348fe63cc508bc0fcf47`;
3. verifies every recorded artifact digest;
4. recomputes the canonical provenance digest; and
5. derives a versioned toolchain identity from that verified digest.

Any missing, malformed, or mismatched artifact fails before a WPA run begins.
The analyzer target depends on the provenance target so the runtime cannot be
built without the identity material it requires.

### 4.3 C++ identities and conformance

C++ conformance and emergency identities are derived from a generated,
canonical build fingerprint containing the VERITAS version and Git revision,
compiler identity/version, target platform, build type, relevant flags, and
rule-bundle version. They cannot reuse a Soufflé identity.

When `run_cpp_conformance_oracle` is true, `ProjectAnalyzer` runs a second
`kCppConformance` execution over the same engine-neutral logical inputs. The
two runs have distinct RunIds. Their per-component canonical facts,
`ExternalHash`, and `FixpointHash` must agree. A mismatch marks both runs
incomplete, records a diagnostic, prevents M9 publication, and returns a
failure. There is still no automatic production fallback.

## 5. Orchestration and Incremental State

Completed facts are keyed by the full `WpaComponentKey` `(SccId,
WpaComponentKind)`. `SuccessorSupport` reads only the matching component from
each successor. Tests must run both components over at least two SCCs and prove
that Reachability support survives the later MemoryEffects completion.

The production analyzer constructs `SccStateRepository` from the same metadata
store used by `WpaRunRepository` and passes it to every orchestrator. A changed
external hash schedules predecessors through M7; an internal-only witness
change does not.

## 6. Cache Identity and Validation

A versioned `ResultCacheDescriptor` contains:

- engine identity and verified toolchain identity;
- logical-input hash;
- SCC and component kind;
- summary and relation schema versions;
- rule and model bundle versions; and
- semantic SVF/WPA configuration hashes.

It is encoded with length-prefixed fields and hashed. The resulting digest,
not raw delimiter concatenation, is the cache and object key. This intentionally
invalidates the old ambiguous key format without rewriting historical run
records.

Cache lookup accepts the expected descriptor rather than only a string. Before
reuse it validates the metadata row, object key, deserialized SCC/component,
logical-input hash, canonical fact identities, rooted witness closure, and
recomputed fixpoint/external hashes. Any mismatch is a hard cache-integrity
error; it is never treated as a hit.

## 7. Versioned Witness and Root-Evidence Contract

The raw witness protocol becomes `witness.v2`. Each edge carries a canonical
`derivation_id` shared by every input edge in one proof. The ID covers the
result semantic key, rule ID, and ordered input semantic keys. `RuleSpec` also
declares its arity so the canonicalizer can reject missing, duplicate, or
out-of-range ordinals.

The canonicalizer groups candidates by `(result, rule, derivation_id)`, proves
each candidate finite and rooted, and selects by:

1. fewest derived edges;
2. rule priority; and
3. lexicographic ordered input FactIDs.

The selected persisted `witness_id` is witness-dependent and is not the
semantic `FactID`. Re-deriving one fact with a different proof retains both
witness records while the current run binding selects exactly one.

`RootedInputFact` is extended with structured evidence: producer/analyzer
identity, provenance reference, source anchor, summary ID, and description.
`WpaRunResult` and `AnalysisFactBatch` carry this root evidence alongside the
canonical rooted-input ID set. The Fact Store persists it and the explanation
graph exposes it, allowing text and JSON explanations to report assumptions,
unknowns, source anchors, and summary references without recomputation.

Changing the witness protocol updates the rule-bundle version while preserving
`relations.v2` semantic rows and existing `FactID` values.

## 8. Batch Validation and Idempotent Persistence

`BatchId` uses a versioned length-prefixed encoding over every immutable field:
the RunId, expected components, each completed component key/object/hash,
rooted evidence, flattened facts, selected witnesses, and diagnostics.

Before fan-out, the Fact Bus validates:

- the supplied BatchId equals the recomputed canonical ID;
- expected and completed component lists contain no duplicates and are equal;
- every completion matches its key and its recomputed result hashes;
- flattened facts, witnesses, roots, and diagnostics equal the canonical union
  of the completions;
- every witness result is a published fact;
- every published fact has exactly one selected, finite, acyclic proof that
  reaches declared roots;
- every referenced fact has its canonical witness-independent FactID; and
- the complete persisted run manifest equals the batch manifest.

Schema v4 adds an atomic Fact Store receipt keyed by `(run_id, batch_id)` and
linked to the WPA run. Receipt insertion, facts, bindings, root evidence, and
witness records commit in one transaction. Redelivery of the same receipt is a
successful no-op. A different batch in the same run may create a new current
occurrence and preserve the prior binding as history.

`ProvenanceStore::Explain` selects `is_current = 1`, follows the binding's
`selected_witness_id`, and orders any retained alternatives deterministically.
Existing v3 data remains readable; new writes use v4 receipts and
witness-dependent IDs. No existing FactID is rewritten.

## 9. Error Handling

The repair is fail-closed at every trust boundary:

- invalid toolchain provenance prevents run creation;
- an invalid or corrupt cache entry prevents reuse and records an incomplete
  run diagnostic;
- engine or conformance failure publishes no batch;
- invalid batch identity, component completion, run binding, or witness DAG
  reaches no sink;
- a Fact Store transaction failure leaves neither a receipt nor partial facts;
  retry is safe; and
- explain rejects a missing current binding or selected witness instead of
  silently choosing historical data.

Earlier successful runs and canonical facts remain queryable as history.

## 10. Qualification Strategy

Tests are added before implementation changes and include:

- two-component, multi-SCC successor propagation;
- production M7 state persistence and predecessor scheduling;
- every run-descriptor field changing the RunId;
- tampered Soufflé manifest/artifact rejection and distinct C++ identities;
- requested C++ conformance execution and mismatch failure;
- collision-resistant cache descriptors and corrupt-cache rejection;
- BatchId changes for every completion, witness, root-evidence, and diagnostic
  field;
- rejection of extra witness outputs, cycles, unrooted subgraphs, malformed
  arity, mixed runs, and incomplete runs;
- crash-window-safe repeated Fact Store delivery;
- distinct witnesses for the same FactID and current-binding explanation;
- populated assumption, unknown, source-anchor, and summary-reference output;
  and
- full mixed C/C++ `semantic_zoo` analysis through Soufflé/C++ differential,
  determinism, SummaryDB persistence, Fact Bus, Fact Store, and explain paths.

The M9 entry registry gains the tests that prove these behaviors. The gate
continues to reject missing, extra, skipped, disabled, failed, or errored
members, but a label is no longer accepted as a substitute for production-path
coverage.

Verification uses the repository's supported serial suite:

```bash
cmake --build --preset default
ctest --test-dir build --output-on-failure
python3 tools/check_m9_entry.py --build-dir build
git diff --check
```

## 11. Documentation Reconciliation

After executable acceptance passes:

- the milestone and plan indexes mark M8R.3–M8R.5 and M9 delivered with exact
  PR/commit evidence;
- the M8R delivery record replaces `this PR` with reviewed merges;
- the qualification-corpus spec records the completed `semantic_zoo` layers;
- the M9 implementation plan is rewritten to match witness-independent
  `FactID`, schema v4, the Fact Bus, and actual APIs;
- the SummaryDB guide documents the working explain command and shared output
  database; and
- `M9DocumentationConsistencyTest` asserts milestone status, plan/spec
  identity terminology, and the operational CLI statement.

Status documents are updated only after their corresponding executable tests
pass.

## 12. Delivery Boundaries

This reconciliation is one implementation project with ordered checkpoints:

1. orchestration correctness and regression tests;
2. exact identities and cache validation;
3. witness.v2 and structured root evidence;
4. Batch/Fact Store/provenance correctness;
5. production pipeline integration and conformance;
6. `semantic_zoo` qualification and gate expansion; and
7. documentation status reconciliation.

Out of scope are new recursive relations, Evidence Builder behavior, external
provider facts, UI work, automatic C++ fallback, and changes to canonical
semantic FactID meaning.

## 13. Acceptance

The repair is complete only when all audit findings in section 2 have a
regression test or an explicit documentation-only resolution, normal
`veritas-build analyze` produces explainable facts in its output SummaryDB,
the complete serial test suite and M9 gate pass, and the milestone/design/plan
documents describe that verified state without forward-looking delivery
claims.
