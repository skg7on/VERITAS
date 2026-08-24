# M10B–M10C API-to-Evidence-IR Test Design Spec

**Status:** Approved
**Milestones:** M10B Evidence Builder inputs and M10C Evidence IR semantic serialization
**Depends on:** M6 thin CPG, M9 fact/provenance store, M10A recursive domain expansion
**Validates:** M10B issue #13 and M10C issue #70

---

# 1. Purpose

This specification defines the executable test contract for the complete
deterministic boundary:

```text
M6/M9/M10A semantic state
    -> EvidenceQueryService
    -> EvidenceBuildInput
    -> EvidenceCaseBuilder
    -> validated EvidenceCase
    -> EIR-T / Protobuf / full-EIR JSON / CLI
```

The existing M10B and M10C specifications define the production contracts.
This companion defines how tests prove those contracts, how fixtures isolate
unsafe and safe behavior, and which outputs are explicitly forbidden. It is
the shared semantic oracle for both milestones; the implementation plans own
the order in which the cases become executable.

The suite must demonstrate more than a successful buffer-overflow output. It
must prove that budget limits, incomplete analysis, contradictory facts,
provenance, identity, and representation boundaries cannot manufacture
certainty or change meaning.

Normative production contracts are the
[M10B Evidence Builder Input APIs design](m10b-evidence-builder-input-apis-demo-design-spec.md)
and the
[M10C Evidence IR semantic model and serialization design](m10c-evidence-ir-semantic-model-serialization-design-spec.md).
The [M9 provenance and Fact Store design](m09-provenance-fact-store-explain-api-design-spec.md)
governs persisted facts, run bindings, and witnesses used by query provenance.

---

# 2. Goals and Non-Goals

## 2.1 Goals

The test suite must:

* exercise the public M10B API with real and synthetic analysis inputs;
* distinguish complete, empty results from partial or truncated results;
* verify one immutable, single-snapshot `EvidenceBuildInput` handoff;
* prove M10C assembly never recomputes analysis or strengthens epistemic state;
* verify EIR-L0/L1/L2 projection and explicit omission behavior;
* reject malformed, mixed-run, dangling, or authority-violating evidence;
* prove canonical identity is semantic and independent of insertion order,
  checkout root, text whitespace, and Protobuf wire order;
* prove EIR-T, Protobuf, and full-EIR JSON represent the same case;
* exercise failure-atomic CLI behavior; and
* provide small, readable demonstrations for unsafe, safe, incomplete, and
  cross-format cases.

## 2.2 Non-goals

This specification does not:

* implement M9, M10A, M10B, or M10C production code;
* treat golden-text comparison as a substitute for semantic assertions;
* test a Review Agent, LLM response, verifier dispatch, or proof promotion;
* establish new security-domain rules beyond the initial buffer-overflow
  fixture and registered predicate mappings;
* require whole source files inside `EvidenceBuildInput` or `EvidenceCase`;
* authorize flaky, disabled, or environment-dependent acceptance tests; or
* duplicate every parser token test already owned by the M10C plan.

---

# 3. Test Architecture

Tests are divided into seven layers. Lower layers use focused synthetic values;
higher layers use the real fixture pipeline and public boundaries.

| Layer | Prefix | Boundary under test | Allowed test doubles | Primary failure localization |
| --- | --- | --- | --- | --- |
| Contract | `AC` | Slice types, budgets, result metadata, deterministic diagnostic JSON | Typed values only | Invalid or ambiguous API state |
| Query | `QRY` | `EvidenceQueryService` over CPG, FactStore, and ProvenanceStore | Unit fakes; real stores in integration cases | Incorrect semantic query or truncation |
| Handoff | `HND` | `BuildEvidenceInput` snapshot and completeness contract | Controlled service backends | Missing, mixed, or JSON-coupled handoff |
| Builder | `BLD` | Predicate mapping and EIR-L0/L1/L2 assembly | Typed M10B scenarios | Epistemic strengthening or projection loss |
| Validation and identity | `VID` | Well-formedness, canonical bytes, `EvidenceID` | Typed `EvidenceCase` values | Invalid semantic object or unstable identity |
| Representation | `REP` | EIR-T, Protobuf, and JSON codecs | Valid and deliberately malformed cases | Representation-specific semantic drift |
| Demonstration | `DEM` | Public CLI and real project fixtures | None below public fixture helpers | End-to-end regression or UX failure |

