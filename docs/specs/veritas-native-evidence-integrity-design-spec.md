# Native Evidence Integrity Refinement

**Status:** Detailed proposed amendment; implementation status is recorded per task.
**Baseline:** `99d3130`; P0 of the claim-driven redesign. Existing native SVF,
Soufflé, `summary.v2`, `relations.v2`, and `eir.v1` contracts remain in force.
**Plan:** [native integrity](../plans/veritas-native-evidence-integrity-implementation-plan.md).

## 1. Purpose and audited seams

P0 makes an existing Evidence result trustworthy before building another
consumer. It does not rebuild M0–M10 or treat closed milestone issues as proof
that semantic acceptance passed. The following source observations are the
starting conditions, not predictions about a later branch:

| Seam | Observation | Required behavior |
| --- | --- | --- |
| `src/facts/AnalysisFact.cpp`, `ValidateSemanticRow` | Checks domain/arity/range, not `RelationSchema::allowed_epistemic` | Reject a valid enum that the relation does not admit |
| `src/analysis/cpg/CpgProjectionStage.cpp`, `BuildThinCpg` | No `kContains` production | Function-local queries resolve ownership from compiler identity |
| `src/evidence/EvidenceQueryService.cpp` | Missing containment produces an absent unknown scope | Emit an explicit unresolved-scope outcome; do not silently describe it as a successful function query |
| `EvidenceCaseBuilder::BuildPaths` | Path construction does not consume flow completeness | Preserve omissions for incomplete and complete-empty open-world queries |
| `BuildSummaryExpansions` | Emits omissions because handoff has no summaries | Carry immutable summary references from the selected snapshot |
| Query budget | Provenance depth is encoded but not threaded through input assembly | Actually bound explanation traversal and expose truncation |

Audit findings from #149 not covered by these source observations require a
focused reproducer before a runtime change. Existing #124 owns the full native
range/capacity/alias/positive-check producer contract; the lightweight local
provider does not close that issue by producing a narrower answer.

## 2. Relation authority admission

`ValidateSemanticRow(const SemanticRow&) -> Status` remains the sole public row
gate. After domain validation, every column whose domain is epistemic must
contain a member of that relation's `allowed_epistemic` vector. Domain-valid
but disallowed input returns `InvalidArgument`; `MakeFact` must propagate it
before deriving an ID. No fallback to `MAY`, `UNKNOWN`, or the first allowed
state is permitted. Relations without epistemic columns retain current behavior.

The registry is authoritative, not a second handwritten per-relation switch.
Registry tests require an empty allowlist exactly when no epistemic column
exists, and otherwise a nonempty, unique set of valid states. A valid row's
semantic bytes and FactID do not change. Previously inadmissible persisted rows
fail validation; they are never silently rewritten under their old IDs.

Regression tests use independent literal authority oracles for restricted
families (UnknownCall, existential reachability and coverage), positive admitted
controls, both semantic/dense gates and MakeFact/DeriveFactId refusal. Keep the
existing valid FactID goldens exact. Broader registry consistency tests must
not calculate their expected authority by simply calling the validator again.

The producer must also emit admitted states. A definite call instruction is
not a definite unresolved target: project source MUST/MAY into UnknownCall MAY;
source INFERRED/ASSUMED/UNKNOWN projects to UNKNOWN, never a stronger claim.
Unresolved negative calls remain unrepresentable and are refused, not coerced.
This is a producer-specific weakening, not a permissive validator fallback.
The native WPA configuration domain advances to `veritas.wpa.config.v2`, so
old receipts/cache descriptors cannot reuse the previously invalid projection.
Unchanged valid row FactIDs remain identical; corrected UnknownCall and dependent
UnknownEffect/witness identities legitimately change. The captured mutex-call
baseline updates only those warrant tags. HND-003 uses admitted positive/negative
Alias facts instead of a schema-invalid MUST_NOT GlobalFlow placeholder.

## 3. Ownership and exact call sites

Introduce an additive compiler-owned mapping, not a heuristic inferred from a
summary's arbitrary flow endpoints:

```cpp
struct EntityOwner {
  core::StableId entity_id;          // value, instruction/callsite, or local memory
  core::StableId function_variant_id;
};
StatusOr<std::vector<EntityOwner>> CollectEntityOwners(const ProgramIr& ir);
```

