# M10C Evidence IR Semantic Model and Serialization Design Spec

**Status:** Approved
**Milestone:** M10C
**Depends on:** M10B Evidence Builder input APIs and first demo
**Feeds:** Future Review Agent, verifier orchestration, and Evidence persistence

---

# 1. Purpose

M10C turns the compact, provenance-backed inputs produced by M10B into a typed
Evidence IR (EIR) case. It implements the deterministic boundary between an
`EvidenceQueryService` result and the representations consumed by later Review
Agent and proof-engine milestones.

M10C provides:

```text
M10B typed EvidenceBuildInput
    -> typed EvidenceCase
    -> validation
    -> canonical identity
    -> EIR-T / Protobuf / EIR diagnostic JSON
```

The milestone proves the first buffer-overflow slice can become a complete,
well-formed initial EIR case without an LLM. EIR-T syntax is governed by the
[formal specification](../veritas-evidence-ir-formal-specification.md), and EIR
semantics are governed by the
[Evidence IR architecture](../../architecture/04-evidence-ir-architecture.md).
This specification selects the architecture's initial V0.1 semantic subset for
the first executable `eir.v1` wire contract and defines its integration with
M10B.

---

# 2. Scope Boundary

## 2.1 In scope

M10C owns:

* the typed initial EIR semantic model;
* stabilization of EIR-T 1.0 so every in-scope semantic field has a lossless
  textual representation;
* deterministic assembly of EIR-L0, EIR-L1, and available EIR-L2 evidence from
  M10B inputs;
* typed mapping from M9/M10A facts into EIR predicates;
* well-formedness, type, provenance, epistemic, and reference validation;
* canonical ordering, semantic encoding, and `EvidenceID` calculation;
* canonical EIR-T parsing and writing;
* Protobuf encoding and decoding;
* deterministic diagnostic JSON for a full `EvidenceCase`;
* CLI formats that emit full EIR; and
* round-trip, canonicalization, negative-validation, and end-to-end golden
  tests.

## 2.2 Out of scope

M10C does not:

* implement the Review Agent or assemble LLM prompts;
* accept LLM output or promote hypotheses to authoritative facts;
* dispatch SMT, symbolic execution, static analysis, fuzzing, or tests;
* implement the verification state-transition engine;
* persist Evidence Cases in a new Evidence Store;
* implement cross-revision Evidence diff or incremental revalidation;
* change the Evidence IR epistemic, claim, proof, or producer-authority
  semantics;
* expand the M10A recursive domains;
* replace M10B semantic queries with EIR-layer graph analysis; or
* extend the initial subset to concurrency, ownership, state-machine, or domain-specific
  language features.

The semantic model includes proof obligations, verification state, dependency
references so later milestones can update them. M10C only constructs and
serializes their deterministic initial values.

---

# 3. Normative Inputs and Compatibility

The following documents have distinct authority:

| Contract | Authority |
| --- | --- |
| EIR meaning, epistemic policy, abstraction levels, and lifecycle | `docs/architecture/04-evidence-ir-architecture.md` |
| EIR-T lexical grammar, syntax, and well-formedness rules | `docs/specs/veritas-evidence-ir-formal-specification.md` |
| Evidence query and slice input contract | `docs/specs/milestones/m10b-evidence-builder-input-apis-demo-design-spec.md` |
| M10C implementation subset and integration choices | This specification |

The draft formal grammar does not currently serialize `schema_version`, EIR
level, overall case verification state, semantic dependencies, or explicit
omissions. Those concepts already exist in the EIR architecture or are required
for a lossless M10C representation. M10C therefore begins with a reviewed
grammar-stabilization task that adds exactly these textual forms:

```text
schema = "eir.v1";
level = l0 | l1 | l2;
state = UNREVIEWED | POSSIBLE_DEFECT | LIKELY_DEFECT |
        VERIFIED_DEFECT | LIKELY_FALSE_POSITIVE | VERIFIED_SAFE |
        INCONCLUSIVE;
dependency <id> { kind = <kind>; stable_id = "<StableId>"; }
omission <id> {
    kind = <qualified-kind>;
    subject = @<reference>;
    reason = "<reason>";
    expandable = true | false;
}
```