No end-to-end case may replace a focused lower-layer case. A CLI golden failure
must be diagnosable through a smaller query, builder, validator, or codec test.

The initial catalog contains 54 stable cases:

```text
AC:   6
QRY: 10
HND:  6
BLD: 10
VID:  8
REP:  8
DEM:  6
total: 54
```

Case identifiers are stable requirements. Implementations may add cases but
must not silently renumber or weaken an existing case.

---

# 4. Query Result and Budget Contract Refinements

Comprehensive testing requires every query result to carry enough information
to distinguish complete absence from incomplete absence and to bind that
conclusion to one analysis snapshot.

## 4.1 Shared result metadata

M10B must use one metadata record for `FlowSlice` and every
`EvidenceFactSet`:

```cpp
namespace veritas::evidence {

enum class QueryCompleteness {
  kUnspecified,
  kComplete,
  kTruncated,
};

struct QueryResultMetadata {
  QueryCompleteness completeness = QueryCompleteness::kUnspecified;
  std::vector<TruncationReason> truncation_reasons;
  std::size_t examined_items = 0;
  core::StableId analysis_run_id;
  core::StableId query_provenance_id;
};

struct EvidenceFactSet {
  std::vector<facts::Fact> facts;
  QueryResultMetadata metadata;
};

}  // namespace veritas::evidence
```

`query_provenance_id` is a `core::IdKind::kFact` ID for an M9-backed query
completion fact in relation `evidence.query_completion.v1`. Its canonical
semantic cells are:

```text
query_kind
ordered_scope_refs
budget
query_implementation_version
input_snapshot_fingerprint
completeness
ordered_truncation_reasons
examined_items
returned_member_digest
```

The `input_snapshot_fingerprint` is SHA-256 over the canonical repository,
revision, build variant, analysis configuration, analysis run, CPG projection,
and fact-snapshot bindings. `returned_member_digest` is SHA-256 over the
canonical ordered IDs of all returned facts, paths, nodes, or edges appropriate
to that query. The completion fact is published through M9 with a
`RunFactBinding` for `analysis_run_id` and a selected `FactWitness` identifying
the query producer and version. Its witness inputs reference the facts and
summary components that support the returned result.

This record is a completion certificate, not evidence that an absent domain
fact is false. For the one registered closed-world rule, the derived negative
fact must have a witness whose input is this completion fact and whose rule ID
is `evidence.closed_world.dominating_check_absence.v1`. A complete empty result
without the matching completion fact, run binding, or selected witness is not
usable as negative evidence.

Validation rules are:

* `kUnspecified` is always invalid at a public boundary;
* `kComplete` has no truncation reasons;
* `kTruncated` has at least one stable truncation reason;
* both complete and truncated results carry one analysis run and query
  provenance reference of kind `kFact`;
* the completion fact's run, scope, budget, completeness, reasons, count, and
  returned-member digest exactly match the result metadata and payload;
* every `query_provenance_id` in `EvidenceBuildInput` resolves to its explicit
  completion fact and run binding plus a selected witness in the handoff's
  provenance closure;
* `examined_items` counts candidates assessed by the query and is at least the
  number of returned facts or path members;
* truncation reasons are unique and serialized in enum order; and
* facts are returned in canonical semantic order, not backend insertion order.

## 4.2 Complete fact budgets

The shared budget gains one explicit non-flow limit:

```cpp
struct EvidenceQueryBudget {
  std::size_t max_depth;
  std::size_t max_nodes;
  std::size_t max_paths;
  std::size_t max_facts_per_query;
  std::size_t max_provenance_depth;
};
```

All limits must be positive. Reaching a limit exactly is complete when the
backend can prove no additional matching result exists. Discovering one more
matching result than the limit returns the canonical prefix and marks the
result truncated. Implementations may use a `limit + 1` probe internally but
must not expose the probe row.

## 4.3 Absence truth table

The builder applies this table without exception:

| Query result | Query semantics | M10B handoff | M10C interpretation |
| --- | --- | --- | --- |
| Complete, non-empty | Open or closed world | Facts plus completion metadata | Map facts without strengthening |
| Complete, empty | Open world | Empty facts plus completion metadata | Explicit `Unknown` or `Omission`; no negative fact |
| Complete, empty | Registered closed world with valid query provenance | Empty facts plus completion metadata | May emit registered negative evidence with derived provenance |
| Truncated, non-empty | Open or closed world | Canonical partial facts plus reasons | Preserve partial facts and add visible incompleteness |
| Truncated, empty | Open or closed world | Empty facts plus reasons | `Unknown` or `Omission`; never negative evidence |