Place this at `src/analysis/cpg/EntityOwnership.{h,cpp}`. Read the existing
OriginMap and value identities at extraction; if an entity lacks a provable
owner, do not invent one. Projection emits `function -> entity` `kContains`
only for declared nodes with exactly one owner. Global/escaped memory and a
callee node shared by callers are not function-local call sites. Reject
conflicting owners for a supposedly local entity. Order/deduplicate owner pairs
canonically; source order must not affect projection identity.

Preserve the existing single-seed CLI. Add typed `CallSiteBinding` internally:
`callsite_id`, `function_variant_id`, `callee_ref`, argument-index-to-value map,
and optional result. For `memcpy`, destination/source/length are arguments
0/1/2 after registered LLVM-intrinsic normalization. Unsupported ABI/intrinsic
forms return `UNSUPPORTED`, never a canonical-minimum guess. Bind all seed
operands to the same callsite. The closed #132 needs an open successor tracker
for its still-unimplemented scope; its producer-gated fan-out policy remains
separate from fixing identity. Adding an unrelated caller cannot move a claim.

CPG projection bytes/IDs legitimately change when containment or callsite nodes
are added. Version the projection producer (`thin-cpg.v2`) and preserve reading
old projections. Do not mask their hashes in golden comparisons or reinterpret
an old projection as ownership-complete.

## 4. Coverage and handoff

Add `QueryCoverage` sidecars to `EvidenceBuildInput`, keyed by completion fact
ID, with `predicate_supported`, `scope_closed`, `frontier_refs`, and
`producer_version`. The sidecar is versioned `evidence.coverage.v1`, serialized
into an immutable handoff artifact and its digest. It does not add cells to an
existing `evidence.query_completion.v1` fact under the same identity.

Completion of traversal and semantic capability are independent. The truth
table is normative:

| Supported | Scope closed | Traversal | Empty-result interpretation |
| --- | --- | --- | --- |
| false | either | either | Unsupported predicate omission |
| true | false | complete | Open-frontier unknown/omission |
| true | true | truncated | Incomplete omission with exact reason |
| true | true | complete | Negative evidence only for an admitted closed-world predicate |

Legacy handoffs without a sidecar use existing admitted behavior and are
labeled legacy coverage; they cannot claim new semantic capabilities. The new
controller refuses legacy coverage for decisions requiring the new guarantee.
An absent function scope is `UNRESOLVED_SCOPE`, with an omission attached to the
claim, not an empty successful function-scoped unknown query.

Add `std::vector<summary::SummaryArtifact> summaries` and explicit component
selection to the immutable input. Query assembly obtains them from the same
read snapshot; M10C never opens the database again. Validate summary IDs,
function/build/run bindings, component hashes and references before projection.
L2 emits genuine `SummaryReference` members when supplied, otherwise an explicit
expandable omission. L0/L1 still carry expansion handles without copying bodies.

For every selected fact, assemble its selected rooted witness closure through
`EvidenceReadSnapshot::Explain(fact_id, ExplainBudget)`. Thread the positive
`max_provenance_depth` into that call. Canonically union nodes/edges under the
same snapshot, preserve each truncated frontier, and bound total provenance
members as well as per-fact depth. A completion certificate's witness alone is
not a substitute for the returned fact's derivation. The builder retains
support, contradiction, unknown and omission separately.

## 5. Proof domains and compatibility

`UNKNOWN` path feasibility never implies membership in an empty universally
quantified domain. A verifier request states the admitted execution domain,
assumptions and coverage explicitly; a universal-safe decision requires a
non-vacuity/domain argument or an explicit proof of infeasible entry. A missing
path, truncated result, or missing producer cannot establish that argument.

Keep `eir.v1` grammar and enum values. Use existing unknowns/omissions and
well-formed source/summary references. New coverage/handoff identity and CPG
producer versions are additive. Explain every intentional EvidenceID/golden
change with the new member or corrected omission; do not overwrite goldens
merely to obtain a green suite. Fact IDs for unchanged valid rows remain exact.

Acceptance: forbidden relation states are rejected; opaque-validator unknowns
reach EIR; missing containment and complete-empty/truncated inputs discriminate;
two callers/two externals never contaminate operands; L2 has real references;
depth 1 versus depth 2 produces the expected rooted frontier; all original
unaffected native conformance tests remain live with no unexpected skips.
