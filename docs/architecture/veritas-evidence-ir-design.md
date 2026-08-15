# VERITAS Evidence IR

## Formal Syntax and Semantic Specification

**Status:** Draft Architecture Specification
**Version:** 0.1
**Project:** VERITAS — Verified Evidence Reasoning IR for Trans-program Analysis and Semantics

---

# 1. Purpose

VERITAS Evidence IR, abbreviated **EIR**, is an intermediate representation for communicating **machine-derived evidence about program behavior** between:

* whole-program static analysis,
* Function Summary DB,
* Code Property Graph analysis,
* symbolic execution,
* SMT solvers,
* runtime/test evidence,
* specifications and architectural models,
* and LLM-based semantic Review Agents.

EIR is not intended to represent the complete program.

Its purpose is to represent:

> **a reviewable claim about a program, together with the minimum causal, semantic, and provenance information required to establish, refute, or further investigate that claim.**

The fundamental abstraction is therefore:

```text
EvidenceCase =
    Claim
  + Evidence
  + Constraints
  + Assumptions
  + Unknowns
  + Provenance
  + Proof Obligations
```

EIR sits between the program-analysis infrastructure and semantic reasoning:

```text
Source / AST / LLVM IR / CPG
              │
              ▼
      Function Summary DB
              │
              ▼
       Whole-Program Analysis
              │
              ▼
        Evidence Builder
              │
              ▼
        ┌─────────────┐
        │ Evidence IR │
        └──────┬──────┘
               │
       ┌───────┴────────┐
       ▼                ▼
 Review Agent      Proof Engines
       │                │
       └───────┬────────┘
               ▼
        Updated Evidence
```

---

# 2. Design principles

EIR SHALL follow the following principles.

## 2.1 Claim-oriented

An EIR object is constructed around a **specific proposition requiring review**.

Bad representation:

```text
Here are 20,000 CPG nodes.
```

Preferred representation:

```text
Claim:
    memcpy length may exceed destination capacity.

Evidence:
    external input → packet.len → copy length

Constraint:
    packet.len > 2048

Fact:
    destination capacity == 2048

Unknown:
    postcondition of validatePacket()
```

---

## 2.2 Evidence, not source code, is primary

Source code is referenced by stable source locations.

EIR primarily contains semantic objects such as:

```text
ValueFlow
CallPath
Constraint
MemoryEffect
AliasRelation
StateTransition
LockRelation
SummaryFact
```

rather than raw source text.

---

## 2.3 Explicit uncertainty

Every important statement SHALL distinguish certainty.

EIR SHALL NOT collapse:

```text
must
may
inferred
assumed
unknown
```

into a single boolean concept.

---

## 2.4 Provenance-preserving

Every non-trivial derived fact SHALL be traceable to:

```text
an analyzer,
a source artifact,
another fact,
a specification,
a runtime observation,
or an explicit hypothesis.
```

---

## 2.5 Compositional

EIR SHALL support references to Function Summary objects without expanding them.

Detailed evidence SHALL be expandable on demand.

---

## 2.6 Verification-safe

LLM-generated conclusions SHALL NOT automatically become verified program facts.

The semantic flow is:

```text
LLM hypothesis
      ↓
Proof Obligation
      ↓
Static/SMT/SE/Test
      ↓
Verified Fact
```

---

# 3. Terminology

The specification uses the following terms.

### Entity

A program or domain object.

Examples:

```text
function
instruction
value
memory object
lock
state
message
task
resource
```

### Fact

A proposition asserted by some evidence producer.

Example:

```text
range(packet.len, 0, 65535)
```

### Claim

The proposition under review.

Example:

```text
buffer_overflow(memcpy_site)
```

### Evidence

Information supporting or contradicting a claim.

### Assumption

A proposition accepted temporarily without proof.

### Hypothesis

A proposition proposed for investigation.

### Unknown

A semantic property whose value cannot currently be established.

### Proof Obligation

A formally expressible proposition requested to be verified.

### Provenance

An explanation of where a fact originated and how it was derived.

---

# 4. EIR semantic layers

EIR is divided into three abstraction levels.

```text
EIR-L0     Claim Summary
             │
             ▼
EIR-L1     Causal Evidence Slice
             │
             ▼
EIR-L2     Detailed Proof Evidence
```

## 4.1 EIR-L0

Designed for initial Agent inspection.

Contains:

```text
claim
subject
severity
primary facts
primary path
primary uncertainty
verification state
```

Target size:

```text
~100–500 semantic tokens
```

---

## 4.2 EIR-L1

Contains the causal program slice relevant to the claim.

Examples:

```text
interprocedural value flow
call path
control dependencies
summary edges
state transition path
memory dependencies
```

Target size:

```text
~1K–10K semantic tokens
```

---

## 4.3 EIR-L2

Contains detailed analyzer representations needed to establish proof.

Examples:

```text
SSA expressions
CFG fragments
symbolic constraints
alias sets
SMT expressions
derivation trees
```

EIR-L2 SHOULD normally be loaded only on demand.

---

# 5. Core semantic model

An Evidence Case is formally modeled as:

[
EC =
\langle
C,
N,
E,
F,
A,
H,
U,
K,
P,
O,
V
\rangle
]

where:

* (C) = claim,
* (N) = entity/node set,
* (E) = evidence relation set,
* (F) = fact set,
* (A) = assumption set,
* (H) = hypothesis set,
* (U) = unknown set,
* (K) = constraints,
* (P) = provenance graph,
* (O) = proof obligations,
* (V) = verification state.

The Evidence Case is therefore a typed heterogeneous graph plus logical propositions.

---

# 6. Top-level syntax

The canonical textual representation uses a declarative syntax called **EIR-T**.

A simplified example:

```text
evidence E103822 {

    claim C1 {
        kind       = buffer_overflow;
        severity   = high;
        subject    = @call.copy_ie.memcpy;

        predicate =
            value(@len) > capacity(@dst);
    }

    fact F1 {
        predicate =
            range(@len, 0, 65535);

        epistemic = must;
        source     = analysis.value_range;
    }

    fact F2 {
        predicate =
            capacity(@dst) == 2048;

        epistemic = must;
        source     = analysis.constant_propagation;
    }

    path P1 value_flow {
        @packet.len
        -> @decode.arg_len
        -> @copy.arg_len
        -> @call.copy_ie.memcpy.size;

        feasible = sat;
    }

    unknown U1 {
        property =
            postcondition(@function.validate_packet);
    }

    verify O1 {
        prove =
            forall path reaching @call.copy_ie.memcpy:
                value(@len) <= capacity(@dst);
    }
}
```

---

# 7. Lexical grammar

Identifiers:

```ebnf
Identifier      ::= Letter { Letter | Digit | "_" } ;
QualifiedId     ::= Identifier { "." Identifier } ;
Reference       ::= "@" QualifiedId ;
FactReference   ::= "$" Identifier ;
StringLiteral   ::= '"' { Character } '"' ;
IntegerLiteral  ::= ["-"] Digit { Digit } ;
BooleanLiteral  ::= "true" | "false" ;
```

Keywords are reserved.

Examples:

```text
evidence
claim
fact
assumption
hypothesis
unknown
entity
edge
path
constraint
verify
provenance
```

---

# 8. Top-level grammar

```ebnf
EvidenceCase ::=
    "evidence" Identifier "{"
        { EvidenceMember }
    "}" ;

EvidenceMember ::=
      Claim
    | EntityDecl
    | FactDecl
    | AssumptionDecl
    | HypothesisDecl
    | UnknownDecl
    | EdgeDecl
    | PathDecl
    | ConstraintDecl
    | ProvenanceDecl
    | VerificationDecl
    | ProofObligationDecl
    | SummaryReference
    ;
```

Every valid Evidence Case SHALL contain exactly one primary claim.

Secondary/subclaims MAY be introduced in later versions.

---

# 9. Entity syntax

```ebnf
EntityDecl ::=
    "entity" Identifier ":" EntityKind "{"
        { Property }
    "}" ;
```

Entity kinds include:

```text
function
parameter
return
value
variable

memory_object
memory_region
field

instruction
callsite
basic_block

type
global

thread
task
lock

state
state_machine

message
channel

resource
hardware_resource

specification
runtime_event
```

Example:

```text
entity memcpy_site : callsite {
    function = "memcpy";
    location = src("copy.cpp", 281, 5);
}

entity len : value {
    type = "size_t";
    origin = @packet_len;
}

entity dst : memory_object {
    allocation_site = @buffer_alloc;
}
```

---

# 10. Entity identity

All entities SHALL have globally stable identity within a repository revision.

Identity SHOULD be represented conceptually as:

[
EntityID =
Hash(
Repository,
BuildVariant,
SemanticIdentity
)
]

For functions, semantic identity SHOULD derive from:

```text
mangled name
canonical signature
linkage
template specialization
ABI/build variant
```

Source line numbers SHALL NOT be primary identity.

---

# 11. Predicate language

Facts, claims, assumptions, hypotheses, and proof obligations share a common predicate language.

```ebnf
Predicate ::=
      AtomicPredicate
    | "(" Predicate ")"
    | "not" Predicate
    | Predicate "and" Predicate
    | Predicate "or" Predicate
    | Predicate "implies" Predicate
    | Predicate ComparisonOp Predicate
    | QuantifiedPredicate
    ;

ComparisonOp ::=
      "=="
    | "!="
    | "<"
    | "<="
    | ">"
    | ">="
    ;

AtomicPredicate ::=
    Identifier "(" [ ArgumentList ] ")" ;
```

Example:

```text
value(@len) > capacity(@dst)
```

or:

```text
reachable(@entry, @sink)
and
not dominates(@bounds_check, @sink)
```

---

# 12. Quantifiers

EIR SHALL support limited quantification.

```ebnf
QuantifiedPredicate ::=
      "forall" Identifier "in" Domain ":" Predicate
    | "exists" Identifier "in" Domain ":" Predicate
    ;
```

Examples:

```text
forall p in paths(@entry, @sink):
    contains_bounds_check(p)
```

```text
exists p in feasible_paths(@source, @sink):
    value(@len) > capacity(@dst)
```

Quantifiers SHOULD normally be lowered to an appropriate solver/query backend rather than evaluated by the Review Agent.

---

# 13. Fact syntax

```ebnf
FactDecl ::=
    "fact" Identifier "{"
        "predicate" "=" Predicate ";"
        "epistemic" "=" EpistemicState ";"
        [ "confidence" "=" Confidence ";" ]
        [ "source" "=" Producer ";" ]
        [ "provenance" "=" Reference ";" ]
    "}" ;
```

