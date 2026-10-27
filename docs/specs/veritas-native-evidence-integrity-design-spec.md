# Native Evidence Integrity Refinement

**Status:** Native Tasks 1–6 implemented; execution and qualification records are in the focused plan.
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
operands to the same callsite. #156 is the successor to closed #132 for exact-callsite scoping; the
producer-gated automatic fan-out policy remains separate from fixing identity. Adding an unrelated caller cannot move a claim.

CPG projection bytes/IDs legitimately change when containment or callsite nodes
are added. Version the projection producer (`thin-cpg.v2`) and preserve reading
old projections. Do not mask their hashes in golden comparisons or reinterpret
an old projection as ownership-complete.

### Task 2 execution amendment (#156)

`thin-cpg.v2` now carries typed `CallSiteBinding` records alongside the existing
node/edge allowlists: callsite ID, compiler function-variant owner, callee ref,
normalized operation, indexed actual value/memory refs, and optional result.
`StableValueMapper` already uses caller-local block/instruction positions; its
native encodings are retained. A real regression showed SVF normalization can
shift those positions and rewrite memcpy intrinsics. `ProjectAnalyzer` now
captures an immutable `CompilerProjection` during M4, before SVF runs, and M6
merges its typed nodes/bindings/owners with the completed summaries. Native
DirectCall/UnknownCall facts and CPG bindings name the same original callsite;
recomputing identities from the post-SVF module is forbidden. The snapshot
contains no LLVM pointers and is checked against the original module hash. Reordering functions or inserting another caller
cannot influence a selected call's operands or seed, provided its prerequisite
native function/value identities are unchanged (#125 still governs portability).

Ownership is captured from live M4 compiler arguments/instructions and OriginMap, never
from a completed summary's arbitrary endpoints. Noncaptured stack locations may
also have a compiler owner. Global/argument pointees and captured allocations
remain unowned; an argument *value* belonging to a function does not establish
ownership of the pointed-to memory. Missing origins, including synthetic SVF
model definitions, do not acquire guessed owners. Conflicting origins/owners or
incomplete callsite bindings are refused.

`CpgRepository` stores v2 bindings/arguments in additive
`cpg_callsite_bindings` / `cpg_callsite_arguments` tables, created inside the
existing summary+CPG publication transaction. Every semantic field participates
in the version-gated canonical suffix; reload validates both references and the
projection ID. Historical `veritas.cpg.v1` projections retain their exact encoding
and remain readable without creating the extension tables. Their explicit
`HasCompilerOwnership()==false` coverage cannot qualify exact seed resolution.
This extension does not consume SummaryDB schema-v5, reserved for query manifests.

The public single-seed command retains `--sink memcpy` and adds optional
`--callsite <callsite-id>` for an explicit selection. Multiple matching calls
without a selection remain an ambiguity error. Exact C memcpy ABI and registered
LLVM memcpy intrinsics map to `memcpy.v1`; name substrings, user definitions and
unsupported ABI forms do not. Automatic qualifying fan-out remains gated by #124.

`ClaimSeed` adds optional `callsite_ref` (also in diagnostic slice JSON). The
resolver sets destination memory from argument 0, retains actual byte source at
argument 1 in the binding, and sets the queried extent to argument 2. Its flow
origin is the unique caller-local extent root when known, otherwise the extent
itself; no global exit closure or canonical-minimum root is consulted. The
finding domain advances to `veritas.evidence.buffer-overflow.v2` and binds the
callsite and operands. The input service re-derives and checks the exact destination, extent and
caller-local source before opening its snapshot and scopes unknowns by this exact callsite even
when the extent is a constant shared by several functions. Legacy hand-written
seeds without that field retain their old query behavior; they do not claim the
new exact-callsite guarantee. `eir.v1` grammar and native fact identities remain.

Intentional artifact changes: corrected destination/seed identity, versioned
CPG/projection and query completion identities, and restored caller-scoped
UnknownEffect facts. The unsafe slice retains its five flow nodes, four edges
and seventeen GlobalFlow supports while adding the two real unknowns. EIR now
includes the function and analyzer omissions referenced by those unknowns; L2
also exposes its existing missing-summary omission. Range/capacity/alias/positive
check producers, semantic truncation coverage and summary handoff stay owned by
#124/#157/#158. A safe and unsafe fixture may have the same exact extent path;
that must not be changed by choosing a different external exit.

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

### Task 3 execution amendment (#157 / RDX03)

`EvidenceBuildInput` now has an additive `evidence.handoff.v2` envelope:
`handoff_version`, six `QueryCoverage` sidecars, and `handoff_digest`. Each
`evidence.coverage.v1` record binds its completion FactID, analysis run and query
kind, supported/closed flags, `RESOLVED` or `UNRESOLVED_SCOPE`, diagnostic frontier
refs and producer version. The sidecar does not alter any of the nine cells of
`evidence.query_completion.v1` or the IDs of unchanged completion facts.

`ComputeEvidenceHandoffDigest` hashes the canonical diagnostic handoff, including
all payload and sidecars, excluding only the digest field. Sidecars and frontier
sets are canonically ordered. Modern binding confidence uses locale-independent,
round-trip-safe floating-point text; legacy diagnostic formatting is retained.
The validator rejects malformed semantic rows and nonfinite confidence before
hashing. `ValidateQueryCoverage` rejects unknown versions,
malformed identities, unknown query kinds, duplicate frontiers, and a closed scope
with an unresolved owner or frontier. `ValidateEvidenceHandoffCoverage` requires
one sidecar per result, matching completion/result kind, traversal state/reasons,
examined count and member digest, same-run bindings, one input snapshot fingerprint
and a matching envelope digest. The builder also retains its existing completion
identity and selected-witness validation. This digest detects corruption and
mixed handoffs; it is not a signature authenticating an untrusted producer.

The native service uses producer `veritas-evidence-query.coverage.v1`. Value flow
and unknown-effect queries are supported but open. Native range, capacity, alias
and positive dominating-check producers remain unavailable (#124), so their
coverage is explicitly unsupported, including when traversal completes empty.
No native query currently asserts scope closure. Known selected unknown-effect
FactIDs are diagnostic frontier refs; an empty frontier list never establishes
closure. A missing sink owner yields `UNRESOLVED_SCOPE` for the function-scoped
unknown query. Traversal metadata still describes the bounded scan independently;
standalone query APIs do not claim the assembled envelope's semantic guarantee.

The builder emits claim-scoped `unsupported_predicate`, `open_frontier`,
`unresolved_scope` and `truncated_query` omissions plus existing-type unknowns.
Multiple gaps may coexist: an unsupported or open traversal can also truncate,
and every exact truncation reason survives. Positive facts remain evidence at
their supplied authority. Only an admitted dominating-check predicate with
supported, resolved, closed, complete-empty coverage can derive `MUST_NOT`.
The exhaustive closed-world fixtures are test oracles, not native qualification.
L0 retains every new coverage unknown and omission; L1/L2 retain them unchanged.
EIR-T and protobuf round trips preserve their content and EvidenceID without any
`eir.v1` grammar or enum extension.

All three envelope fields absent is the explicit legacy path, with a
claim-scoped `legacy_coverage` omission at every level. Its old admitted behavior
remains; any partial envelope is refused. A future controller requiring modern
guarantees must reject the legacy path. Intentionally changed native artifacts
gain coverage unknowns/omissions and lose the unsupported complete-empty negative
dominating-check fact. Historical handoff JSON with no envelope keeps its bytes.

### Remaining handoff work (#158 / RDX04)

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

### Task 2 review refinements

Verifier-valid ABI-affecting parameter/return attributes (including `byval`,
`byref`, `sret`, register/static-chain and Swift conventions) do not qualify the
ordinary memcpy profile. The producer also refuses ordinary C pointer spaces
other than zero. Registered LLVM memcpy intrinsics retain their own explicit
address-space semantics. Scalable/unsized base or GEP storage cannot acquire a
fixed memory location: the memory helper reports unsupported before fixed-size
conversion, ownership skips it, and call binding retains the raw value while
withholding the normalized-copy capability when required memory is unavailable.
These paths retain valid fixed-layout native encodings.

Review deferred minor: pure-graph validation checks optional `result_ref`
existence but not its owner. The compiler producer supplies the correct owner;
no current consumer uses this field. Tighten that check before qualifying a
result-consuming backend.

### Tasks 4–6 execution amendment (#158 / RDX04)

Native assembly now calls pinned `Explain` for every distinct selected support,
contradiction, domain and unknown fact. `max_provenance_depth` is the actual M9
walk depth. Each call bounds witness nodes and edges; the canonical union of
fact witness nodes plus edges is bounded by `max_nodes`. The six query
certificate witnesses are fixed additional overhead. Per-fact `FactExplanation`
records bind the current run/witness and preserve the exact unexpanded FactIDs
and truncation reason, independently of another selected fact's shared closure.
Aggregate exhaustion adds `max_provenance_members`; it does not erase a prior
depth reason. An interrupted edge list retains its output fact as an expansion
frontier. `ProvenanceGraph.frontier_fact_ids` is an additive M9 protobuf field;
`eir.v1` grammar and enum values are unchanged.

`ProvenanceStore` checks the current selected witness at every expanded derived
fact. A stale intermediate binding is an error, not an apparently rooted leaf.
Assembly validates the returned semantic fact/run/binding, rejects conflicting
union members and cycles, and preserves exact-boundary completion. The backend
uses one SQLite read transaction for facts, explanations and summary bindings;
a changed synthetic backend returns the existing retryable failure without a
new-run subquery or partial handoff.

EIR records the bounded selected witnesses with their actual rule, source anchor
when supplied, and producer (including the native engine when the producer text
is absent). Mapped selected facts name their own derivation, independently of
query-certificate provenance. Edges between mapped facts become provenance
inputs. Other premise bodies remain typed Fact dependencies with expandable
`provenance_premise` omissions carrying the output/input FactIDs, rooted/derived
kind, ordinal and witness. They do not acquire a fabricated EIR predicate. L0
similarly replaces withheld premise bodies with expansion handles, and every
level preserves claim-scoped `provenance_frontier` omissions. Native source
anchors not produced upstream remain absent.

`ReadSummaries(refs)` accepts function refs and exact summary refs from selected
provenance. With the optional SummaryRepository CAS reader, the native backend
resolves current bindings in the pinned revision/build, verifies immutable
content identity and refuses stale exact refs. The service carries actual
`SummaryArtifact`s and `SummarySelection`s (run, function, summary ID and explicit
component digests) in the handoff. Payloads, selections and per-fact explanations
are canonically included in its v2 digest. The builder validates summary/function/
run/program binding and advertised/computed component hashes before projection;
it never reopens a database. L2 emits genuine `SummaryReference`s naming their
selected components. L0/L1 retain the function and exact summary expansion handle;
backends lacking summary capability retain an explicit missing-summary omission.

The native rooted-input producer had written FunctionVariantIDs into
`RootedInputFact.summary_id`. A regression now requires the actual immutable
FunctionSummaryID. `WpaSummaryIndex` computes it once per artifact, and the
materializer carries that ID. Native configuration identity advances to
`veritas.wpa.config.v3`, preventing v2 receipts from impersonating corrected
provenance. This changes run/receipt/handoff/Evidence identities, while unchanged
semantic FactIDs, summary schemas and function identities remain exact. Historical
placeholder refs are not reinterpreted as real summary IDs.

The safety request now asks a verifier to prove all three parts for one exact
source/sink execution domain:

```
execution_domain_complete(source, sink)
and ((exists path in feasible_paths(source, sink): true)
     or entry_infeasible(source, sink))
and (forall path in feasible_paths(source, sink): within_capacity)
```

`UNKNOWN` feasibility never proves domain emptiness. The model validator refuses
`VERIFIED_SAFE` based solely on a bare universal result, even when it names a
producer/result. A decided guarded result must establish the same domain. Native
assembly still emits only `PENDING` obligations and `POSSIBLE_DEFECT`; this adds
no verifier dispatch, proof result or native domain-completeness producer.

Intentional golden changes are the v3 run binding, selected witness records and
Fact dependencies, premise/frontier omissions, current summary expansion handles,
and the stronger pending predicate. On the recorded host, the unsafe L1 case
keeps its 14 entities and 17 facts; provenance grows from 6 to 31, dependencies
from 3 to 41, and omissions from 12 to 33. These are fixture observations, not
portable count guarantees. L2 replaces its summary handle with the actual
reference. The truncated case additionally cuts provenance under the shared
member limit, so its provenance omission count is not inferred by adding one
to the untruncated case. Existing golden identity masks are not broadened.
Native range/capacity/alias/positive-check production remains under #124; no
unsupported or open query has acquired negative or verified authority.


RDX04 review refinements: the v2 handoff now declares digest-bound
`provenance_version = evidence.provenance.v1`. That capability requires exactly
one explanation per selected fact, including validation when the carrier is
empty. Unknown versions and partial capability/carrier combinations are refused.
Pre-RDX04 inputs without the capability remain readable with a claim-scoped
`legacy_provenance` omission at every level; a completion witness does not grant
rooted-derivation capability. Controllers requiring that capability must reject
this labeled legacy path. Absent legacy JSON carriers retain their old bytes.

When a legitimate aggregate cut omits a selected fact's witness, EIR still
records the pinned occurrence's actual producer/run, with no invented rule or
anchor. Its selected witness ID and exact frontier remain in the truncation
omission. This keeps MAY facts valid and unstrengthened under small budgets,
without substituting query provenance or claiming the witness was traversed.
Fact dependencies include these bounded selected occurrences as well as the
retained graph's premises. The native five-node cut and a durable synthetic
MAY-alias corpus exercise this path; the latter is a handoff oracle and does not
qualify the deferred native alias producer.

The native snapshot also checks its revision/build against the persisted WPA
run manifest inside the read transaction before serving refs. A descriptor for
a different program context fails even if its requested summary set is empty;
`RejectsStaleSummaryBinding` observes that boundary RED→GREEN as well as stale
current-object bindings.