`schema`, `level`, and `state` are top-level case properties.
`dependency` and `omission` are evidence members. Their exact EBNF, keywords,
well-formedness rules, and canonical order must be added to the formal
specification before parser implementation begins. This is a syntax alignment,
not a change to the governing EIR semantics.

The same stabilization replaces the draft predicate grammar's left recursion
with explicit precedence: atoms and parenthesized expressions bind first,
followed by prefix `not`, non-associative comparisons, left-associative `and`,
left-associative `or`, and right-associative `implies`. A quantified predicate
owns the complete predicate following its colon. Repeated comparisons such as
`a < b < c` are rejected unless written as an explicit conjunction.

If implementation exposes any other ambiguity or contradiction between the
EIR architecture and formal grammar, the documents must be reconciled in a
separate reviewed documentation change. The parser and writer must not invent
silent syntax extensions.

M10C introduces `eir.v1` as the semantic schema version and
`veritas.evidence.v1` as the Protobuf package. EIR-T output declares the schema
as a top-level case property. Unsupported versions and unknown textual
constructs are rejected rather than silently ignored.

---

# 4. Initial Semantic Model (`eir.v1`)

An `EvidenceCase` is the executable form of:

```text
EC = <C, N, E, F, A, H, U, K, P, O, V>
```

The initial model contains:

```text
EvidenceCase {
    evidence_id
    schema_version
    level
    program_context
    primary_claim
    entities[]
    edges[]
    paths[]
    facts[]
    assumptions[]
    hypotheses[]
    unknowns[]
    constraints[]
    provenance[]
    proof_obligations[]
    summaries[]
    dependencies[]
    verification_state
    omissions[]
}
```

Every member uses a case-local identifier for EIR references and retains any
globally resolvable VERITAS stable ID separately. Local identifiers are syntax
handles; global IDs preserve cross-run and cross-revision identity.

## 4.1 Entities and relations

Initial entity kinds are:

```text
FUNCTION
CALLSITE
VALUE
MEMORY_OBJECT
BASIC_BLOCK
```

Initial relation kinds are:

```text
CALLS
FLOWS_TO
READS
WRITES
DOMINATES
MAY_ALIAS
```

Initial path kinds are `CALL`, `VALUE_FLOW`, and `CONTROL`. Summary edges retain
their immutable `FunctionSummaryID`, component references, and `expandable`
state. An unexpanded summary edge is never represented as a concrete path
segment without its omission marker.

## 4.2 Facts and expressions

Initial built-in fact predicates are:

```text
range
capacity
reachability
alias
memory_effect
```

The expression model is a typed abstract syntax tree rather than an opaque
string. The initial subset supports identifiers, integer and Boolean literals, references,
predicate calls, comparison, Boolean conjunction/disjunction/negation, and the
quantified forms already admitted by the formal grammar. Unsupported syntax is
a parse or validation error; it is not retained as untyped text.

Every fact has one explicit epistemic state:

```text
MUST
MAY
MUST_NOT
INFERRED
ASSUMED
UNKNOWN
```

Confidence remains independent of epistemic state. M10C copies authoritative
M9/M10A epistemic values and never strengthens them during assembly.

## 4.3 Claims, unknowns, and proof obligations

Exactly one primary claim is required. M10C accepts a typed M10B `ClaimSeed`
and constructs the initial claim predicate, subject, severity, and verification
state. It reuses M10B's `ClaimKind` and `Severity` types rather than declaring
parallel enums. The buffer-overflow demo uses the predicate:

```text
value(@copy_length) > capacity(@destination)
```

Missing semantic information becomes an explicit `Unknown` or `Omission`.
Absence of a fact is not converted to a negative fact. A missing dominating
check is represented as an explicit query result with its completeness state;
if the relevant query was truncated, the case records an unknown instead of
asserting that no check exists.

Proof obligations are typed goals with requested verifier kinds and an initial
`PENDING` status. M10C may construct deterministic obligations
from registered claim kinds, but it may not mark them `PROVED` or `REFUTED`.
The buffer-overflow finding seed initializes the overall case to
`POSSIBLE_DEFECT`; M10C never emits a `LIKELY_*` or `VERIFIED_*` state without
the later authority-bearing transition machinery.