For M10C V1, only the scoped dominating-bounds-check query is eligible for
closed-world absence, and only when its query provenance proves complete
coverage of all paths admitted by the claim scope. Range, capacity, alias, and
external-semantics queries remain open world.

## 4.4 Single-snapshot handoff

`BuildEvidenceInput` executes all subqueries against one immutable analysis
snapshot. Every result, fact, summary, source anchor, and provenance node must
agree on:

```text
repository
revision
build variant
analysis configuration
analysis run
CPG projection
fact snapshot
```

Backends changing their current binding during assembly cannot produce a mixed
input. The service either holds a snapshot/read transaction or returns a stable
retryable failure. The handoff contains the query completion facts and run
bindings plus their selected witnesses; M10C does not perform a second
provenance lookup. M10C independently rejects a mixed or unresolved input.

---

# 5. Case Definition and Oracle Rules

Every catalog case is documented and implemented with these fields:

```text
case_id
test_name
layer
fixture_or_scenario
preconditions
claim_seed_and_scope
budget
input_result_metadata
required_output
forbidden_output
provenance_expectation
representations
determinism_mutation
```

## 5.1 Required assertion style

Tests assert typed semantics before presentation:

1. validate the M10B result or `EvidenceCase`;
2. assert exact member types, references, epistemic values, completeness, and
   provenance;
3. assert explicitly forbidden facts, states, and promotions;
4. compare canonical semantic bytes or `EvidenceID` where identity matters;
5. then compare human-readable golden output if the case owns a golden.

Substring-only JSON or EIR-T assertions do not satisfy a semantic test. Golden
files are parsed back and checked against the typed oracle.

## 5.2 Determinism mutations

Each applicable case repeats the assertion under at least one controlled
mutation:

* reverse backend insertion order;
* use a different temporary checkout root;
* reorder independent facts or entities;
* vary unordered-map construction order;
* encode Protobuf fields in a legal non-canonical wire order;
* pretty-print rather than canonical-print EIR-T; or
* run the same fixture twice from clean stores.

Semantic output and `EvidenceID` remain stable unless the mutation changes a
semantic input.

## 5.3 Negative oracles

Every safety-sensitive test names what must not happen. Common forbidden
outputs include:

```text
truncated empty -> MUST_NOT
UNKNOWN or INFERRED -> MUST
dominating check on one path -> universal check on all paths
MAY_ALIAS -> MUST_ALIAS or NO_ALIAS
contradicting fact dropped from the case
mixed-run members silently rebased
missing provenance silently synthesized
L0 omission hidden rather than declared
different semantic content -> same EvidenceID
wire-format order -> different EvidenceID
failed binary output -> truncated destination
```

---

# 6. Fixture and Scenario Family

## 6.1 Real project fixture

Use the repository's existing project-fixture convention. Each semantic shape
is an isolated project so the public CLI can resolve one deterministic finding
without adding fixture-only selection flags:

```text
tests/fixtures/projects/evidence_overflow_unsafe/
    compile_commands.json
    packet.h
    main.cpp
tests/fixtures/projects/evidence_overflow_safe/
    compile_commands.json
    packet.h
    main.cpp
tests/fixtures/projects/evidence_overflow_non_dominating/
    compile_commands.json
    packet.h
    main.cpp
tests/fixtures/projects/evidence_overflow_mixed_paths/
    compile_commands.json
    packet.h
    main.cpp
tests/fixtures/projects/evidence_overflow_opaque_validator/
    compile_commands.json
    packet.h
    main.cpp
tests/fixtures/projects/evidence_overflow_alias_uncertain/
    compile_commands.json
    packet.h
    main.cpp
tests/fixtures/projects/evidence_overflow_summary/
    compile_commands.json
    packet.h
    entry.cpp
    copy.cpp
```

The shared types are intentionally simple:

```cpp
struct Packet {
  const unsigned char* payload;
  std::uint16_t length;
};

struct Buffer {
  unsigned char data[2048];
};
```

The unsigned 16-bit length provides the expected `[0, 65535]` range and the
array provides the expected 2048-byte capacity without a handwritten test
fact.