Example:

```text
fact F17 {
    predicate =
        may_alias(@p, @q);

    epistemic = may;

    source =
        analysis.pointer;
}
```

---

# 14. Epistemic lattice

EIR distinguishes evidence state from confidence.

The fundamental epistemic classes are:

```text
MUST
MAY
MUST_NOT
INFERRED
ASSUMED
UNKNOWN
```

They have different semantics.

### MUST

Analyzer asserts proposition holds for all represented executions.

[
MUST(P) \Rightarrow \forall e \in Exec : P(e)
]

within the stated abstraction assumptions.

### MAY

There exists at least one execution in the analyzer's abstraction where the proposition holds.

[
MAY(P) \Rightarrow \exists e \in AbstractExec : P(e)
]

This does **not** necessarily imply concrete feasibility.

### MUST_NOT

The analyzer establishes the proposition cannot hold.

### INFERRED

Derived by a semantic/non-authoritative reasoning component.

Typical producer:

```text
LLM
heuristic engine
machine-learning classifier
```

### ASSUMED

Accepted as an external premise.

### UNKNOWN

Analysis cannot establish a meaningful answer.

---

# 15. Epistemic propagation

Derived facts SHALL conservatively preserve epistemic strength.

For example:

```text
MUST(P)
MUST(P → Q)
----------------
MUST(Q)
```

But:

```text
MAY(P)
MUST(P → Q)
---------------
MAY(Q)
```

An inferred premise cannot directly yield a verified fact:

```text
INFERRED(P)
MUST(P → Q)
----------------
INFERRED(Q)
```

not:

```text
MUST(Q)
```

This rule is fundamental to VERITAS soundness.

---

# 16. Confidence

Confidence and epistemic state are independent.

Suggested confidence values:

```text
exact
high
medium
low
unknown
```

Example:

```text
epistemic = MAY
confidence = high
```

means:

> the analyzer is highly confident that this is a valid may-property.

It does not mean the property is definitely realized at runtime.

---

# 17. Evidence relations

Evidence is represented as a typed directed multigraph:

[
G_E = (N,E)
]

where:

[
E \subseteq N \times RelationKind \times N
]

Core relation kinds include:

```text
CALLS
MAY_CALL

DEF
USE
FLOWS_TO

READS
WRITES

MAY_ALIAS
MUST_ALIAS

CONTROLS
DOMINATES
POST_DOMINATES

ALLOCATES
FREES
ESCAPES

ACQUIRES
RELEASES

TRANSITIONS

SPAWNS
RUNS_ON

SENDS
RECEIVES

SUPPORTED_BY
CONTRADICTED_BY
DERIVED_FROM

SUMMARIZED_BY
EXPANDS_TO
```

---

# 18. Edge syntax

```ebnf
EdgeDecl ::=
    "edge" Identifier "{"
        "from" "=" Reference ";"
        "to"   "=" Reference ";"
        "kind" "=" RelationKind ";"
        [ "condition" "=" Predicate ";" ]
        [ "epistemic" "=" EpistemicState ";" ]
        [ "provenance" "=" Reference ";" ]
    "}" ;
```

Example:

```text
edge E17 {
    from = @packet_len;
    to   = @copy_len;

    kind = FLOWS_TO;

    condition =
        @packet.type == EXTENSION;

    epistemic = must;
}
```

---

# 19. Value-flow semantics

A value-flow edge:

```text
x FLOWS_TO y
```

means that information contained in (x) may contribute to the value of (y).

It does not necessarily imply equality.

Formally:

[
Flows(x,y)
]

represents a dependency relation.

Specific transfer semantics MAY be attached:

```text
transfer =
    y = truncate(x, 16)
```

or:

```text
transfer =
    y = x + 1
```

---

# 20. Summary edges

Interprocedural evidence SHOULD normally use summarized edges.

Example:

```text
edge E41 {
    from = @decode.arg0;
    to = @decode.ret;

    kind = FLOWS_TO;

    summary = summary("decode", "53af...");
}
```

The summary edge logically stands for a subgraph:

[
SummaryEdge \equiv G_{internal}
]

but the internal graph is omitted until expanded.

This supports context-efficient Agent reasoning.

---

# 21. Path syntax

```ebnf
PathDecl ::=
    "path" Identifier PathKind "{"
        PathExpression
        [ "conditions" "{" { Predicate ";" } "}" ]
        [ "feasible" "=" Feasibility ";" ]
        [ "provenance" "=" Reference ";" ]
    "}" ;
```

Path kinds:

```text
call
control
data_flow
value_flow
taint
ownership
memory
state
lock
message
resource
```

Example:

```text
path P1 value_flow {

    @packet.len
        -> @parse.len
        -> @decode.len
        -> @copy.len
        -> @memcpy.size;

    conditions {
        @packet.type == EXTENSION;
        @packet.version >= 2;
    }

    feasible = sat;
}
```

---

# 22. Path feasibility

Possible values:

```text
PROVED_FEASIBLE
SAT
MAYBE
UNTESTED
UNSAT
PROVED_INFEASIBLE
UNKNOWN
```

The distinction matters.

For example:

```text
MAYBE
```

can result from static over-approximation.

```text
SAT
```