## 4.4 Program context and dependencies

Every case is bound to exactly one repository, revision, build variant, target,
analysis configuration, and analyzer-version set. It records all summary IDs,
fact IDs, type-layout IDs, configuration IDs, and specification IDs used during
assembly.

M10C rejects mixed-run or mixed-context inputs. It does not silently rebase
facts or provenance from another revision.

---

# 5. M10B-to-M10C Assembly Contract

M10C consumes an immutable request:

```cpp
namespace veritas::evidence {

enum class EvidenceLevel { kL0, kL1, kL2 };
enum class QueryCompleteness { kComplete, kTruncated };

struct ClaimSeed {
  core::StableId finding_id;
  ClaimKind kind;
  Severity severity;
  core::StableId subject_ref;
  core::StableId source_ref;
  core::StableId sink_ref;
};

struct EvidenceFactSet {
  std::vector<facts::Fact> facts;
  QueryCompleteness completeness;
  std::vector<TruncationReason> truncation_reasons;
};

struct EvidenceBuildInput {
  ClaimSeed claim_seed;
  FlowSlice flow_slice;
  EvidenceFactSet ranges;
  EvidenceFactSet capacities;
  EvidenceFactSet aliases;
  EvidenceFactSet dominating_checks;
  EvidenceFactSet unknowns;
  facts::ProvenanceGraph provenance;
};

struct EvidenceBuildRequest {
  build::ProgramContext context;
  EvidenceBuildInput input;
  EvidenceLevel level;
};

class EvidenceCaseBuilder {
 public:
  StatusOr<EvidenceCase> Build(const EvidenceBuildRequest& request) const;
};

}  // namespace veritas::evidence
```

M10B exposes the typed `ClaimSeed` and `EvidenceBuildInput` handoff using
values already required in its diagnostic output. M10B must expose those
values without requiring M10C to parse diagnostic JSON. It must also expose
capacity results and a completeness/truncation state for every fact lookup;
the current vector-only M10B API cannot distinguish a complete empty result
from an incomplete empty result.

The M10B design and plan must be amended before implementation so ranges,
capacities, aliases, dominating checks, unknowns, and provenance can be bundled
into this immutable input. This is an additive handoff correction, not a move
of EIR assembly into M10B.

`EvidenceCaseBuilder` is an assembler, not a second analysis engine. It maps
the supplied slice and referenced M9/M10A facts through a versioned predicate
registry. It does not perform recursive reachability, alias analysis, range
propagation, dominance computation, or provenance reconstruction.

Assembly is deterministic:

1. validate the request context and truncation state;
2. collect entities and assign deterministic local references;
3. map typed facts without epistemic strengthening;
4. preserve supporting and contradicting evidence separately;
5. construct paths and retain summary expansion markers;
6. attach resolved provenance and dependency references;
7. materialize explicit unknowns and omissions;
8. create the claim-kind proof obligation;
9. project the requested EIR level;
10. validate, canonicalize, and calculate `EvidenceID`.

Any required fact or reference that cannot be mapped causes a typed `Status`
failure unless the governing EIR semantics requires an explicit unknown.
Unsupported relation names fail as `UNSUPPORTED`; they are never dropped.

---

# 6. EIR-L0, EIR-L1, and EIR-L2 Projection

All levels are projections of one semantic model, not separate wire schemas.

## 6.1 EIR-L0

L0 contains:

```text
program context identity
primary claim and subject
severity
primary supporting and contradicting facts
primary path reference
primary unknown or omission
verification state
```

References to omitted L1/L2 members remain expandable and identify their
source slice or semantic dependency.

## 6.2 EIR-L1

L1 adds the causal slice:

```text
interprocedural value-flow and call paths
control and dominance relations
memory relations
summary edges
constraints and assumptions
provenance roots
all relevant unknowns and truncation markers
```

The M10B buffer-overflow fixture must produce an input from which M10C builds a
complete L1 case.

## 6.3 EIR-L2