| Fixture project | Semantic shape | Required distinguishing outcome |
| --- | --- | --- |
| `evidence_overflow_unsafe` | `packet.length` flows directly to `memcpy` size | Complete unsafe flow; range exceeds capacity; no proven dominating check |
| `evidence_overflow_safe` | `length <= sizeof(buffer.data)` dominates sink | Positive dominating-check fact; no verified-safe promotion |
| `evidence_overflow_non_dominating` | Check occurs only on a sibling branch | Check existence does not become sink dominance |
| `evidence_overflow_mixed_paths` | One checked and one unchecked path reaches sink | No universal safety; both paths remain visible or truncation is explicit |
| `evidence_overflow_opaque_validator` | External validator guards the sink but has no model | External postcondition remains unknown |
| `evidence_overflow_alias_uncertain` | Destination may alias another object | Alias remains `MAY_ALIAS`; capacity reasoning is not strengthened |
| `evidence_overflow_summary` | Source and sink cross translation units and summary edges | Flow retains immutable summary references and expansion markers |

No fixture depends on undefined behavior before the sink, optimizer-specific
constant folding, source line numbers for identity, or platform-specific type
sizes beyond the explicitly recorded target layout.

## 6.2 Synthetic query scenarios

Focused unit tests use `EvidenceScenarioBuilder` under
`tests/support/evidence/`. It assigns stable IDs from symbolic names, constructs
typed facts and provenance, and supports controlled insertion order and run
identity. It does not parse JSON or EIR-T.

Synthetic scenarios cover:

* exact flow-depth, node, path, fact, and provenance budget boundaries;
* complete and truncated empty results;
* all four alias semantic values: `MUST_ALIAS`, `MAY_ALIAS`, `NO_ALIAS`, and
  `UNKNOWN_ALIAS`;
* supporting and contradicting facts for one predicate;
* missing and malformed provenance;
* mixed analysis runs and build variants;
* invalid local references and expression shapes; and
* legal non-canonical representation order.

## 6.3 Malformed representation corpus

Use small, reviewed inputs under:

```text
tests/fixtures/evidence/invalid/
    unsupported_schema.eir
    duplicate_member_id.eir
    dangling_reference.eir
    repeated_comparison.eir
    unterminated_string.eir
    malformed_evidence.pb
    stale_evidence_id.pb
```

Fuzzing may extend this corpus later, but fuzzing is not an acceptance
dependency for M10C.

## 6.4 Goldens

Only demonstration outputs are golden:

```text
tests/golden/evidence/overflow_unsafe.slice.json
tests/golden/evidence/overflow_unsafe.l0.eir
tests/golden/evidence/overflow_unsafe.l1.eir
tests/golden/evidence/overflow_unsafe.l1.eir.json
tests/golden/evidence/overflow_safe.l1.eir
tests/golden/evidence/overflow_truncated.l1.eir
```

Protobuf is decoded and compared semantically; raw Protobuf bytes are not a
golden or an identity oracle.

---

# 7. Contract Test Catalog (`AC`)

| ID and test | Scenario | Required oracle | Forbidden outcome |
| --- | --- | --- | --- |
| `AC-001 CompleteEmptyDiffersFromTruncatedEmpty` | Two empty `EvidenceFactSet` values differ only in metadata | Typed inequality and different diagnostic JSON; both validate for their declared states | Empty vectors treated as equivalent |
| `AC-002 RejectsInvalidResultMetadata` | Parameterized unspecified completeness, complete with reasons, truncated without reasons, missing run, missing query provenance | Each invalid value returns the stable expected status/code | Silent normalization or inferred defaults |
| `AC-003 ExactFactBudgetBoundaryIsComplete` | Backend contains exactly `max_facts_per_query` matching facts | All facts returned, `kComplete`, no truncation reason | Boundary incorrectly marked truncated |
| `AC-004 FactBudgetOverflowReturnsCanonicalPrefix` | Backend contains one more matching fact than the limit | Canonical prefix, `kTruncated`, `kMaxFacts`, examined count proves overflow | Backend-order-dependent prefix or hidden extra row |
| `AC-005 MetadataOrderingIsCanonical` | Same duplicated/unsorted truncation reasons and facts inserted in different order | Validation rejects duplicates; normalized valid inputs serialize identically | Insertion order changes JSON |
| `AC-006 EvidenceBuildInputJsonIsDeterministicAndComplete` | Fully populated input serialized twice and after independent-member reordering | Byte-identical JSON containing every field and one final newline | Omitted capacity, completeness, run, or provenance field |

---

# 8. Query Service Test Catalog (`QRY`)

