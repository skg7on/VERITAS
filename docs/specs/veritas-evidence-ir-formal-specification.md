# VERITAS Evidence IR — Formal Specification

**Status:** Stabilized Formal Specification (EIR-T 1.0)
**Version:** 1.0
**Project:** VERITAS — Verified Evidence Reasoning IR for Trans-program Analysis and Semantics
**Depends on:** `docs/architecture/04-evidence-ir-architecture.md`

> **Stability:** the grammar in this document is the frozen EIR-T 1.0 contract.
> The lexer, parser, and writer of M10C implement exactly these productions;
> they do not add silent syntax extensions. The `veritas_eir_contract_docs`
> CTest, driven by `tests/ci/ValidateEvidenceIrContract.cmake`, pins the required
> productions of §3.1 and §5.1 in this file and fails if any of them is removed
> or rewritten.

---

## 1. Purpose

This document provides a complete formal specification of the VERITAS Evidence IR language (EIR-T) in Extended Backus-Naur Form (EBNF). It consolidates the grammar fragments from the architecture document into a single, executable specification suitable for parser generation and validation.

---

## 2. Lexical Grammar

### 2.1 Whitespace and Comments

```ebnf
Whitespace ::= " " | "\t" | "\n" | "\r" ;

LineComment ::= "//" { Character } "\n" ;

BlockComment ::= "/*" { Character } "*/" ;

Comment ::= LineComment | BlockComment ;
```

Comments and whitespace are ignored during parsing.

### 2.2 Identifiers and References

```ebnf
Letter ::= "a".."z" | "A".."Z" ;

Digit ::= "0".."9" ;

Identifier ::= Letter { Letter | Digit | "_" } ;

QualifiedId ::= Identifier { "." Identifier } ;

Reference ::= "@" QualifiedId ;

FactReference ::= "$" Identifier ;
```

### 2.3 Literals

```ebnf
StringLiteral ::= '"' { StringCharacter } '"' ;

StringCharacter ::= EscapedCharacter | (Character - '"' - "\\") ;

EscapedCharacter ::= "\\" ( '"' | "\\" | "n" | "r" | "t" ) ;

IntegerLiteral ::= [ "-" ] Digit { Digit } ;

BooleanLiteral ::= "true" | "false" ;
```

### 2.4 Keywords

Reserved keywords (cannot be used as identifiers):

```ebnf
Keyword ::= 
      "evidence" | "claim" | "entity" | "fact" | "assumption" 
    | "hypothesis" | "unknown" | "edge" | "path" | "constraint"
    | "provenance" | "verify" | "summary" | "context"
    | "predicate" | "epistemic" | "confidence" | "source"
    | "producer" | "reason" | "property" | "from" | "to" | "kind"
    | "condition" | "conditions" | "feasible" | "transfer"
    | "guard" | "scope" | "blocking" | "suggested_resolution"
    | "severity" | "description" | "function" | "summary_id"
    | "components" | "rule" | "inputs" | "location" | "version"
    | "configuration" | "prove" | "using" | "budget"
    | "repository" | "revision" | "build_variant" | "target"
    | "analyzer_configuration" | "expandable" | "summarized_by"
    | "type" | "origin" | "allocation_site" | "machine" | "effect"
    | "expr" | "must" | "may" | "inferred" | "assumed" | "unknown"
    | "must_not" | "and" | "or" | "not" | "implies" | "forall" 
    | "exists" | "in" | "dependency" | "omission" ;
```

### 2.5 Operators

```ebnf
ComparisonOp ::= "==" | "!=" | "<" | "<=" | ">" | ">=" ;

AssignmentOp ::= "=" | ":=" ;

LogicalOp ::= "and" | "or" | "not" | "implies" ;

PathOp ::= "->" ;
```

`LogicalOp` names the logical operator terminals as a lexical class. The predicate
productions of §5.1 spell those terminals directly, so `LogicalOp` is not
referenced by any production.