L2 adds detailed proof evidence already available through supplied M10B/M9
references, including expanded provenance DAGs, alias sets, constraints, and
analyzer expressions. M10C does not invoke a proof engine to manufacture L2
data. Unavailable expansions remain explicit omissions with stable expansion
references.

Requesting L2 therefore means "include all available detailed evidence," not
"claim the analysis is complete."

---

# 7. Validation

`EvidenceValidator` runs before identity calculation and before every
serialization operation. It returns `Status` with a stable error code, member
identifier, and source span when available.

Validation enforces at least:

1. exactly one primary claim;
2. one complete `ProgramContext`;
3. unique local identifiers;
4. resolution of every local or global reference;
5. explicit epistemic state for every fact;
6. provenance for every derived fact;
7. typed predicate and constraint expressions;
8. valid path endpoints and connected segments;
9. immutable, canonical summary references;
10. isolation of hypotheses from authoritative facts;
11. verification-producer identity for non-unknown proof results;
12. no mixed analysis runs or program revisions;
13. visible truncation and omitted expansions; and
14. schema-version compatibility.

Serializers refuse invalid cases. Parsers may construct an intermediate syntax
tree, but conversion to `EvidenceCase` succeeds only after semantic validation.

Because VERITAS builds with RTTI and exceptions disabled, parse and validation
failures use `Status`/`StatusOr<T>`. EIR-T diagnostics include line and column;
Protobuf diagnostics include the field path.

---

# 8. Canonicalization and Evidence Identity

`EvidenceCanonicalizer` produces one typed, domain-separated canonical byte
encoding. It:

* uses schema order for record fields;
* sorts semantically unordered collections by kind and stable identity;
* preserves path segment order and other semantically ordered lists;
* normalizes expression trees according to the formal EIR rules;
* excludes comments, source formatting, and diagnostic-only text;
* excludes the computed `evidence_id` and non-semantic local case label; and
* includes `eir.v1`, program context, analyzer versions, semantic payload,
  dependencies, omissions, and verification state.

Identity is:

```text
EvidenceID = evidence:sha256:SHA256(CanonicalEvidenceBytes)
```

M10C adds `core::IdKind::kEvidence` with the stable serialized spelling
`evidence`; `MakeStableId`, `ToString`, and `ParseStableId` remain the only ID
construction and parsing boundary.

The canonical EIR-T writer derives the top-level identifier from this digest,
so semantically equivalent input names do not change identity. Protobuf wire
ordering and non-canonical EIR-T whitespace are never hash inputs.

Parsing canonical EIR-T or Protobuf and re-canonicalizing must reproduce the
same bytes and `EvidenceID`. Reordering semantically unordered input members
must not change identity; changing epistemic state, context, a path, a
dependency, an omission, or verification state must change identity.

---

# 9. Serialization Formats

## 9.1 EIR-T

M10C implements a dependency-free, hand-written C++ lexer and recursive-descent
parser for the stabilized initial grammar. This avoids a new runtime dependency
and permits exception-free error handling.

The writer has two modes:

```text
canonical   identity-stable ordering and whitespace
pretty      deterministic human-readable formatting
```

Both modes are semantically equivalent. Only canonical mode participates in
canonical text fixtures.

## 9.2 Protobuf

The production schema is:

```text
proto/veritas/evidence/v1/evidence.proto
package veritas.evidence.v1
```

The Protobuf representation covers every initial semantic-model field. Domain
objects are validated before encoding and after decoding. Binary wire bytes are
not used directly as semantic identity because field order, unknown fields, and
library versions must not affect `EvidenceID`.

## 9.3 Diagnostic JSON

M10C JSON describes the full `EvidenceCase`, not the M10B `FlowSlice` envelope.
It uses deterministic field and member ordering and includes `schema_version`,
`evidence_id`, `level`, validation-visible omissions, and truncation.

M10B's existing `--format json` slice output remains compatible. The full EIR
JSON format is named `eir-json` so callers cannot confuse a diagnostic slice
with a validated Evidence Case.

---

# 10. CLI Contract

The M10B command remains valid:

```bash
veritas-query evidence overflow --sink memcpy --format json
```