typically means an SMT/symbolic model was found.

```text
PROVED_FEASIBLE
```

may require concrete replay or stronger proof evidence.

---

# 23. Constraints

Constraints represent predicates controlling the evidence.

```ebnf
ConstraintDecl ::=
    "constraint" Identifier "{"
        "expr" "=" Predicate ";"
        [ "scope" "=" Scope ";" ]
        [ "epistemic" "=" EpistemicState ";" ]
        [ "provenance" "=" Reference ";" ]
    "}" ;
```

Constraint scopes include:

```text
global
function
path
basic_block
callsite
entity
```

Example:

```text
constraint K1 {
    expr =
        @len >= 0 and @len <= 65535;

    scope =
        path(@P1);

    epistemic =
        must;
}
```

---

# 24. Assumptions

```ebnf
AssumptionDecl ::=
    "assumption" Identifier "{"
        "predicate" "=" Predicate ";"
        "source" "=" AssumptionSource ";"
        [ "scope" "=" Scope ";" ]
    "}" ;
```

Examples:

```text
assumption A1 {
    predicate =
        caller_holds(@ue_lock);

    source =
        specification("RRC-ARCH-214");
}
```

or:

```text
assumption A2 {
    predicate =
        interrupts_disabled(@function.foo);

    source =
        analyzer_configuration;
}
```

Assumptions SHALL remain visible in derived conclusions.

---

# 25. Hypotheses

Hypotheses are explicitly non-authoritative propositions.

```ebnf
HypothesisDecl ::=
    "hypothesis" Identifier "{"
        "predicate" "=" Predicate ";"
        "producer" "=" Producer ";"
        [ "reason" "=" StringLiteral ";" ]
        [ "confidence" "=" Confidence ";" ]
    "}" ;
```

Example:

```text
hypothesis H1 {
    predicate =
        postcondition(@validate) implies
        @len <= capacity(@dst);

    producer =
        llm.review_agent;

    confidence =
        medium;
}
```

---

# 26. Unknowns

Unknowns SHALL be first-class objects.

```ebnf
UnknownDecl ::=
    "unknown" Identifier "{"
        "property" "=" Predicate ";"
        "reason" "=" UnknownReason ";"
        [ "blocking" "=" ReferenceList ";" ]
        [ "suggested_resolution" "=" ResolutionAction ";" ]
    "}" ;
```

Unknown reasons include:

```text
UNRESOLVED_CALL
UNKNOWN_ALIAS
EXTERNAL_FUNCTION
MISSING_SPECIFICATION
ANALYSIS_TIMEOUT
STATE_EXPLOSION
UNSUPPORTED_LANGUAGE_FEATURE
INLINE_ASSEMBLY
DYNAMIC_LOADING
UNKNOWN_BUILD_CONFIGURATION
```

Example:

```text
unknown U17 {
    property =
        postcondition(@vendor_validate);

    reason =
        EXTERNAL_FUNCTION;

    suggested_resolution =
        infer_contract(@vendor_validate);
}
```

---

# 27. Claim syntax

```ebnf
Claim ::=
    "claim" Identifier "{"
        "kind" "=" ClaimKind ";"
        "subject" "=" Reference ";"
        "predicate" "=" Predicate ";"
        "severity" "=" Severity ";"
        [ "description" "=" StringLiteral ";" ]
    "}" ;
```

Typical claim kinds:

```text
buffer_overflow
null_dereference
use_after_free
memory_leak

data_race
deadlock
lock_order_violation

taint_flow
injection
privilege_violation

state_violation
protocol_violation

deadline_violation
resource_violation

architecture_violation

semantic_regression
```

EIR does not restrict implementations to this predefined list.

Projects MAY define domain-specific claim namespaces.

Example:

```text
ran.harq_state_violation
```

---

# 28. Claim semantics

A claim is not automatically true merely because an analyzer emitted it.

The claim represents a proposition:

[
C : Program \rightarrow {true,false,unknown}
]

The purpose of evidence analysis is to determine the strongest justified verification state for (C).

---

# 29. Evidence supporting and contradicting claims

Evidence MAY explicitly declare logical relationships.

```text
edge ES1 {
    from = @F1;
    to = @C1;
    kind = SUPPORTED_BY;
}
```

Conceptually:

```text
Fact F1 ─────────────┐
                     │ SUPPORT
Fact F2 ─────────────┼────► Claim C1
                     │
Path P1 ─────────────┘

Contract F7 ─── CONTRADICT ───► Claim C1
```

This allows competing interpretations to coexist.

---

# 30. Provenance

Provenance is represented as a derivation DAG.

```ebnf
ProvenanceDecl ::=
    "provenance" Identifier "{"
        "producer" "=" Producer ";"
        [ "rule" "=" StringLiteral ";" ]
        [ "inputs" "=" ReferenceList ";" ]
        [ "location" "=" SourceLocation ";" ]
        [ "version" "=" StringLiteral ";" ]
        [ "configuration" "=" StringLiteral ";" ]
    "}" ;
```

Example:

```text
provenance PR17 {
    producer =
        analysis.interproc_valueflow;

    rule =
        "parameter-return propagation";

    inputs =
        [$F3, $F5, $F9];

    version =
        "veritas-vfg-0.4";
}
```

---

# 31. Provenance semantics

Given:

```text
F3
F5
F9
 │
 └──── rule R17 ───► F17
```

the derivation is represented as:

[
F_3,F_5,F_9 \vdash_{R17} F_{17}
]

The complete proof provenance of (F_{17}) is recursively defined as the provenance closure of all input facts.

The operation:

```text
explain(F17)
```

SHALL return a finite provenance subgraph.

---

# 32. Proof obligations

Proof obligations turn Agent hypotheses into deterministic analysis requests.

Syntax:

```ebnf
ProofObligationDecl ::=
    "verify" Identifier "{"
        VerificationGoal
        [ "using" "=" VerificationBackendList ";" ]
        [ "budget" "=" ResourceBudget ";" ]
    "}" ;
```

Example:

```text
verify O17 {

    prove =
        forall p in feasible_paths(@entry, @memcpy):
            @len <= capacity(@dst);

    using =
        [range_analysis, smt, symbolic_execution];
}
```

---

# 33. Proof obligation semantics

A proof obligation has state:

[
O = \langle P,S,R\rangle
]

where:

* (P) = proposition,
* (S) = status,
* (R) = result evidence.

Possible statuses:

```text
PENDING
PROVED
REFUTED
UNKNOWN
TIMEOUT
UNSUPPORTED
```

For:

```text
PROVED
```

the resulting proposition MAY be promoted to a verified fact.

For:

```text
REFUTED
```

the result SHOULD contain counterexample evidence when available.

---

# 34. Verification state

The Evidence Case has an overall state.

```text
UNREVIEWED

POSSIBLE_DEFECT

LIKELY_DEFECT

VERIFIED_DEFECT

LIKELY_FALSE_POSITIVE

VERIFIED_SAFE

INCONCLUSIVE
```

These states are not probabilities.

They describe epistemic status.

---

# 35. State transition semantics

Typical transitions:

```text
UNREVIEWED
    ↓
POSSIBLE_DEFECT
```

after a static finding.

```text
POSSIBLE_DEFECT
    ↓
LIKELY_DEFECT
```

after semantic/contextual evidence strengthens the claim.

```text
LIKELY_DEFECT
    ↓
VERIFIED_DEFECT
```

only after deterministic verification or concrete reproduction.

Similarly:

```text
POSSIBLE_DEFECT
    ↓
LIKELY_FALSE_POSITIVE
```

may be triggered by semantic reasoning.

But:

```text
LIKELY_FALSE_POSITIVE
    ↓
VERIFIED_SAFE
```

requires proof or sufficiently authoritative evidence according to project policy.

An LLM alone SHALL NOT perform that transition.

---

# 36. Formal evidence judgment

Let:

[
\Gamma
]

represent verified facts,

[
A
]

assumptions,

and:

[
H
]

hypotheses.

The principal verification judgment is:

[
\Gamma ; A \vdash C
]

meaning:

> claim (C) follows from verified facts under assumptions (A).

A hypothesis is not part of the proof context:

[
H \not\subseteq \Gamma
]

unless promoted through verification.

For example:

[
\Gamma \vdash len \in [0,65535]
]

[
\Gamma \vdash capacity(dst)=2048
]

[
\Gamma \vdash \exists p :
Reachable(p) \land len > 2048
]

therefore:

[
\Gamma \vdash MayOverflow(dst,len)
]

---

# 37. Conditional evidence

Facts MAY contain guards.

Example:

```text
fact F21 {
    predicate =
        (@ret == SUCCESS)
        implies
        (@state == CONNECTED);

    epistemic =
        must;
}
```

Semantically:

[
ret=SUCCESS \Rightarrow state=CONNECTED
]

This is preferable to flattening the result into:

```text
state = CONNECTED
```

---

# 38. Path-sensitive effects

Effects MAY be guarded.

Example:

```text
effect {
    guard =
        @len <= 128;

    write =
        @ctx.state := VALID;
}
```

Semantically:

[
guard \Rightarrow effect
]

The Evidence Builder SHOULD preserve such guards during summary expansion.

---

# 39. Function Summary references

EIR SHALL not duplicate the SummaryDB.

Instead:

```ebnf
SummaryReference ::=
    "summary" Identifier "{"
        "function" "=" Reference ";"
        "summary_id" "=" StringLiteral ";"
        [ "components" "=" SummaryComponentList ";" ]
    "}" ;
```

Example:

```text
summary S_decode {

    function =
        @function.decodeIE;

    summary_id =
        "sha256:e12f...";

    components =
        [value_flow, range, memory];
}
```

---

# 40. Summary expansion

Given:

```text
SummaryEdge(F)
```

the operation:

```text
expand(F, component)
```

returns an Evidence Graph fragment.

The expansion SHALL preserve semantic equivalence:

[
SummaryEdge(F) \equiv Abstract(G_F)
]

where (G_F) is the expanded internal graph.

---

# 41. Evidence slicing

An Evidence Case SHOULD represent the smallest causally relevant subgraph.

Given full program graph:

[
G_P
]

and claim (C),

the Evidence Builder computes:

[
Slice(G_P,C,B)
]

where (B) is an analysis budget.

The slice SHOULD preserve all relations necessary to evaluate the claim within the selected abstraction.

Common slicing directions:

```text
backward value slice
forward impact slice
call-chain slice
control-dependence slice
state-machine slice
ownership slice
lock-dependence slice
```