---

## 3. Top-Level Grammar

### 3.1 Evidence Case

```ebnf
EvidenceCase ::=
    "evidence" [ Identifier ] "{"
        SchemaDecl
        LevelDecl
        StateDecl
        ContextDecl
        { EvidenceMember }
    "}" ;

SchemaDecl ::= "schema" "=" StringLiteral ";" ;
LevelDecl ::= "level" "=" EvidenceLevel ";" ;
EvidenceLevel ::= "l0" | "l1" | "l2" ;
StateDecl ::= "state" "=" EvidenceState ";" ;
EvidenceState ::=
      "UNREVIEWED" | "POSSIBLE_DEFECT" | "LIKELY_DEFECT"
    | "VERIFIED_DEFECT" | "LIKELY_FALSE_POSITIVE"
    | "VERIFIED_SAFE" | "INCONCLUSIVE" ;

ContextDecl ::=
    "context" "{"
        { ContextProperty }
    "}" ;

ContextProperty ::=
      "repository" "=" StringLiteral ";"
    | "revision" "=" StringLiteral ";"
    | "build_variant" "=" StringLiteral ";"
    | "target" "=" StringLiteral ";"
    | "analyzer_configuration" "=" StringLiteral ";"
    ;

EvidenceMember ::=
      Claim | EntityDecl | FactDecl | AssumptionDecl | HypothesisDecl
    | UnknownDecl | EdgeDecl | PathDecl | ConstraintDecl
    | ProvenanceDecl | VerificationDecl | SummaryReference
    | DependencyDecl | OmissionDecl ;

DependencyDecl ::=
    "dependency" Identifier "{"
        "kind" "=" DependencyKind ";"
        "stable_id" "=" StringLiteral ";"
    "}" ;
DependencyKind ::=
      "summary" | "fact" | "type_layout"
    | "configuration" | "specification" ;

OmissionDecl ::=
    "omission" Identifier "{"
        "kind" "=" QualifiedId ";"
        "subject" "=" Reference ";"
        "reason" "=" StringLiteral ";"
        "expandable" "=" BooleanLiteral ";"
    "}" ;
```

The case `Identifier` is a display label and carries no semantic content: it is
not an input to `EvidenceID`, and the semantic model has no member for it. A
parser must accept any well-formed label, and must not reject a case for
carrying one, but it preserves no label in the semantic model. The canonical
writer emits no label, so canonical text is a function of the case's semantics
alone.

`SchemaDecl`, `LevelDecl`, and `StateDecl` are mandatory and appear once each, in
that order, before `ContextDecl`. `ContextDecl` is likewise mandatory and
appears exactly once, before any `EvidenceMember`. `SchemaDecl` binds the
semantic schema version: the string literal must equal `eir.v1`. Any other value
is rejected as a well-formedness error; it is not an extension point.

`LevelDecl` selects the abstraction level (`EIR-L0`, `EIR-L1`, or `EIR-L2`).
`StateDecl` carries the overall case verification state; its alternatives follow
the verification state transitions of §18. Both enumerations are closed: no other
alternative is defined in this revision, and unknown alternatives are rejected
rather than ignored.

`DependencyDecl` records one semantic input the case consumed, identified by a
`DependencyKind` and a stable ID string of the form
`<kind>:sha256:<digest>`. `OmissionDecl` records one semantic member that was
deliberately withheld at the declared level; `kind` is a qualified identifier
(for example `analyzer_expansion`), `subject` names the referenced member or
expansion target, and `expandable` states whether a higher level can recover
it. A withheld member is never represented by its absence alone: an omission that
cannot be expanded at any higher level must still be declared.

---

## 4. Entity Grammar

### 4.1 Entity Declaration