| ID and test | Scenario | Required oracle | Forbidden outcome |
| --- | --- | --- | --- |
| `QRY-001 UnsafeDirectReturnsFlowRangeAndCapacity` | Real `evidence_overflow_unsafe` fixture | Flow reaches `memcpy.size`; range `[0,65535]`; capacity `2048`; all refs share one run | Source-text blob or missing capacity |
| `QRY-002 NoPathIsCompleteEmpty` | Synthetic disconnected graph with ample budget | Empty paths, `kComplete`, no reasons, valid query provenance | Empty result marked truncated or treated as error |
| `QRY-003 ReportsEachTraversalBudget` | Parameterized depth, node, and path overflow | Canonical partial result plus exact stable reason for each limit | Limit silently drops work or reports wrong reason |
| `QRY-004 RangeAndCapacityFactBudgetsAreVisible` | More range/capacity facts than fact limit | Canonical partial facts and `kMaxFacts` for each query independently | One query's truncation contaminates another result |
| `QRY-005 SafeCheckDominatesSink` | Real `evidence_overflow_safe` fixture | Returned check references exact condition and sink and has authoritative provenance | Mere lexical check match without dominance |
| `QRY-006 SiblingCheckDoesNotDominateSink` | Real `evidence_overflow_non_dominating` fixture | No positive dominance fact; complete scoped outcome or explicit incompleteness | Check existence promoted to dominance |
| `QRY-007 MixedPathsDoNotClaimUniversalCheck` | Real `evidence_overflow_mixed_paths` fixture | Checked and unchecked paths are both represented, or result is visibly truncated | One checked path hides unchecked path |
| `QRY-008 PreservesAllAliasStates` | Parameterized `MUST_ALIAS`, `MAY_ALIAS`, `NO_ALIAS`, and `UNKNOWN_ALIAS` facts | Exact alias semantic and epistemic values round-trip | `MAY_ALIAS` or `UNKNOWN_ALIAS` converted to `MUST_ALIAS`/`NO_ALIAS` |
| `QRY-009 OpaqueValidatorRemainsUnknown` | Real `evidence_overflow_opaque_validator` fixture without external model | Unknown result references callsite, requested property, and resolution hint | Assumed postcondition or negative check fact |
| `QRY-010 ProvenanceBudgetIsIndependentAndVisible` | Complete semantic facts with provenance depth below required closure | Facts remain present; provenance result truncates with its own reason | Fact disappearance or fabricated closed provenance |

---

# 9. Typed Handoff Test Catalog (`HND`)

| ID and test | Scenario | Required oracle | Forbidden outcome |
| --- | --- | --- | --- |
| `HND-001 BundlesEveryRequiredQueryResult` | Unsafe claim with complete service responses | Claim, flow, ranges, capacities, aliases, checks, unknowns, summaries, anchors, completion facts, and witnesses are present | M10C must issue an additional analysis or provenance query |
| `HND-002 UsesOneImmutableSnapshot` | Backend current run changes between subquery opportunities | Input is wholly from the pinned first snapshot or returns retryable failure | Mixed-run input returned as success |
| `HND-003 KeepsSupportingAndContradictingFactsSeparate` | Same predicate has support and counterevidence | Both collections retain IDs, epistemic states, and provenance | Conflict resolved by dropping one side |
| `HND-004 CarriesCompleteEmptyCheckEvidence` | Closed-world dominating-check query completes empty | Empty set retains scope, run, query provenance, examined count, and complete state | Bare empty vector interpreted as proof |
| `HND-005 CarriesTruncatedEmptyCheckEvidence` | Dominating-check query truncates before finding a result | Empty set retains reason and truncated state | Result indistinguishable from `HND-004` |
| `HND-006 UsesReferencesWithoutSourceTextAndIsDeterministic` | `evidence_overflow_summary` built in two checkout roots and insertion orders | Same semantic input and IDs; source anchors and summary refs present; no source body | Absolute path or source text changes handoff bytes |

---

# 10. Evidence Builder Test Catalog (`BLD`)