---

# 42. Minimality is not absolute

Producing the mathematically smallest evidence graph may be computationally expensive.

Therefore EIR defines **semantic relevance** rather than strict minimality.

A valid Evidence Slice SHOULD:

1. contain all primary claim entities,
2. preserve all selected support/contradiction paths,
3. preserve required constraints,
4. preserve provenance,
5. explicitly mark omitted expansions.

---

# 43. Omitted evidence

Omitted detail SHALL be represented explicitly.

Example:

```text
edge E19 {
    from = @foo.arg0;
    to = @foo.ret;

    kind =
        FLOWS_TO;

    summarized_by =
        @S_foo;

    expandable =
        true;
}
```

This avoids pretending that the visible Evidence Graph is the complete program.

---

# 44. Program revision semantics

Every Evidence Case SHALL be bound to a program snapshot.

```text
ProgramContext {
    repository
    revision
    build_variant
    target
    analyzer_configuration
}
```

Therefore:

[
EC(P_1) \neq EC(P_2)
]

unless explicitly rebased.

Evidence from different revisions SHALL NOT silently mix.

---

# 45. Evidence stability

Evidence SHOULD reference immutable semantic IDs whenever possible.

For example:

```text
FunctionSummaryID
FactID
EntityID
```

rather than unstable textual positions.

This enables:

```text
evidence diff
semantic regression analysis
cross-commit reuse
```

---

# 46. Evidence diff

Given:

[
EC_1, EC_2
]

for two program revisions, define:

[
\Delta EC = EC_2 - EC_1
]

with categories:

```text
AddedFact
RemovedFact
ChangedFact

AddedPath
RemovedPath

ConstraintStrengthened
ConstraintWeakened

UnknownResolved
NewUnknown

ClaimStrengthened
ClaimWeakened
```

This is particularly useful for pull-request review.

---

# 47. Example: complete overflow case

```text
evidence Overflow_001 {

    context {
        repository =
            "radio-stack";

        revision =
            "a87f03e";

        build_variant =
            "ARM64_RELEASE";
    }

    entity len : value {
        origin =
            @packet.length;
    }

    entity dst : memory_object {
        allocation_site =
            @allocate_payload_buffer;
    }

    entity sink : callsite {
        function =
            "memcpy";

        location =
            src("decoder.cpp", 281, 9);
    }

    claim C1 {

        kind =
            buffer_overflow;

        subject =
            @sink;

        predicate =
            value(@len) > capacity(@dst);

        severity =
            high;
    }

    fact F1 {
        predicate =
            range(@len, 0, 65535);

        epistemic =
            must;

        provenance =
            @PR1;
    }

    fact F2 {
        predicate =
            capacity(@dst) == 2048;

        epistemic =
            must;

        provenance =
            @PR2;
    }

    fact F3 {
        predicate =
            not dominates(
                @validate_length,
                @sink
            );

        epistemic =
            must;

        provenance =
            @PR3;
    }

    path P1 value_flow {

        @packet.length
          -> @parse.length
          -> @decode.length
          -> @copy.length
          -> @sink.size;

        conditions {
            @packet.type == EXTENSION;
            @packet.version >= 2;
        }

        feasible =
            sat;
    }

    unknown U1 {

        property =
            postcondition(
                @vendor_validate
            );

        reason =
            EXTERNAL_FUNCTION;

        suggested_resolution =
            infer_contract(
                @vendor_validate
            );
    }

    verify O1 {

        prove =
            exists p in
            feasible_paths(
                @entry,
                @sink
            ):
                value(@len)
                >
                capacity(@dst);

        using =
            [smt, symbolic_execution];
    }

    provenance PR1 {
        producer =
            analysis.value_range;

        version =
            "0.3";
    }

    provenance PR2 {
        producer =
            analysis.memory_object;

        version =
            "0.5";
    }

    provenance PR3 {
        producer =
            analysis.dominator;

        version =
            "0.2";
    }
}
```

---

# 48. Example: Agent-generated hypothesis

After examining the evidence, the Review Agent might add:

```text
hypothesis H1 {

    predicate =
        success(@vendor_validate)
        implies
        @len <= capacity(@dst);

    producer =
        llm.review_agent;

    reason =
        "Name, callers and nearby checks suggest this API performs length validation.";

    confidence =
        medium;
}
```

This SHALL NOT modify F1/F2/F3.

Instead the Agent generates:

```text
verify O2 {

    prove =
        success(@vendor_validate)
        implies
        @len <= capacity(@dst);

    using =
        [
            summary_analysis,
            symbolic_execution,
            specification_search
        ];
}
```

Suppose O2 returns:

```text
REFUTED
```

with:

```text
counterexample:
    len = 4096
    capacity = 2048
    vendor_validate = SUCCESS
```

Then the Evidence Case becomes substantially stronger.

---

# 49. Domain extension mechanism

EIR SHALL support extension namespaces.

For example:

```text
ran.task
ran.slot
ran.harq_process
ran.ue_context

ran.transitions
ran.deadline
ran.assigned_core
```

A RAN-specific predicate might be:

```text
ran.deadline(@task) <= 100us
```

or:

```text
ran.allowed_transition(
    @HARQ_WAIT_ACK,
    @HARQ_RETX
)
```

The core EIR semantics remain unchanged.