```ebnf
EntityDecl ::=
    "entity" Identifier ":" EntityKind "{"
        { EntityProperty }
    "}" ;

EntityKind ::=
      "function" | "parameter" | "return" | "value" | "variable"
    | "memory_object" | "memory_region" | "field"
    | "instruction" | "callsite" | "basic_block"
    | "type" | "global"
    | "thread" | "task" | "lock"
    | "state" | "state_machine"
    | "message" | "channel"
    | "resource" | "hardware_resource"
    | "specification" | "runtime_event"
    ;

EntityProperty ::=
      PropertyKey "=" PropertyValue ";"
    ;

PropertyKey ::= Identifier ;

PropertyValue ::=
      StringLiteral
    | IntegerLiteral
    | BooleanLiteral
    | Reference
    | FunctionCall
    ;

FunctionCall ::= Identifier "(" [ ArgumentList ] ")" ;

ArgumentList ::= PropertyValue { "," PropertyValue } ;
```

---

## 5. Predicate Language

### 5.1 Predicate Expression

The predicate grammar is factored into precedence levels. It contains no left
recursion, so it is directly parsable by a recursive-descent parser or a
table-driven generator without precedence annotations.

```ebnf
Predicate ::= QuantifiedPredicate | ImplicationExpr ;
ImplicationExpr ::= OrExpr [ "implies" ImplicationExpr ] ;
OrExpr ::= AndExpr { "or" AndExpr } ;
AndExpr ::= ComparisonExpr { "and" ComparisonExpr } ;
ComparisonExpr ::= UnaryExpr [ ComparisonOp UnaryExpr ] ;
UnaryExpr ::= "not" UnaryExpr | PrimaryExpr ;
PrimaryExpr ::= AtomicPredicate | "(" Predicate ")" ;
QuantifiedPredicate ::=
      "forall" Identifier "in" Domain ":" Predicate
    | "exists" Identifier "in" Domain ":" Predicate ;

AtomicPredicate ::=
      Identifier "(" [ PredicateArgumentList ] ")"
    | Reference
    | BooleanLiteral
    ;

PredicateArgumentList ::= PredicateArgument { "," PredicateArgument } ;

PredicateArgument ::=
      Reference
    | IntegerLiteral
    | StringLiteral
    | Identifier
    | Predicate
    ;

Domain ::=
      Identifier "(" [ ArgumentList ] ")"
    | Reference
    ;
```

### 5.2 Precedence and Associativity

Binding tightest first:

| Level | Production | Associativity |
| --- | --- | --- |
| 1 (tightest) | `PrimaryExpr` — atom or `"(" Predicate ")"` | n/a |
| 2 | `UnaryExpr` — prefix `not` | right (prefix) |
| 3 | `ComparisonExpr` — `ComparisonOp` | **non-associative** |
| 4 | `AndExpr` — `and` | left |
| 5 | `OrExpr` — `or` | left |
| 6 | `ImplicationExpr` — `implies` | right |
| 7 (loosest) | `QuantifiedPredicate` — `forall` / `exists` | prefix; owns everything after `:` |

Rules that follow from the factored grammar:

1. **Comparisons are non-associative.** The optional trailing comparison in
   `ComparisonExpr` is never repeated, so `a < b < c` is not a predicate. A
   chained comparison must be written as an explicit conjunction:
   `a < b and b < c`.
2. **`and` and `or` are left-associative.** `a and b and c` groups as
   `(a and b) and c` and `a or b or c` groups as `(a or b) or c`. Because the
   two operators sit at different levels, `and` binds tighter than `or`:
   `a or b and c` groups as `a or (b and c)`.
3. **`implies` is right-associative.** `a implies b implies c` groups as
   `a implies (b implies c)`, and `implies` binds loosest of the logical
   operators, so `a and b implies c` groups as `(a and b) implies c`.
4. **A quantifier owns the full predicate after its colon.** The body of
   `forall x in D: P` and `exists x in D: P` is a complete `Predicate`, so the
   scope of `x` extends as far right as possible and the body may itself
   contain `and`, `or`, `implies`, comparisons, and nested quantifiers. To
   restrict the body, parenthesize it explicitly.