| ID and test | Scenario | Required oracle | Forbidden outcome |
| --- | --- | --- | --- |
| `BLD-001 UnsafeInputBuildsCompleteL1` | `HND-001` unsafe input | Valid L1 with claim, flow, range, capacity, scoped check outcome, unknown, summaries, provenance, and `PENDING` obligation | Verified verdict or missing causal member |
| `BLD-002 SafeInputRetainsCounterevidenceWithoutVerdict` | Real safe input | Dominating check appears as counterevidence/constraint; case remains initial authority state | `VERIFIED_SAFE` without verifier |
| `BLD-003 NonDominatingAndMixedPathsRemainUnsafeOrInconclusive` | Parameterized sibling check and mixed paths | No universal check claim; unchecked path or uncertainty visible | Any check treated as universal dominance |
| `BLD-004 OpaqueValidatorProducesBlockingUnknown` | Opaque external validation input | Unknown identifies property, subject, blocker, and suggested resolution | Assumption or MUST fact synthesized |
| `BLD-005 AliasUncertaintyDoesNotStrengthenCapacityReasoning` | Destination is `MAY_ALIAS`/`UNKNOWN_ALIAS` | Alias uncertainty and dependent proof uncertainty remain visible | MUST_ALIAS/NO_ALIAS or exact capacity conclusion invented |
| `BLD-006 TruncatedEmptyCheckCannotBecomeNegativeEvidence` | `HND-005` input | Unknown/omission records truncation; no `MUST_NOT` check fact | Negative absence or safety claim |
| `BLD-007 CompleteClosedWorldAbsenceRequiresQueryProvenance` | Complete empty check result with and without valid query provenance | Valid case may emit registered negative fact with derived provenance; missing provenance is rejected or unknown | Unproven complete flag creates negative fact |
| `BLD-008 L0ProjectionDeclaresEveryRemovedMember` | Build unsafe input at L0 | Claim, primary uncertainty/evidence refs, obligation, dependencies, and omissions for removed detail | Hidden L1/L2 member or dangling ref |
| `BLD-009 L2ProjectionPreservesDetailAndUnavailableExpansion` | Detailed input lacks one requested analyzer expansion | Available provenance/aliases/constraints retained; missing expansion is explicit and expandable | Fabricated detail or silent omission |
| `BLD-010 RejectsUnsupportedRelationAndMixedContext` | Parameterized unsupported predicate, mixed run, revision, build, and projection | Stable rejecting status identifies offending relation/member/context | Generic mapping, silent drop, or rebase |

---

# 11. Validation and Identity Test Catalog (`VID`)

| ID and test | Scenario | Required oracle | Forbidden outcome |
| --- | --- | --- | --- |
| `VID-001 MinimalInitialCaseIsValid` | Smallest legal `eir.v1` L0 case | No validation issues and computable Evidence ID | Optional presentation fields made mandatory |
| `VID-002 RejectsDuplicateAndDanglingReferences` | Parameterized duplicate local IDs and missing claim/entity/path/summary/provenance refs | Stable issue code and exact member ID | First duplicate overwrites prior member |
| `VID-003 RejectsDerivedFactWithoutProvenance` | Derived positive and negative facts lack provenance | `kMissingProvenance` for each | Serializer invents provenance |
| `VID-004 RejectsInvalidExpressionAndDisconnectedPath` | Wrong expression arity/type and non-adjacent path sequence | Stable typed/path issue | Parser/writer accepts structurally invalid object |
| `VID-005 EnforcesProofAuthority` | Non-pending proof result lacks result ID or verification producer; initial builder emits promoted state | Stable authority issue | Builder self-promotes proof or case state |
| `VID-006 RequiresVisibleOmissions` | L0/L1 reference removed detail without omission or uses non-expandable missing summary | Stable hidden-omission issue | Progressive disclosure loses auditability |
| `VID-007 IdentityIgnoresUnorderedConstructionAndLocalPaths` | Reverse independent members and change checkout root/project path | Equal canonical bytes and `EvidenceID` | Construction order/path affects identity |
| `VID-008 SemanticOrContextChangeChangesIdentity` | Change epistemic value, predicate, path order, dependency, analyzer version, run, or proof status one at a time | Every semantic mutation changes canonical bytes and `EvidenceID` | Distinct semantic cases collide |

---

# 12. Representation Test Catalog (`REP`)