---

# 50. Type system

Predicates SHALL be type checked.

For example:

```text
capacity(memory_object) -> integer

range(value) -> interval

calls(function,function) -> bool

dominates(program_point,program_point) -> bool

holds_lock(thread,lock) -> bool
```

Therefore:

```text
capacity(@function.foo)
```

is invalid.

Likewise:

```text
dominates(@memory_object.x, @function.foo)
```

is ill-typed.

---

# 51. Primitive semantic types

Initial primitive types SHOULD include:

```text
Bool

Int
UInt
BitVector<N>

String

Address

Interval<T>

SourceLocation

FunctionRef
ValueRef
MemoryRef
TypeRef

ThreadRef
LockRef

StateRef
ResourceRef

PathRef
FactRef
SummaryRef
```

---

# 52. Symbolic expressions

For integration with SMT/symbolic execution, expressions SHOULD preserve machine semantics.

Example:

```text
bvadd<32>(x,y)
```

rather than:

```text
x + y
```

when overflow semantics matter.

The IR therefore SHOULD eventually support:

```text
signed integers
unsigned integers
bitvectors
floating-point
pointer expressions
memory-region expressions
```

---

# 53. Memory model

EIR SHOULD reason about abstract memory objects rather than raw addresses.

Examples:

```text
global("gScheduler")

argument_object(foo,0)

field(
    argument_object(foo,1),
    "state"
)

heap_object(
    allocation_site("foo.cpp",128)
)
```

Relations include:

```text
reads(F,M)

writes(F,M)

aliases(M1,M2)

owns(F,M)

frees(F,M)

escapes(M,F)
```

---

# 54. Concurrency semantics

Concurrency extensions SHOULD support:

```text
may_happen_in_parallel(A,B)

happens_before(A,B)

acquires(F,L)

releases(F,L)

holds(F,L)

lock_order(L1,L2)
```

Example race claim:

[
MHP(W_1,R_2)
\land
AccessSameMemory(W_1,R_2)
\land
\neg CommonProtection(W_1,R_2)
]

---

# 55. State semantics

State transitions are represented as:

[
Transition(M,S_1,S_2,G,E)
]

where:

* (M) = state machine,
* (S_1) = source state,
* (S_2) = target state,
* (G) = guard,
* (E) = effect.

Example:

```text
transition {
    machine = @ue_state;
    from = IDLE;
    to = CONNECTED;

    guard =
        setup_success;

    effect =
        allocate_context;
}
```

---

# 56. Review Agent contract

The Review Agent SHALL consume EIR through semantic operations rather than requiring full serialization into one prompt.

Required API concepts:

```text
get_case(id)

get_claim(id)

get_primary_evidence(id)

expand_summary(id)

expand_path(id)

explain_fact(id)

get_unknowns(id)

get_assumptions(id)

get_conflicts(id)

request_source(entity)

request_proof(predicate)

add_hypothesis(predicate)

create_proof_obligation(predicate)
```

---

# 57. Agent write permissions

The Agent MAY create:

```text
hypotheses
review annotations
candidate contracts
candidate claims
proof obligations
evidence requests
```

The Agent SHALL NOT directly create:

```text
VERIFIED_FACT
PROVED
VERIFIED_SAFE
VERIFIED_DEFECT
```

without an authoritative verifier result.

---

# 58. Evidence producer hierarchy

Example producer classes:

```text
compiler

static_analysis
    callgraph
    value_flow
    pointer
    range
    taint
    ownership
    concurrency

formal
    smt
    model_checker

dynamic
    runtime_trace
    test
    fuzzing

specification
    API_contract
    architecture_spec

semantic
    llm
    heuristic
```

Each producer SHOULD have a trust policy.

---

# 59. Trust policy

A VERITAS deployment MAY define:

```text
TrustLevel 3:
    compiler semantics
    formal proof
    concrete replay

TrustLevel 2:
    sound static analysis

TrustLevel 1:
    unsound heuristic analysis
    specification extraction

TrustLevel 0:
    LLM inference
```

This hierarchy is deployment-specific and SHALL NOT be hard-coded into the language semantics.

---

# 60. Well-formedness rules

An Evidence Case is well formed only if:

1. exactly one primary claim exists;
2. every referenced entity exists;
3. every fact has epistemic state;
4. every derived fact has provenance;
5. all expressions are type-correct;
6. all paths reference valid graph entities;
7. summary references identify immutable summaries;
8. hypotheses cannot masquerade as verified facts;
9. program revision context is defined;
10. proof results identify their verification producer.

---

# 61. Referential integrity

For every reference:

[
@x
]

there SHALL exist a matching entity or globally resolvable semantic object.

Dangling references invalidate the EIR module.

---

# 62. Determinism

Given identical:

```text
program snapshot
build variant
analysis configuration
analyzer versions
```

the deterministic portion of Evidence IR SHOULD be reproducible.

LLM-generated hypotheses are explicitly outside this guarantee.

---

# 63. Serialization

The canonical internal representation SHOULD use a structured binary format such as Protobuf.

Recommended architecture:

```text
EIR semantic model
     │
     ├── Protobuf binary
     ├── EIR-T textual form
     └── JSON diagnostic form
```

The textual form is intended for:

```text
debugging
testing
documentation
human inspection
```

The binary form is intended for production infrastructure.

---