5. **`not` binds tighter than every binary operator.** `not a == b` groups as
   `(not a) == b`; write `not (a == b)` to negate a comparison. Consecutive
   prefixes (`not not a`) are legal.

A writer must emit parentheses whenever the child production's binding level is
looser than the parent's, whenever a second comparison would otherwise be
juxtaposed, or whenever right-associative `implies` would regroup. Re-serializing
a parsed predicate must reproduce the same grouping.

---

## 6. Fact Grammar

### 6.1 Fact Declaration

```ebnf
FactDecl ::=
    "fact" Identifier "{"
        "predicate" "=" Predicate ";"
        "epistemic" "=" EpistemicState ";"
        [ "confidence" "=" Confidence ";" ]
        [ "source" "=" Producer ";" ]
        [ "provenance" "=" Reference ";" ]
    "}" ;

EpistemicState ::=
      "must"
    | "may"
    | "must_not"
    | "inferred"
    | "assumed"
    | "unknown"
    ;

Confidence ::=
      "exact"
    | "high"
    | "medium"
    | "low"
    | "unknown"
    ;

Producer ::= QualifiedId ;
```

---

## 7. Assumption, Hypothesis, and Unknown Grammar

### 7.1 Assumption Declaration

```ebnf
AssumptionDecl ::=
    "assumption" Identifier "{"
        "predicate" "=" Predicate ";"
        "source" "=" AssumptionSource ";"
        [ "scope" "=" Scope ";" ]
    "}" ;

AssumptionSource ::=
      FunctionCall
    | QualifiedId
    ;
```

### 7.2 Hypothesis Declaration

```ebnf
HypothesisDecl ::=
    "hypothesis" Identifier "{"
        "predicate" "=" Predicate ";"
        "producer" "=" Producer ";"
        [ "reason" "=" StringLiteral ";" ]
        [ "confidence" "=" Confidence ";" ]
    "}" ;
```

### 7.3 Unknown Declaration

```ebnf
UnknownDecl ::=
    "unknown" Identifier "{"
        "property" "=" Predicate ";"
        "reason" "=" UnknownReason ";"
        [ "blocking" "=" ReferenceList ";" ]
        [ "suggested_resolution" "=" ResolutionAction ";" ]
    "}" ;

UnknownReason ::=
      "UNRESOLVED_CALL"
    | "UNKNOWN_ALIAS"
    | "EXTERNAL_FUNCTION"
    | "MISSING_SPECIFICATION"
    | "ANALYSIS_TIMEOUT"
    | "STATE_EXPLOSION"
    | "UNSUPPORTED_LANGUAGE_FEATURE"
    | "INLINE_ASSEMBLY"
    | "DYNAMIC_LOADING"
    | "UNKNOWN_BUILD_CONFIGURATION"
    ;

ResolutionAction ::= FunctionCall ;

ReferenceList ::= "[" [ Reference { "," Reference } ] "]" ;
```

---

## 8. Claim Grammar

### 8.1 Claim Declaration

```ebnf
Claim ::=
    "claim" Identifier "{"
        "kind" "=" ClaimKind ";"
        "subject" "=" Reference ";"
        "predicate" "=" Predicate ";"
        "severity" "=" Severity ";"
        [ "description" "=" StringLiteral ";" ]
    "}" ;

ClaimKind ::=
      "buffer_overflow"
    | "null_dereference"
    | "use_after_free"
    | "memory_leak"
    | "data_race"
    | "deadlock"
    | "lock_order_violation"
    | "taint_flow"
    | "injection"
    | "privilege_violation"
    | "state_violation"
    | "protocol_violation"
    | "deadline_violation"
    | "resource_violation"
    | "architecture_violation"
    | "semantic_regression"
    | QualifiedId  (* Domain-specific claim kinds *)
    ;

Severity ::=
      "critical"
    | "high"
    | "medium"
    | "low"
    | "info"
    ;
```