| ID and test | Scenario | Required oracle | Forbidden outcome |
| --- | --- | --- | --- |
| `REP-001 CanonicalEirTextRoundTripsSemanticBytes` | Valid overflow case write/parse/write | Same canonical bytes, ID, and canonical text | Parser depends on original display label/whitespace |
| `REP-002 ProtobufRoundTripsSemanticBytes` | Valid overflow case encode/decode | Same canonical bytes and ID after validation | Protobuf wire bytes used as identity |
| `REP-003 AllRepresentationsShareEvidenceId` | Same case through EIR-T, Protobuf, and JSON | Parsed/decoded forms report one exact ID and semantic member set | Representation-specific ID or dropped member |
| `REP-004 FullJsonIsDeterministicAndComplete` | Reverse member insertion order and escape strings | Byte-identical JSON with every semantic field and final newline | M10B slice JSON reused or field omitted |
| `REP-005 RejectsUnsupportedSchemaAndUnknownRequiredSyntax` | Unsupported major schema and unrecognized required member | Stable rejection before semantic output | Best-effort downgrade |
| `REP-006 RejectsStaleOrMismatchedEvidenceId` | Encoded ID differs from recomputation in text/proto/domain value | Stable invalid-identity status | Trusts supplied ID |
| `REP-007 ReportsStableParserLocationsAndPrecedence` | Parameterized invalid escape, unterminated input, repeated comparison, and precedence cases | Exact one-based line/column and expected AST | Exception, ambiguous parse, or chained comparison |
| `REP-008 RejectsMalformedProtobufWithoutPartialCase` | Truncated bytes, unspecified enums, invalid stable IDs, dangling refs | Decode error and no returned case | Partially initialized semantic object |

---

# 13. Demonstration and CLI Test Catalog (`DEM`)

| ID and test | Scenario | Required oracle | Forbidden outcome |
| --- | --- | --- | --- |
| `DEM-001 SliceJsonCompatibility` | Run M10B overflow command before and after M10C formats | Existing `--format json` golden remains slice JSON and deterministic | `json` silently changes to full EIR |
| `DEM-002 EmitsValidatedL0L1L2` | Unsafe fixture at each level | Each output parses, validates, shares program binding, and obeys projection rules | Higher-detail members leak into L0 without omission |
| `DEM-003 TextJsonAndProtoAgree` | Unsafe L1 emitted in all M10C formats | Decode/parse all outputs and compare canonical bytes and ID | Golden text alone considered sufficient |
| `DEM-004 ProtobufOutputIsRequiredAndFailureAtomic` | Missing output, unwritable sibling, existing destination, and successful write | Clear error or atomic replacement; prior destination preserved on failure | Binary stdout or truncated destination |
| `DEM-005 UnsafeSafeAndTruncatedGoldensRemainDistinct` | Unsafe, safe, and truncated inputs | Three validated cases have expected semantic differences and reviewed goldens | Safe/truncated output byte-identical to unsafe case |
| `DEM-006 RepeatedRunsAndCheckoutRootsAreDeterministic` | Run full fixture twice in two materialized roots with fresh stores | Identical slice JSON, canonical EIR-T, full JSON, and Evidence ID | Absolute path, store order, or run timing changes output |

---

# 14. Four Required Demonstrations

These demonstrations are concise user-visible proofs built from the catalog.
Before each command, the integration harness creates a fresh analysis store
from the named isolated project fixture. Fixture selection belongs to test
setup, not the public `veritas-query` interface.

## 14.1 Unsafe complete evidence

```bash
veritas-query evidence overflow \
  --sink memcpy --level l1 --format eir-t
```

Demonstrates complete flow, `[0,65535]` range, 2048-byte capacity, scoped check
outcome, explicit unknown external semantics, provenance, summaries, and a
pending proof obligation. It does not claim a verified defect.

## 14.2 Safe counterevidence without premature verdict

```bash
veritas-query evidence overflow \
  --sink memcpy --level l1 --format eir-t
```

Demonstrates a positive dominating check and different Evidence ID while
remaining below `VERIFIED_SAFE` until a later verifier supplies authority.

## 14.3 Truncation blocks negative proof

```bash
veritas-query evidence overflow \
  --sink memcpy --level l1 --format eir-json \
  --max-paths 1
```

Demonstrates partial evidence, a stable path-budget reason, and an explicit
unknown/omission. No universal check or `MUST_NOT` fact may be emitted.

## 14.4 Cross-format semantic equivalence

```bash
veritas-query evidence overflow \
  --sink memcpy --level l1 --format eir-t
veritas-query evidence overflow \
  --sink memcpy --level l1 --format eir-json
veritas-query evidence overflow \
  --sink memcpy --level l1 --format protobuf \
  --output overflow.eir.pb
```

The test harness parses or decodes all three forms and compares canonical
semantic bytes and `EvidenceID`, not presentation bytes.

---

# 15. Planned Test Support and File Layout