# 64. Canonical representation

Serialization order SHALL NOT affect semantic identity.

Evidence objects SHOULD therefore support canonical hashing:

[
EvidenceID =
Hash(
CanonicalRepresentation(Evidence)
)
]

This enables:

```text
deduplication
caching
incremental comparison
cross-agent references
```

---

# 65. Evidence lifecycle

The standard lifecycle is:

```text
Finding generated
      ↓
Evidence Builder
      ↓
EIR-L0
      ↓
Agent inspection
      ↓
EIR-L1 expansion
      ↓
Hypothesis
      ↓
Proof Obligation
      ↓
Verification
      ↓
New Facts / Counterexample
      ↓
Evidence Case update
      ↓
Conclusion
```

---

# 66. Incremental EIR updates

Evidence SHALL be incrementally maintainable.

When a Function Summary changes:

```text
SummaryDelta
      ↓
Dependency Index
      ↓
Affected Evidence Cases
      ↓
Revalidate Evidence Slice
```

Cases MAY be classified:

```text
UNCHANGED

STALE

PARTIALLY_STALE

INVALID

REBUILT
```

---

# 67. Evidence dependencies

Every Evidence Case SHOULD record semantic dependencies:

```text
summary IDs
fact IDs
type-layout IDs
configuration IDs
specification IDs
```

Example:

```text
depends_on {
    summary("foo", A12)
    summary("bar", B17)
    type_layout(Packet, C19)
}
```

This enables precise invalidation.

---

# 68. Evidence equivalence

Two Evidence Cases may differ syntactically while representing the same semantic argument.

Define approximate semantic equivalence:

[
EC_1 \simeq EC_2
]

when they have:

```text
equivalent claim predicates
equivalent relevant facts
equivalent causal relationships
equivalent assumptions
```

Exact automated equivalence is not required for V1.

Hash equality provides only structural equality.

---

# 69. Initial implementation subset

EIR V0.1 SHOULD initially implement only:

### Entities

```text
function
callsite
value
memory_object
basic_block
```

### Relations

```text
CALLS
FLOWS_TO
READS
WRITES
DOMINATES
MAY_ALIAS
```

### Facts

```text
range
capacity
reachability
alias
memory_effect
```

### Paths

```text
call
value_flow
control
```

### Epistemic states

```text
MUST
MAY
INFERRED
ASSUMED
UNKNOWN
```

### Verification

```text
PROVED
REFUTED
UNKNOWN
```

This is sufficient for meaningful memory-safety demonstrations.

---

# 70. Initial defect targets

The first EIR implementation SHOULD target:

```text
buffer overflow
null dereference
use-after-free
unchecked return value
tainted sink
```

These defects exercise the major concepts:

```text
value flow
memory objects
control flow
interprocedural summaries
constraints
path feasibility
```

without initially requiring complex concurrency modeling.

---

# 71. Recommended compiler/runtime architecture

```text
            SummaryDB
                │
                ▼
        Evidence Builder
                │
         ┌──────┴──────┐
         │             │
      Slicer        Constraint
                     Extractor
         │             │
         └──────┬──────┘
                ▼
             EIR Core
                │
      ┌─────────┼──────────┐
      ▼         ▼          ▼
 Serializer   Query API   Verifier API
      │         │          │
      │      Review Agent  │
      │         │          │
      └─────────┴──────────┘
                │
                ▼
          Evidence Store
```

---

# 72. Key distinction from compiler IRs

Traditional compiler IRs answer:

> What computation should this program perform?

Function Summary IR answers:

> What externally visible semantic effects does this function have?

Evidence IR answers:

> Why do we believe this particular behavior or defect is possible or impossible?

Semantic Review IR, if introduced later, answers:

> What does this behavior mean relative to programmer intent, architecture, and domain requirements?

Thus:

```text
LLVM IR
   ↓
Program computation

Summary IR
   ↓
Behavior abstraction

Evidence IR
   ↓
Causal justification

Semantic Review IR
   ↓
Intent interpretation
```

---

# 73. VERITAS Evidence IR design invariant

The most important invariant of EIR is:

> **Every conclusion must retain a machine-traceable distinction between what is known, what is derived, what is assumed, what is inferred, and what remains unknown.**

This enables LLM semantic reasoning without sacrificing the fundamental strengths of deterministic program analysis.

---

# 74. Final architectural definition

VERITAS Evidence IR can therefore be formally characterized as:

> **A typed, provenance-preserving, claim-oriented heterogeneous graph IR containing logical predicates, causal paths, constraints, epistemic states, assumptions, unknowns, and proof obligations, designed to mediate an iterative reasoning loop between whole-program program analysis and semantic AI agents.**

Its central loop is:

```text
            VERIFIED PROGRAM FACTS
                     │
                     ▼
              ┌────────────┐
              │ Evidence IR│
              └─────┬──────┘
                    │
                    ▼
              Semantic Agent
                    │
          hypothesis / question
                    │
                    ▼
             Proof Obligation
                    │
            ┌───────┼────────┐
            ▼       ▼        ▼
          Static   SMT    Symbolic/Test
            │       │        │
            └───────┼────────┘
                    ▼
               New Evidence
                    │
                    └──────────────► Evidence IR
```

This feedback loop—not the serialization format itself—is the defining semantic property of VERITAS Evidence IR.