---

## 9. Edge and Path Grammar

### 9.1 Edge Declaration

```ebnf
EdgeDecl ::=
    "edge" Identifier "{"
        "from" "=" Reference ";"
        "to" "=" Reference ";"
        "kind" "=" RelationKind ";"
        [ "condition" "=" Predicate ";" ]
        [ "epistemic" "=" EpistemicState ";" ]
        [ "provenance" "=" Reference ";" ]
        [ "transfer" "=" TransferFunction ";" ]
        [ "summarized_by" "=" Reference ";" ]
        [ "expandable" "=" BooleanLiteral ";" ]
        [ "summary" "=" FunctionCall ";" ]
    "}" ;

RelationKind ::=
      "CALLS" | "MAY_CALL" | "MUST_CALL"
    | "DEF" | "USE" | "FLOWS_TO"
    | "READS" | "WRITES" | "MAY_READ" | "MAY_WRITE"
    | "MAY_ALIAS" | "MUST_ALIAS" | "MUST_NOT_ALIAS"
    | "CONTROLS" | "DOMINATES" | "POST_DOMINATES"
    | "ALLOCATES" | "FREES" | "ESCAPES"
    | "ACQUIRES" | "RELEASES" | "HOLDS"
    | "TRANSITIONS"
    | "SPAWNS" | "RUNS_ON"
    | "SENDS" | "RECEIVES"
    | "SUPPORTED_BY" | "CONTRADICTED_BY" | "DERIVED_FROM"
    | "SUMMARIZED_BY" | "EXPANDS_TO"
    ;

TransferFunction ::= StringLiteral | Predicate ;
```

### 9.2 Path Declaration

```ebnf
PathDecl ::=
    "path" Identifier PathKind "{"
        PathExpression
        [ "conditions" "{" { Predicate ";" } "}" ]
        [ "feasible" "=" Feasibility ";" ]
        [ "provenance" "=" Reference ";" ]
    "}" ;

PathKind ::=
      "call"
    | "control"
    | "data_flow"
    | "value_flow"
    | "taint"
    | "ownership"
    | "memory"
    | "state"
    | "lock"
    | "message"
    | "resource"
    ;

PathExpression ::= Reference { PathOp Reference } ";" ;

Feasibility ::=
      "PROVED_FEASIBLE"
    | "SAT"
    | "MAYBE"
    | "UNTESTED"
    | "UNSAT"
    | "PROVED_INFEASIBLE"
    | "UNKNOWN"
    ;
```

---

## 10. Constraint Grammar

### 10.1 Constraint Declaration

```ebnf
ConstraintDecl ::=
    "constraint" Identifier "{"
        "expr" "=" Predicate ";"
        [ "scope" "=" Scope ";" ]
        [ "epistemic" "=" EpistemicState ";" ]
        [ "provenance" "=" Reference ";" ]
    "}" ;

Scope ::=
      "global"
    | "function"
    | "path"
    | "basic_block"
    | "callsite"
    | "entity"
    | FunctionCall
    ;
```

---

## 11. Provenance Grammar

### 11.1 Provenance Declaration

```ebnf
ProvenanceDecl ::=
    "provenance" Identifier "{"
        "producer" "=" Producer ";"
        [ "rule" "=" StringLiteral ";" ]
        [ "inputs" "=" FactReferenceList ";" ]
        [ "location" "=" SourceLocation ";" ]
        [ "version" "=" StringLiteral ";" ]
        [ "configuration" "=" StringLiteral ";" ]
    "}" ;

FactReferenceList ::= "[" [ FactReference { "," FactReference } ] "]" ;

SourceLocation ::= FunctionCall ;
```

---

## 12. Verification Grammar

### 12.1 Verification (Proof Obligation) Declaration