M10C adds:

```bash
veritas-query evidence overflow --sink memcpy --level l1 --format eir-t
veritas-query evidence overflow --sink memcpy --level l1 --format eir-json
veritas-query evidence overflow --sink memcpy --level l1 \
    --format protobuf --output overflow.eir.pb
```

`--level` accepts only `l0`, `l1`, or `l2`. Binary Protobuf output requires an
explicit `--output` path and is never written to an interactive terminal.

CLI failures distinguish query failure, assembly failure, validation failure,
parse failure, unsupported schema version, and output failure. A failed command
must not leave a partial output file.

---

# 11. Planned Component Layout

```text
proto/veritas/evidence/v1/evidence.proto

include/veritas/evidence/EvidenceCase.h
include/veritas/evidence/EvidenceCaseBuilder.h
include/veritas/evidence/EvidencePredicateMapper.h
include/veritas/evidence/EvidenceValidator.h
include/veritas/evidence/EvidenceCanonicalizer.h
include/veritas/evidence/EirText.h
include/veritas/evidence/EvidenceProto.h
include/veritas/evidence/EvidenceJson.h

src/evidence/EvidenceCase.cpp
src/evidence/EvidenceCaseBuilder.cpp
src/evidence/EvidencePredicateMapper.cpp
src/evidence/EvidenceValidator.cpp
src/evidence/EvidenceCanonicalizer.cpp
src/evidence/EirLexer.cpp
src/evidence/EirParser.cpp
src/evidence/EirWriter.cpp
src/evidence/EvidenceProto.cpp
src/evidence/EvidenceJson.cpp
```

The semantic model, validation, canonicalization, and each representation
boundary remain separate units. Parsing does not perform analysis, and
serialization does not repair invalid semantic objects.

All new source, header, Protobuf, and CMake files use the repository's required
Apache-2.0 header. All C++ code remains compatible with `-fno-rtti` and
`-fno-exceptions`.

---

# 12. Acceptance Tests

M10C is complete only when all of the following are executable and passing:

```text
unsafe M10B overflow slice builds a valid EIR-L1 case
safe and unsafe fixtures remain semantically distinguishable
L0 contains the claim summary and explicit primary uncertainty
L1 contains flow, range, capacity, dominance, unknown, and provenance evidence
L2 preserves unavailable analyzer detail as explicit expandable omissions
every derived fact without provenance is rejected
dangling entity, path, summary, and fact references are rejected
mixed ProgramContext inputs are rejected
INFERRED and UNKNOWN inputs cannot become MUST during assembly
truncated no-check query cannot become a MUST_NOT check fact
canonicalization is invariant to unordered member insertion order
semantic changes alter EvidenceID
EIR-T parse/write/parse round-trip preserves canonical semantic bytes
Protobuf encode/decode preserves canonical semantic bytes
EIR-T and Protobuf forms produce the same EvidenceID
diagnostic EIR JSON is deterministic
unsupported schema major version is rejected
parse errors report stable line and column
binary CLI output is failure-atomic
overflow EIR-T output matches the canonical golden fixture
```

The canonical overflow golden case includes:

```text
one buffer-overflow claim
packet.length -> memcpy.size value-flow path
range(packet.length) = [0, 65535]
capacity(destination) = 2048
dominating-check evidence or an explicit completeness-qualified absence
unknown external validation
provenance and immutable summary references
one initial proof obligation with PENDING status
visible truncation and omissions
```

---

# 13. Milestone Handoff

M10B is complete when it produces deterministic evidence input slices. M10C is
complete when those slices can be assembled into validated, canonical,
content-addressed `eir.v1` and emitted as EIR-T, Protobuf, or full-EIR diagnostic
JSON.

The next Review Agent milestone may consume M10C through semantic operations
over `EvidenceCase`; it must not depend on parsing CLI text. A later verifier
orchestration milestone may update hypotheses, proof obligations, and
verification results only through the state-transition and producer-authority
rules defined by the Evidence IR architecture.

M10C does not claim the Agent or verifier feedback loop is implemented. It
provides the stable, typed, provenance-preserving boundary that makes that loop
possible.
