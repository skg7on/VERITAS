# VERITAS Evidence IR — Formal Specification

**Status:** Draft Formal Specification  
**Version:** 0.1  
**Project:** VERITAS — Verified Evidence Reasoning IR for Trans-program Analysis and Semantics  
**Depends on:** `docs/architecture/veritas-evidence-ir-design.md`

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
    | "exists" | "in" ;
```

### 2.5 Operators

```ebnf
ComparisonOp ::= "==" | "!=" | "<" | "<=" | ">" | ">=" ;

AssignmentOp ::= "=" | ":=" ;

LogicalOp ::= "and" | "or" | "not" | "implies" ;

PathOp ::= "->" ;
```

---

## 3. Top-Level Grammar

### 3.1 Evidence Case

```ebnf
EvidenceCase ::=
    "evidence" Identifier "{"
        [ ContextDecl ]
        { EvidenceMember }
    "}" ;

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
    | SummaryReference
    ;
```

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

```ebnf
Predicate ::=
      AtomicPredicate
    | "(" Predicate ")"
    | "not" Predicate
    | Predicate LogicalOp Predicate
    | Predicate ComparisonOp Predicate
    | QuantifiedPredicate
    ;

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

QuantifiedPredicate ::=
      "forall" Identifier "in" Domain ":" Predicate
    | "exists" Identifier "in" Domain ":" Predicate
    ;

Domain ::=
      Identifier "(" [ ArgumentList ] ")"
    | Reference
    ;
```

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

The following complete example demonstrates the formal grammar:

```eir
evidence Overflow_001 {

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
- Keywords: 67
- Operators: 12
- Delimiters: 8 (`{`, `}`, `[`, `]`, `(`, `)`, `;`, `,`)

### Non-Terminal Symbols
- Top-level: 4 (EvidenceCase, ContextDecl, ContextProperty, EvidenceMember)
- Entities: 5 (EntityDecl, EntityKind, EntityProperty, PropertyKey, PropertyValue)
- Predicates: 7 (Predicate, AtomicPredicate, QuantifiedPredicate, etc.)
- Facts: 4 (FactDecl, EpistemicState, Confidence, Producer)
- Assumptions/Hypotheses/Unknowns: 6
- Claims: 3 (Claim, ClaimKind, Severity)
- Edges/Paths: 7 (EdgeDecl, PathDecl, RelationKind, PathKind, etc.)
- Constraints: 2 (ConstraintDecl, Scope)
- Provenance: 2 (ProvenanceDecl, SourceLocation)
- Verification: 4 (VerificationDecl, VerificationGoal, VerificationStatus, etc.)
- Summaries: 2 (SummaryReference, SummaryComponentList)
- Types: 2 (PrimitiveType, Type)

**Total Non-Terminals**: ~50

---

## 21. Implementation Notes

### 21.1 Parser Generation

This grammar is suitable for:
- **ANTLR 4** — direct EBNF translation
- **Bison/Yacc** — with minor operator precedence annotations
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

- **Source Architecture Document**: `docs/architecture/veritas-evidence-ir-design.md`
- **EBNF Standard**: ISO/IEC 14977:1996
- **Related Specifications**:
  - `docs/architecture/veritas-platform-architecture-design.md`
  - `docs/specs/veritas-engineering-backbone-design-specification.md`

---

## 24. Grammar Version History

| Version | Date | Changes |
|---------|------|---------|
| 0.1 | 2026-08-16 | Initial formal specification consolidating architecture document grammar |

---

**End of Formal Specification**