```ebnf
VerificationDecl ::=
    "verify" Identifier "{"
        VerificationGoal
        [ "using" "=" VerificationBackendList ";" ]
        [ "budget" "=" ResourceBudget ";" ]
        [ "status" "=" VerificationStatus ";" ]
        [ "result" "=" Reference ";" ]
    "}" ;

VerificationGoal ::=
      "prove" "=" Predicate ";"
    | "refute" "=" Predicate ";"
    | "check" "=" Predicate ";"
    ;

VerificationBackendList ::= "[" [ Identifier { "," Identifier } ] "]" ;

ResourceBudget ::=
      IntegerLiteral
    | FunctionCall
    ;

VerificationStatus ::=
      "PENDING"
    | "PROVED"
    | "REFUTED"
    | "UNKNOWN"
    | "TIMEOUT"
    | "UNSUPPORTED"
    ;
```

---

## 13. Summary Reference Grammar

### 13.1 Summary Reference Declaration

```ebnf
SummaryReference ::=
    "summary" Identifier "{"
        "function" "=" Reference ";"
        "summary_id" "=" StringLiteral ";"
        [ "components" "=" SummaryComponentList ";" ]
    "}" ;

SummaryComponentList ::= "[" [ Identifier { "," Identifier } ] "]" ;
```

---

## 14. Type System (Semantic Types)

### 14.1 Primitive Types

```ebnf
PrimitiveType ::=
      "Bool"
    | "Int"
    | "UInt"
    | "BitVector" "<" IntegerLiteral ">"
    | "String"
    | "Address"
    | "Interval" "<" Type ">"
    | "SourceLocation"
    | "FunctionRef"
    | "ValueRef"
    | "MemoryRef"
    | "TypeRef"
    | "ThreadRef"
    | "LockRef"
    | "StateRef"
    | "ResourceRef"
    | "PathRef"
    | "FactRef"
    | "SummaryRef"
    ;
```

### 14.2 Type Expressions

```ebnf
Type ::=
      PrimitiveType
    | EntityKind
    | QualifiedId
    ;
```

---

## 15. Concrete Syntax Example

The following complete example demonstrates the formal grammar, including the
three mandatory top-level declarations, one dependency, and one omission:

```eir
evidence Overflow_001 {
    schema = "eir.v1";
    level = l1;
    state = POSSIBLE_DEFECT;

    context {
        repository = "radio-stack";
        revision = "a87f03e";
        build_variant = "ARM64_RELEASE";
    }

    entity len : value {
        origin = @packet.length;
    }

    entity dst : memory_object {
        allocation_site = @allocate_payload_buffer;
    }

    entity sink : callsite {
        function = "memcpy";
        location = src("decoder.cpp", 281, 9);
    }

    claim C1 {
        kind = buffer_overflow;
        subject = @sink;
        predicate = value(@len) > capacity(@dst);
        severity = high;
    }

    fact F1 {
        predicate = range(@len, 0, 65535);
        epistemic = must;
        provenance = @PR1;
    }

    fact F2 {
        predicate = capacity(@dst) == 2048;
        epistemic = must;
        provenance = @PR2;
    }

    fact F3 {
        predicate = not dominates(@validate_length, @sink);
        epistemic = must;
        provenance = @PR3;
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

        feasible = SAT;
    }

    unknown U1 {
        property = postcondition(@vendor_validate);
        reason = EXTERNAL_FUNCTION;
        suggested_resolution = infer_contract(@vendor_validate);
    }

    verify O1 {
        prove = exists p in feasible_paths(@entry, @sink):
                    value(@len) > capacity(@dst);
        using = [smt, symbolic_execution];
    }

    provenance PR1 {
        producer = analysis.value_range;
        version = "0.3";
    }

    provenance PR2 {
        producer = analysis.memory_object;
        version = "0.5";
    }

    provenance PR3 {
        producer = analysis.dominator;
        version = "0.2";
    }

    dependency DEP1 {
        kind = summary;
        stable_id = "summary:sha256:62be5c86c9bef6e9230c791dc8fa6cd426a3495aecd14df5a03430b3ba5e7dd7";
    }

    omission OM1 {
        kind = analyzer_expansion;
        subject = @U1;
        reason = "vendor_validate contract is unavailable for expansion";
        expandable = true;
    }
}
```