```text
tests/support/evidence/EvidenceScenario.h
tests/support/evidence/EvidenceScenario.cpp

tests/fixtures/projects/evidence_overflow_*/...
tests/fixtures/evidence/invalid/...
tests/golden/evidence/...

tests/unit/evidence/EvidenceContractTest.cpp
tests/unit/evidence/EvidenceQueryServiceTest.cpp
tests/unit/evidence/EvidenceCaseBuilderTest.cpp
tests/unit/evidence/EvidenceValidatorTest.cpp
tests/unit/evidence/EvidenceCanonicalizerTest.cpp
tests/unit/evidence/EirLexerTest.cpp
tests/unit/evidence/EirParserTest.cpp
tests/unit/evidence/EirWriterTest.cpp
tests/unit/evidence/EvidenceProtoTest.cpp
tests/unit/evidence/EvidenceJsonTest.cpp

tests/integration/evidence/OverflowEvidenceFixtureTest.cpp
tests/integration/evidence/EvidenceHandoffIntegrationTest.cpp
tests/integration/evidence/VeritasQueryEvidenceTest.cpp
tests/integration/evidence/VeritasQueryEirTest.cpp
```

`EvidenceScenario` is test-only and may depend on public VERITAS types. Public
production headers never depend on test support.

---

# 16. CTest Registration and Execution

Register stable suite labels:

```text
evidence-unit
evidence-contract
evidence-integration
evidence-roundtrip
evidence-cli
```

Focused commands are:

```bash
ctest --test-dir build -L evidence-contract --output-on-failure
ctest --test-dir build -L evidence-integration --output-on-failure
ctest --test-dir build -L evidence-roundtrip --output-on-failure
ctest --test-dir build -L evidence-cli --output-on-failure
```

The milestone gate also runs:

```bash
ctest --test-dir build \
  -R "Evidence|Eir|VeritasQueryEir" \
  --no-tests=error --output-on-failure
ctest --test-dir build --output-on-failure
```

No catalog case may be disabled or skipped at milestone completion. An
environment prerequisite missing from local development must be a configure
failure or an explicitly approved repository-wide policy, not a silent test
skip.

---

# 17. Traceability

| Requirement | Governing cases |
| --- | --- |
| Complete empty differs from truncated empty | `AC-001`, `HND-004`, `HND-005`, `BLD-006`, `BLD-007` |
| Every query is bounded and deterministic | `AC-003`–`AC-006`, `QRY-002`–`QRY-004`, `QRY-010` |
| One immutable typed handoff; no JSON coupling | `HND-001`–`HND-006` |
| Supporting and contradicting evidence remain separate | `HND-003`, `BLD-001`, `BLD-002` |
| No epistemic strengthening | `QRY-008`, `QRY-009`, `BLD-002`–`BLD-007`, `VID-005` |
| Progressive L0/L1/L2 disclosure is auditable | `BLD-001`, `BLD-008`, `BLD-009`, `DEM-002` |
| Invalid or mixed evidence is rejected | `BLD-010`, `VID-002`–`VID-006`, `REP-005`–`REP-008` |
| Identity is semantic and content-addressed | `VID-007`, `VID-008`, `REP-001`–`REP-004`, `DEM-003`, `DEM-006` |
| Representation failures are deterministic and atomic | `REP-005`–`REP-008`, `DEM-004` |
| First demo is understandable and regression-safe | `QRY-001`, `BLD-001`, `DEM-001`–`DEM-006` |

---

# 18. Milestone Ownership

M10B owns:

```text
AC-001 through AC-006
QRY-001 through QRY-010
HND-001 through HND-006
DEM-001
```

M10C owns:

```text
BLD-001 through BLD-010
VID-001 through VID-008
REP-001 through REP-008
DEM-002 through DEM-006
```

M10C reruns the M10B-owned cases as compatibility tests. Ownership identifies
where a failure is fixed; it does not permit later milestones to stop running
the prerequisite contract.

---

# 19. Completion Gate

The API-to-Evidence-IR boundary is qualified only when:

* all 54 catalog cases are executable and passing;
* the four demonstrations produce reviewed, validated outputs;
* complete and truncated absence remain distinguishable through every layer;
* no forbidden epistemic promotion or hidden omission occurs;
* EIR-T, Protobuf, and JSON decode to identical canonical semantic bytes and
  Evidence ID;
* repeated clean runs and alternate checkout roots remain deterministic;
* focused Evidence suites and the complete repository suite pass;
* formatting and license checks pass; and
* M10B and M10C documentation and issue trackers reference the same case IDs
  and ownership boundary.