---

## 16. Well-Formedness Constraints

An Evidence Case is well-formed if and only if:

1. **Unique Primary Claim**: Exactly one claim declaration exists.
2. **Referential Integrity**: Every reference `@x` has a matching entity declaration or globally resolvable semantic ID.
3. **Epistemic Annotation**: Every fact has an explicit epistemic state.
4. **Provenance Tracking**: Every derived fact has provenance metadata.
5. **Type Correctness**: All predicate expressions are type-correct per the type system.
6. **Path Validity**: All path expressions reference declared entities.
7. **Summary Stability**: All summary references identify immutable summary IDs.
8. **Hypothesis Isolation**: Hypotheses cannot appear as proven facts.
9. **Context Binding**: Program revision context is defined.
10. **Verification Traceability**: Proof results identify their verification producer.

---

## 17. Epistemic State Lattice

The epistemic states form a lattice with the following partial order:

```
        MUST
         │
         ├─── MAY
         │
         ├─── MUST_NOT
         │
         ├─── INFERRED
         │
         ├─── ASSUMED
         │
         └─── UNKNOWN
```

**Propagation Rules:**

- `MUST(P) ∧ MUST(P → Q) ⊢ MUST(Q)`
- `MAY(P) ∧ MUST(P → Q) ⊢ MAY(Q)`
- `INFERRED(P) ∧ MUST(P → Q) ⊢ INFERRED(Q)`
- `INFERRED(P) ∧ MUST(P → Q) ⊬ MUST(Q)` — no promotion without verification

---

## 18. Verification State Transition Semantics

```
UNREVIEWED
    ↓
POSSIBLE_DEFECT
    ↓ (semantic evidence)
LIKELY_DEFECT
    ↓ (deterministic verification only)
VERIFIED_DEFECT

POSSIBLE_DEFECT
    ↓ (semantic reasoning)
LIKELY_FALSE_POSITIVE
    ↓ (proof or authoritative evidence only)
VERIFIED_SAFE
```

**Invariant**: Only deterministic verification (static analysis, SMT, symbolic execution, concrete test) may transition to `VERIFIED_DEFECT` or `VERIFIED_SAFE`. LLM output may only produce `LIKELY_*` states.

---

## 19. Canonical Serialization

### 19.1 Canonical Form Requirements

For content-addressable identity, Evidence Cases must have a canonical serialization:

1. **Sorted Keys**: All property keys in entity, fact, and other declarations must be sorted lexicographically.
2. **Normalized Whitespace**: Canonical whitespace (single space between tokens, newline after semicolons).
3. **Sorted Lists**: Reference lists, component lists, and backend lists must be sorted.
4. **No Comments**: Comments are removed in canonical form.
5. **Stable Predicate Representation**: Predicates must be normalized (associative/commutative operators ordered).

### 19.2 Content Hash

```
EvidenceID = sha256(CanonicalForm(EvidenceCase))
```

---

## 20. Grammar Summary Statistics

### Terminal Symbols
- Keywords: 73 entries (72 distinct; `unknown` is listed twice)
- Operators: 12
- Delimiters: 8 (`{`, `}`, `[`, `]`, `(`, `)`, `;`, `,`)

### Non-Terminal Symbols
- Top-level: 12 (EvidenceCase, SchemaDecl, LevelDecl, EvidenceLevel, StateDecl, EvidenceState, ContextDecl, ContextProperty, EvidenceMember, DependencyDecl, DependencyKind, OmissionDecl)
- Entities: 5 (EntityDecl, EntityKind, EntityProperty, PropertyKey, PropertyValue)
- Predicates: 12 (Predicate, ImplicationExpr, OrExpr, AndExpr, ComparisonExpr, UnaryExpr, PrimaryExpr, QuantifiedPredicate, AtomicPredicate, PredicateArgumentList, PredicateArgument, Domain)
- Facts: 4 (FactDecl, EpistemicState, Confidence, Producer)
- Assumptions/Hypotheses/Unknowns: 6
- Claims: 3 (Claim, ClaimKind, Severity)
- Edges/Paths: 7 (EdgeDecl, PathDecl, RelationKind, PathKind, etc.)
- Constraints: 2 (ConstraintDecl, Scope)
- Provenance: 2 (ProvenanceDecl, SourceLocation)
- Verification: 4 (VerificationDecl, VerificationGoal, VerificationStatus, etc.)
- Summaries: 2 (SummaryReference, SummaryComponentList)
- Types: 2 (PrimitiveType, Type)

**Total productions**: 86 (including the lexical productions of §2)

---

## 21. Implementation Notes

### 21.1 Parser Generation

This grammar is suitable for:
- **ANTLR 4** — direct EBNF translation
- **Bison/Yacc** — direct translation; predicate precedence is already factored into the productions (§5.2), so no `%left`/`%right` declarations are required
- **Hand-written recursive descent** — straightforward due to keyword-driven structure

### 21.2 Type Checking

Type checking requires a separate pass after parsing:
1. Build symbol table of entity declarations
2. Resolve all references
3. Type-check predicate expressions against declared entity types
4. Verify provenance references point to fact/entity declarations

### 21.3 Semantic Validation

Well-formedness checking requires:
1. Single claim verification
2. Referential integrity check (all `@x` resolve)
3. Epistemic annotation completeness
4. Provenance traceability
5. Summary ID validation (lookup in SummaryDB)

---

## 22. Extensions and Future Work

### 22.1 Planned Extensions (V0.2+)

- **Concurrency Primitives**: `happens_before`, `may_happen_in_parallel`, lock orders
- **State Machines**: explicit state transition graphs
- **Ownership Semantics**: linear types, move semantics, borrow checking
- **Symbolic Expressions**: SMT-LIB integration for bit-precise semantics
- **Domain-Specific Extensions**: RAN, automotive, medical device predicates

### 22.2 Serialization Formats

- **Primary**: Protobuf binary (production)
- **Debug**: EIR-T textual form (this grammar)
- **Interchange**: JSON (diagnostics, web tools)

---

## 23. References

- **Source Architecture Document**: `docs/architecture/04-evidence-ir-architecture.md`
- **EBNF Standard**: ISO/IEC 14977:1996
- **Related Specifications**:
  - `docs/architecture/01-platform-architecture.md`
  - `docs/specs/veritas-engineering-backbone-design-specification.md`
  - [Evidence IR Agent security use cases](veritas-evidence-ir-agent-security-use-cases-design-spec.md)

---

## 24. Grammar Version History

| Version | Date | Changes |
|---------|------|---------|
| 0.1 | 2026-08-16 | Initial formal specification consolidating architecture document grammar |
| 1.0 | 2026-09-11 | Stabilized EIR-T 1.0. Added the mandatory top-level `SchemaDecl`, `LevelDecl`, and `StateDecl` (with `EvidenceLevel` and `EvidenceState`); added the `DependencyDecl`/`DependencyKind` and `OmissionDecl` evidence members and the `dependency`/`omission` reserved keywords; replaced the left-recursive predicate production with the precedence-factored `ImplicationExpr`/`OrExpr`/`AndExpr`/`ComparisonExpr`/`UnaryExpr`/`PrimaryExpr` chain and documented precedence, associativity, and quantifier scope in §5.2; updated the concrete syntax example. The grammar is frozen as the M10C implementation contract. |

---

**End of Formal Specification**
