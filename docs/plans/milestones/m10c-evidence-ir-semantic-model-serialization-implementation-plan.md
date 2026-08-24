# M10C Evidence IR Semantic Model and Serialization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Convert M10B evidence inputs into validated, canonical, content-addressed Evidence IR and emit equivalent EIR-T, Protobuf, and full-EIR diagnostic JSON.

**Architecture:** Add an `eir.v1` semantic model under `veritas::evidence`, with validation and canonicalization as mandatory gates before any representation boundary. M10B supplies one immutable `EvidenceBuildInput`; M10C maps it without recomputing analysis, projects L0/L1/L2, and uses independent EIR-T, Protobuf, and JSON codecs over the same model.

**Tech Stack:** C++20, `veritas::Status`/`StatusOr<T>`, `core::CanonicalValue`, SHA-256 stable IDs, Protobuf, LLVM JSON output, GoogleTest, CMake/CTest.

**Spec:** `docs/specs/milestones/m10c-evidence-ir-semantic-model-serialization-design-spec.md`

**Executable test contract:** `docs/specs/milestones/m10b-m10c-api-to-evidence-ir-test-design-spec.md`

## Global Constraints

- M10B and its revised typed `EvidenceBuildInput` handoff must be implemented and passing before M10C begins.
- `eir.v1` is the only accepted semantic schema version.
- EIR-T syntax and well-formedness follow `docs/specs/veritas-evidence-ir-formal-specification.md` after Task 1 stabilizes it.
- Every serializer validates first; no serializer repairs or silently drops invalid semantic members.
- EIR assembly never strengthens epistemic state and never treats truncated absence as negative evidence.
- A complete empty open-world query becomes an explicit unknown or omission.
- Only a complete dominating-check result with matching M9 query-completion
  provenance may feed `evidence.closed_world.dominating_check_absence.v1`.
- M10C resolves every query completion fact and selected witness from the
  immutable handoff; it never performs a second analysis or provenance query.
- M10B `--format json` remains slice JSON; full EIR JSON is `--format eir-json`.
- Protobuf bytes are not identity input; `core::CanonicalValue` bytes are the identity input.
- No RTTI, exceptions, `dynamic_cast`, `typeid`, `throw`, `try`, or `catch`.
- No Review Agent, verifier dispatch, Evidence Store, cross-revision diff, or incremental revalidation is implemented in M10C.
- Every new source, header, Protobuf, CMake, and test file begins with the required Apache-2.0 header.
- All 31 M10C-owned `BLD`, `VID`, `REP`, and `DEM-002`–`DEM-006` cases pass;
  all 23 M10B-owned cases rerun as compatibility tests.

---

### Task 1: Stabilize the EIR-T 1.0 Contract

**Files:**
- Modify: `docs/specs/veritas-evidence-ir-formal-specification.md`
- Create: `tests/ci/ValidateEvidenceIrContract.cmake`
- Modify: `tests/ci/CMakeLists.txt`

**Interfaces:**
- Produces: lossless top-level `schema`, `level`, and `state` syntax
- Produces: `DependencyDecl` and `OmissionDecl`
- Produces: non-left-recursive predicate precedence grammar
- Produces: CTest `veritas_eir_contract_docs`

- [ ] **Step 1: Write the failing documentation-contract test**

Create `ValidateEvidenceIrContract.cmake` with the license header and these exact checks:

```cmake
if(NOT DEFINED VERITAS_SOURCE_DIR)
  message(FATAL_ERROR "VERITAS_SOURCE_DIR is required")
endif()

set(EIR_SPEC
    "${VERITAS_SOURCE_DIR}/docs/specs/veritas-evidence-ir-formal-specification.md")
file(READ "${EIR_SPEC}" EIR_SPEC_CONTENT)

foreach(REQUIRED_LITERAL IN ITEMS
    "SchemaDecl ::= \"schema\" \"=\" StringLiteral \";\""
    "EvidenceLevel ::= \"l0\" | \"l1\" | \"l2\""
    "EvidenceState ::="
    "DependencyDecl ::="
    "OmissionDecl ::="
    "ImplicationExpr ::="
    "OrExpr ::="
    "AndExpr ::="
    "ComparisonExpr ::="
    "UnaryExpr ::=")
  string(FIND "${EIR_SPEC_CONTENT}" "${REQUIRED_LITERAL}" FOUND_AT)
  if(FOUND_AT EQUAL -1)
    message(FATAL_ERROR "EIR formal spec is missing: ${REQUIRED_LITERAL}")
  endif()
endforeach()
```

- [ ] **Step 2: Register the test**

Append to `tests/ci/CMakeLists.txt`:

```cmake
add_test(
  NAME veritas_eir_contract_docs
  COMMAND
    "${CMAKE_COMMAND}"
    "-DVERITAS_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/ValidateEvidenceIrContract.cmake"
)
set_tests_properties(veritas_eir_contract_docs PROPERTIES TIMEOUT 30)
```

- [ ] **Step 3: Run the test to verify it fails**

Run:

```bash
cmake --preset default
cmake --build --preset default
ctest --test-dir build -R veritas_eir_contract_docs --output-on-failure
```

Expected: FAIL because the draft formal specification lacks the stabilized declarations.

- [ ] **Step 4: Replace the top-level and predicate EBNF with the stabilized grammar**

Use these productions exactly, and add `dependency` and `omission` to the reserved-keyword list:

```ebnf
EvidenceCase ::=
    "evidence" Identifier "{"
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
```

Document that the schema string must equal `eir.v1`, comparisons are non-associative, `and`/`or` are left-associative, `implies` is right-associative, and a quantifier owns the full predicate after `:`. Update the complete example to include `schema`, `level`, `state`, one dependency, and one omission.

- [ ] **Step 5: Run the contract test**

Run: `ctest --test-dir build -R veritas_eir_contract_docs --output-on-failure`

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add docs/specs/veritas-evidence-ir-formal-specification.md tests/ci
git commit -m "docs: stabilize EIR-T v1 grammar"
```

---

### Task 2: Add the Evidence ID Kind and Semantic Model

**Files:**
- Modify: `include/veritas/core/Ids.h`
- Modify: `src/core/Ids.cpp`
- Modify: `tests/unit/core/IdsTest.cpp`
- Create: `include/veritas/evidence/EvidenceCase.h`
- Create: `src/evidence/EvidenceCase.cpp`
- Modify: `src/evidence/CMakeLists.txt`
- Modify: `tests/support/evidence/EvidenceScenario.h`
- Modify: `tests/support/evidence/EvidenceScenario.cpp`
- Create: `tests/unit/evidence/EvidenceCaseTest.cpp`
- Modify: `tests/unit/evidence/CMakeLists.txt`

**Interfaces:**
- Produces: `core::IdKind::kEvidence` serialized as `evidence`
- Produces: `EvidenceCase`, typed expression AST, and all `eir.v1` record types
- Produces: test-only `MakeValidMinimalEvidenceCase()` and
  `MakeOverflowEvidenceCase()` from the shared typed scenario builder
- Consumes: `core::StableId`
- Consumes: M10B `ClaimKind` and `Severity`

- [ ] **Step 1: Write failing ID and semantic-model tests**

Add to `IdsTest.cpp`:

```cpp
TEST(IdsTest, EvidenceIdRoundTrips) {
  const std::array<std::byte, 3> bytes{std::byte{1}, std::byte{2}, std::byte{3}};
  const auto id = MakeStableId(IdKind::kEvidence, bytes);
  EXPECT_THAT(ToString(id), testing::StartsWith("evidence:sha256:"));
  auto parsed = ParseStableId(ToString(id));
  ASSERT_TRUE(parsed.ok()) << parsed.status().message();
  EXPECT_EQ(*parsed, id);
}
```

Create `EvidenceCaseTest.cpp`:

```cpp
TEST(EvidenceCaseTest, RepresentsInitialOverflowCaseState) {
  EvidenceCase value;
  value.schema_version = "eir.v1";
  value.level = EvidenceLevel::kL1;
  value.verification_state = VerificationState::kPossibleDefect;
  value.primary_claim.kind = ClaimKind::kBufferOverflow;
  value.primary_claim.severity = Severity::kHigh;
  EXPECT_EQ(value.schema_version, "eir.v1");
  EXPECT_EQ(value.proof_obligations.size(), 0U);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `ctest --test-dir build -R "IdsTest|EvidenceCaseTest" --output-on-failure`

Expected: FAIL because `kEvidence` and `EvidenceCase` do not exist.

- [ ] **Step 3: Add `IdKind::kEvidence`**

Add the enum value in `Ids.h`; add `kEvidence -> "evidence"` to `IdKindToString` and `StringToIdKind` in `Ids.cpp`. Do not add a second Evidence-ID parser.

- [ ] **Step 4: Define the semantic enums and expression AST**

`EvidenceCase.h` must define these enum families with `kUnspecified` sentinels where invalid default construction would otherwise look valid:

```text
EvidenceLevel: L0, L1, L2
VerificationState: UNREVIEWED, POSSIBLE_DEFECT, LIKELY_DEFECT,
                   VERIFIED_DEFECT, LIKELY_FALSE_POSITIVE,
                   VERIFIED_SAFE, INCONCLUSIVE
EntityKind: FUNCTION, CALLSITE, VALUE, MEMORY_OBJECT, BASIC_BLOCK
RelationKind: CALLS, FLOWS_TO, READS, WRITES, DOMINATES, MAY_ALIAS
PathKind: CALL, VALUE_FLOW, CONTROL
EpistemicState: MUST, MAY, MUST_NOT, INFERRED, ASSUMED, UNKNOWN
Confidence: EXACT, HIGH, MEDIUM, LOW, UNKNOWN
ProofStatus: PENDING, PROVED, REFUTED, UNKNOWN, TIMEOUT, UNSUPPORTED
DependencyKind: SUMMARY, FACT, TYPE_LAYOUT, CONFIGURATION, SPECIFICATION
Feasibility: PROVED_FEASIBLE, SAT, MAYBE, UNTESTED, UNSAT,
             PROVED_INFEASIBLE, UNKNOWN
ProofGoalKind: PROVE, REFUTE, CHECK
```

Reuse M10B's invalid-sentinel `ClaimKind` and `Severity` enums in `Claim`; do
not redeclare them in the semantic model.

Use one recursively owned value type for predicates and properties:

```cpp
struct Expression {
  enum class Kind {
    kBool, kInteger, kString, kSymbol, kReference, kCall,
    kNot, kCompare, kAnd, kOr, kImplies, kForAll, kExists
  };
  Kind kind;
  std::string text;
  int64_t integer = 0;
  bool boolean = false;
  std::vector<Expression> operands;
};

struct SourceSpan {
  std::size_t offset = 0;
  std::size_t line = 0;
  std::size_t column = 0;
};
```

- [ ] **Step 5: Define the exact record set**

Use case-local IDs as `std::string`; global IDs are `std::optional<core::StableId>`. Define records with these fields:

```text
AnalyzerVersion: producer, version, configuration
ProgramBinding: repository_id, revision_id, build_variant_id, target_triple,
                analysis_configuration_id, type_layout_id, analysis_run_id,
                analyzer_versions[]
Entity: id, kind, stable_id, properties<string, Expression>
Edge: id, from, to, kind, epistemic, provenance_id, summarized_by, expandable
Path: id, kind, entity_ids[], conditions[], feasibility, provenance_id
Claim: id, kind, subject, predicate, severity, description
Fact: id, stable_id, predicate, epistemic, confidence, producer,
      provenance_id, derived
Assumption: id, predicate, source, scope
Hypothesis: id, predicate, producer, reason, confidence
Unknown: id, property, reason, blocking_ids[], suggested_resolution
Constraint: id, expression, scope, epistemic, provenance_id
Provenance: id, producer, rule, input_fact_ids[], source_anchor_id,
            analysis_run_id, version, configuration
ProofObligation: id, goal_kind, predicate, verifier_kinds[], budget,
                 status, result_id, verification_producer
SummaryReference: id, function_id, summary_id, components[]
Dependency: id, kind, stable_id
Omission: id, kind, subject, reason, expandable
EvidenceCase: optional<core::StableId> evidence_id, schema_version, level,
              program, primary_claim,
              entities[], edges[], paths[], facts[], assumptions[],
              hypotheses[], unknowns[], constraints[], provenance[],
              proof_obligations[], summaries[], dependencies[],
              verification_state, omissions[]
```

Add `ToString`/parse helpers for textual enums in `EvidenceCase.cpp`; all parse helpers return `StatusOr<T>` and reject unknown spellings.

- [ ] **Step 6: Extend the shared typed scenario builder**

Add semantic-case factories to the M10B `EvidenceScenarioBuilder`. The minimal
factory returns the smallest valid initial L0 case. The overflow factory maps a
fully populated typed M10B scenario into stable local IDs, but does not parse or
serialize any representation. Every later M10C unit test uses these factories
instead of independently inventing look-alike cases. Add fluent
`WithTruncatedDominatingChecks(TruncationReason)` and
`BuildRequest(EvidenceLevel)` helpers; the former creates matching truncated
metadata and query-completion provenance rather than mutating fields into an
invalid combination.

- [ ] **Step 7: Wire the model into CMake**

Add `EvidenceCase.cpp` to `veritas_evidence`, link `veritas::core` publicly, register `EvidenceCaseTest`, and apply `veritas_add_warnings` plus `DISCOVERY_TIMEOUT 60`.

- [ ] **Step 8: Build and run tests**

Run:

```bash
cmake --build --preset default
ctest --test-dir build -R "IdsTest|EvidenceCaseTest" --output-on-failure
```

Expected: PASS.

- [ ] **Step 9: Commit**

```bash
git add include/veritas/core/Ids.h src/core/Ids.cpp tests/unit/core/IdsTest.cpp \
  include/veritas/evidence/EvidenceCase.h src/evidence/EvidenceCase.cpp \
  src/evidence/CMakeLists.txt tests/support/evidence tests/unit/evidence
git commit -m "feat: add Evidence IR semantic model"
```

---

### Task 3: Enforce Evidence Well-Formedness

**Files:**
- Create: `include/veritas/evidence/EvidenceValidator.h`
- Create: `src/evidence/EvidenceValidator.cpp`
- Modify: `src/evidence/CMakeLists.txt`
- Create: `tests/unit/evidence/EvidenceValidatorTest.cpp`
- Modify: `tests/unit/evidence/CMakeLists.txt`

**Interfaces:**
- Consumes: `const EvidenceCase&`
- Produces: `EvidenceValidationReport ValidateEvidenceCase(const EvidenceCase&)`
- Produces: `Status RequireValidEvidenceCase(const EvidenceCase&)`

- [ ] **Step 1: Write failing validation tests**

Cover one valid minimal case plus individual failures:

```cpp
TEST(EvidenceValidatorTest, RejectsDanglingClaimSubject) {
  auto value = MakeValidMinimalEvidenceCase();
  value.primary_claim.subject = "missing";
  const auto report = ValidateEvidenceCase(value);
  ASSERT_FALSE(report.ok());
  EXPECT_EQ(report.issues.front().code,
            EvidenceValidationCode::kDanglingReference);
  EXPECT_EQ(report.issues.front().member_id, value.primary_claim.id);
}

TEST(EvidenceValidatorTest, RejectsDerivedFactWithoutProvenance) {
  auto value = MakeValidMinimalEvidenceCase();
  value.facts.front().derived = true;
  value.facts.front().provenance_id.clear();
  EXPECT_EQ(ValidateEvidenceCase(value).issues.front().code,
            EvidenceValidationCode::kMissingProvenance);
}
```

Also test duplicate IDs, missing context fields, invalid expression arity/type, disconnected paths, invalid summary IDs, hypothesis/fact ID collision, provenance whose `analysis_run_id` differs from the case binding, hidden truncation, and non-`PENDING` proof results without a producer/result.

These tests implement `VID-001` through `VID-006` exactly: one minimal valid
case, duplicate/dangling references, missing derived provenance, expression and
path failures, proof-authority violations, and hidden omissions. Name each test
with its stable case ID in a source comment or parameter name so catalog
coverage can be audited without relying on CTest display-name parsing.

- [ ] **Step 2: Run the test to verify it fails**

Run: `ctest --test-dir build -R EvidenceValidatorTest --output-on-failure`

Expected: FAIL because the validator does not exist.

- [ ] **Step 3: Define stable validation diagnostics**

```cpp
enum class EvidenceValidationCode {
  kSchemaVersion,
  kMissingProgramContext,
  kPrimaryClaimCount,
  kDuplicateLocalId,
  kDanglingReference,
  kMissingEpistemic,
  kMissingProvenance,
  kExpressionType,
  kPathDisconnected,
  kInvalidStableId,
  kMixedProgramContext,
  kHypothesisAuthority,
  kVerificationProducer,
  kHiddenOmission,
};

struct EvidenceValidationIssue {
  EvidenceValidationCode code;
  std::string member_id;
  std::string message;
  std::optional<SourceSpan> source_span;
};

struct EvidenceValidationReport {
  std::vector<EvidenceValidationIssue> issues;
  bool ok() const { return issues.empty(); }
};
```

`RequireValidEvidenceCase` returns `Status::InvalidArgument` with the stable code name and member ID from the first issue; callers needing all failures use the report.

- [ ] **Step 4: Implement validation passes**

Implement separate private passes for ID collection, reference resolution, expression typing, path connectivity, provenance/epistemic checks, program binding, proof authority, and omission visibility. Do not mutate the case during validation.

Validation also resolves every M10B `query_provenance_id` to the supplied query
completion fact, run binding, and selected witness. A mismatch is reported as
`kMissingProvenance` or `kMixedProgramContext`; the validator never fetches or
synthesizes the missing provenance.

- [ ] **Step 5: Run validator tests**

Run: `ctest --test-dir build -R EvidenceValidatorTest --output-on-failure`

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add include/veritas/evidence/EvidenceValidator.h \
  src/evidence/EvidenceValidator.cpp src/evidence/CMakeLists.txt \
  tests/unit/evidence
git commit -m "feat: validate Evidence IR cases"
```

---

### Task 4: Canonicalize Evidence and Compute `EvidenceID`

**Files:**
- Create: `include/veritas/evidence/EvidenceCanonicalizer.h`
- Create: `src/evidence/EvidenceCanonicalizer.cpp`
- Modify: `src/evidence/CMakeLists.txt`
- Create: `tests/unit/evidence/EvidenceCanonicalizerTest.cpp`
- Modify: `tests/unit/evidence/CMakeLists.txt`

**Interfaces:**
- Consumes: validated `EvidenceCase`
- Produces: `StatusOr<std::vector<std::byte>> CanonicalEvidenceBytes(const EvidenceCase&)`
- Produces: `StatusOr<core::StableId> ComputeEvidenceId(const EvidenceCase&)`
- Produces: `Status FinalizeEvidenceIdentity(EvidenceCase*)`

- [ ] **Step 1: Write failing canonicalization tests**

```cpp
TEST(EvidenceCanonicalizerTest, IgnoresUnorderedInsertionOrder) {
  auto left = MakeOverflowEvidenceCase();
  auto right = left;
  std::reverse(right.entities.begin(), right.entities.end());
  std::reverse(right.facts.begin(), right.facts.end());
  ASSERT_TRUE(FinalizeEvidenceIdentity(&left).ok());
  ASSERT_TRUE(FinalizeEvidenceIdentity(&right).ok());
  EXPECT_EQ(left.evidence_id, right.evidence_id);
}

TEST(EvidenceCanonicalizerTest, EpistemicChangeChangesIdentity) {
  auto left = MakeOverflowEvidenceCase();
  auto right = left;
  right.facts.front().epistemic = EpistemicState::kMay;
  ASSERT_TRUE(FinalizeEvidenceIdentity(&left).ok());
  ASSERT_TRUE(FinalizeEvidenceIdentity(&right).ok());
  EXPECT_NE(left.evidence_id, right.evidence_id);
}
```

Also assert that path order is preserved, top-level display name and existing `evidence_id` are excluded, and context/dependency/omission/proof-status changes alter identity.

This test group implements `VID-007` and `VID-008`. Materialize the same typed
case in two temporary checkout roots and reverse every semantically unordered
collection for `VID-007`. For `VID-008`, mutate epistemic value, predicate,
ordered path segment, dependency, analyzer version, analysis run, and proof
status one at a time; each mutation must change canonical bytes and ID.

- [ ] **Step 2: Run the test to verify it fails**

Run: `ctest --test-dir build -R EvidenceCanonicalizerTest --output-on-failure`

Expected: FAIL because canonicalization does not exist.

- [ ] **Step 3: Implement semantic normalization**

Copy the case, clear `evidence_id`, sort semantically unordered collections by `(kind, stable_id-or-local-id)`, preserve path/entity sequence order, and recursively normalize expressions. For commutative `and`/`or`, sort canonical child encodings; never reorder comparison, implication, call arguments, or path segments.

- [ ] **Step 4: Encode with `core::CanonicalValue`**

Build one root object containing `schema`, `level`, `state`, the normalized semantic program binding, analyzer versions, semantic members, summaries, dependencies, and omissions. The local `project_root` path from the M10B `build::ProgramContext` never enters `EvidenceCase` or canonical bytes. Call `core::CanonicalEncode`; do not hash Protobuf or EIR-T bytes.

Implement:

```cpp
StatusOr<core::StableId> ComputeEvidenceId(const EvidenceCase& value) {
  auto bytes = CanonicalEvidenceBytes(value);
  if (!bytes.ok()) return bytes.status();
  return core::MakeStableId(core::IdKind::kEvidence, *bytes);
}
```

`FinalizeEvidenceIdentity` validates first, computes the ID, and assigns it only after every operation succeeds.

- [ ] **Step 5: Run canonicalization tests**

Run: `ctest --test-dir build -R EvidenceCanonicalizerTest --output-on-failure`

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add include/veritas/evidence/EvidenceCanonicalizer.h \
  src/evidence/EvidenceCanonicalizer.cpp src/evidence/CMakeLists.txt \
  tests/unit/evidence
git commit -m "feat: canonicalize Evidence IR identity"
```

---

### Task 5: Add the Protobuf Schema and Lossless Codec

**Files:**
- Create: `proto/veritas/evidence/v1/evidence.proto`
- Create: `include/veritas/evidence/EvidenceProto.h`
- Create: `src/evidence/EvidenceProto.cpp`
- Modify: `src/evidence/CMakeLists.txt`
- Create: `tests/unit/evidence/EvidenceProtoTest.cpp`
- Modify: `tests/unit/evidence/CMakeLists.txt`

**Interfaces:**
- Produces: Protobuf package `veritas.evidence.v1`
- Produces: `StatusOr<veritas::evidence::v1::EvidenceCase> ToEvidenceProto(const EvidenceCase&)`
- Produces: `StatusOr<EvidenceCase> FromEvidenceProto(const veritas::evidence::v1::EvidenceCase&)`
- Produces: `StatusOr<std::string> EncodeEvidenceProto(const EvidenceCase&)`
- Produces: `StatusOr<EvidenceCase> DecodeEvidenceProto(std::string_view)`

- [ ] **Step 1: Write the failing codec round-trip test**

```cpp
TEST(EvidenceProtoTest, RoundTripPreservesCanonicalIdentity) {
  auto input = MakeOverflowEvidenceCase();
  ASSERT_TRUE(FinalizeEvidenceIdentity(&input).ok());
  auto bytes = EncodeEvidenceProto(input);
  ASSERT_TRUE(bytes.ok()) << bytes.status().message();
  auto output = DecodeEvidenceProto(*bytes);
  ASSERT_TRUE(output.ok()) << output.status().message();
  auto output_id = ComputeEvidenceId(*output);
  ASSERT_TRUE(output_id.ok()) << output_id.status().message();
  EXPECT_EQ(*output_id, input.evidence_id);
}
```

Add negative tests for unspecified enums, missing schema/context, malformed bytes, dangling references after decode, and a serialized `evidence_id` that disagrees with recomputation.

Map these tests to `REP-002`, the Protobuf branch of `REP-003`, `REP-006`, and
`REP-008`. `REP-008` asserts decode failure returns no partially initialized
`EvidenceCase`. Add a legal non-canonical wire-order input and prove its decoded
canonical bytes and `EvidenceID` equal the canonical encoding.

- [ ] **Step 2: Run the test to verify it fails**

Run: `ctest --test-dir build -R EvidenceProtoTest --output-on-failure`

Expected: FAIL because the schema and codec do not exist.

- [ ] **Step 3: Define the Protobuf schema**

Use `syntax = "proto3";` and `package veritas.evidence.v1;`. The root message must have these stable field numbers:

```proto
message EvidenceCase {
  string evidence_id = 1;
  string schema_version = 2;
  EvidenceLevel level = 3;
  ProgramBinding program = 4;
  Claim primary_claim = 5;
  repeated Entity entities = 10;
  repeated Edge edges = 11;
  repeated Path paths = 12;
  repeated Fact facts = 13;
  repeated Assumption assumptions = 14;
  repeated Hypothesis hypotheses = 15;
  repeated Unknown unknowns = 16;
  repeated Constraint constraints = 17;
  repeated Provenance provenance = 18;
  repeated ProofObligation proof_obligations = 19;
  repeated SummaryReference summaries = 20;
  repeated Dependency dependencies = 21;
  VerificationState verification_state = 22;
  repeated Omission omissions = 23;
}

message Expression {
  ExpressionKind kind = 1;
  string text = 2;
  int64 integer = 3;
  bool boolean = 4;
  repeated Expression operands = 5;
}
```

Define one Protobuf message and enum for every Task 2 record and enum. Use strings for case-local IDs and canonical stable-ID strings for global IDs. Do not add timestamps, debug text, absolute paths, or protobuf `Any`.

- [ ] **Step 4: Add generated-code targets**

Follow `src/summary/CMakeLists.txt`:

```cmake
add_library(veritas_evidence_proto OBJECT
  "${CMAKE_SOURCE_DIR}/proto/veritas/evidence/v1/evidence.proto"
)
target_include_directories(veritas_evidence_proto PUBLIC
  "${CMAKE_CURRENT_BINARY_DIR}"
  ${Protobuf_INCLUDE_DIRS}
)
target_link_libraries(veritas_evidence_proto PUBLIC protobuf::libprotobuf)
if(TARGET absl::absl_log)
  target_link_libraries(veritas_evidence_proto PUBLIC
    absl::absl_log
    absl::absl_check
  )
endif()
protobuf_generate(
  TARGET veritas_evidence_proto
  IMPORT_DIRS "${CMAKE_SOURCE_DIR}/proto"
  PROTOC_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}"
)
target_link_libraries(veritas_evidence PUBLIC veritas_evidence_proto)
```

Add `EvidenceProto.cpp` to `veritas_evidence` and expose the generated include directory through that target.

- [ ] **Step 5: Implement explicit domain/proto conversion**

Write one conversion function per message and enum. Reject every `*_UNSPECIFIED` enum, parse all stable IDs through `core::ParseStableId`, validate after decoding, and recompute `EvidenceID`; return `InvalidArgument` when the encoded ID differs.

Use `SerializeToString`/`ParseFromArray` only after conversion and validation. Do not expose generated Protobuf types from `EvidenceCase.h`.

- [ ] **Step 6: Run codec tests**

Run:

```bash
cmake --build --preset default
ctest --test-dir build -R EvidenceProtoTest --output-on-failure
```

Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add proto/veritas/evidence/v1/evidence.proto \
  include/veritas/evidence/EvidenceProto.h src/evidence/EvidenceProto.cpp \
  src/evidence/CMakeLists.txt tests/unit/evidence
git commit -m "feat: serialize Evidence IR with protobuf"
```

---

### Task 6: Tokenize Stabilized EIR-T

**Files:**
- Create: `include/veritas/evidence/EirText.h`
- Create: `src/evidence/EirSyntax.h`
- Create: `src/evidence/EirLexer.cpp`
- Modify: `src/evidence/CMakeLists.txt`
- Create: `tests/unit/evidence/EirLexerTest.cpp`
- Modify: `tests/unit/evidence/CMakeLists.txt`

**Interfaces:**
- Produces: internal `Token`, `TokenKind`, and `EirLexer`
- Produces: public `EirParseError {offset, line, column, message}`
- Consumes: UTF-8 `std::string_view`

- [ ] **Step 1: Write failing lexer tests**

```cpp
TEST(EirLexerTest, TracksTokensAndSourceCoordinates) {
  EirParseError error;
  EirLexer lexer("evidence E {\n  schema = \"eir.v1\";\n}", &error);
  auto tokens = lexer.Tokenize();
  ASSERT_TRUE(tokens.ok()) << error.message;
  EXPECT_EQ((*tokens)[0].text, "evidence");
  EXPECT_EQ((*tokens)[3].line, 2U);
  EXPECT_EQ((*tokens)[3].column, 3U);
}

TEST(EirLexerTest, RejectsInvalidEscapeWithLocation) {
  EirParseError error;
  EirLexer lexer("\"bad\\q\"", &error);
  EXPECT_FALSE(lexer.Tokenize().ok());
  EXPECT_EQ(error.line, 1U);
  EXPECT_EQ(error.column, 5U);
}
```

Cover line/block comments, escaped strings, negative integers, `@` and `$` references, qualified identifiers, punctuation, `->`, comparisons, EOF, unterminated string/comment, invalid UTF-8, and integer overflow.

- [ ] **Step 2: Run the test to verify it fails**

Run: `ctest --test-dir build -R EirLexerTest --output-on-failure`

Expected: FAIL because the lexer does not exist.

- [ ] **Step 3: Define the token model**

Use these token kinds:

```cpp
enum class TokenKind {
  kIdentifier,
  kString,
  kInteger,
  kAt,
  kDollar,
  kLeftBrace,
  kRightBrace,
  kLeftParen,
  kRightParen,
  kLeftBracket,
  kRightBracket,
  kColon,
  kSemicolon,
  kComma,
  kEqual,
  kEqualEqual,
  kNotEqual,
  kLess,
  kLessEqual,
  kGreater,
  kGreaterEqual,
  kArrow,
  kEnd,
};
```

`Token` stores `kind`, decoded `text`, byte `offset`, one-based `line`, and one-based `column`. Keywords remain identifier tokens and are matched by text in the parser.

- [ ] **Step 4: Implement the exception-free lexer**

Use cursor methods `Peek`, `Advance`, `SkipWhitespaceAndComments`, `LexIdentifier`, `LexString`, and `LexInteger`. Parse integers with `std::from_chars`; return `Status::InvalidArgument` and fill `EirParseError` on the first failure.

- [ ] **Step 5: Run lexer tests**

Run: `ctest --test-dir build -R EirLexerTest --output-on-failure`

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add include/veritas/evidence/EirText.h src/evidence/EirSyntax.h \
  src/evidence/EirLexer.cpp src/evidence/CMakeLists.txt tests/unit/evidence
git commit -m "feat: tokenize EIR-T"
```

---

### Task 7: Parse EIR-T and Lower It to the Semantic Model

**Files:**
- Create: `src/evidence/EirParser.cpp`
- Modify: `include/veritas/evidence/EirText.h`
- Modify: `src/evidence/EirSyntax.h`
- Modify: `src/evidence/CMakeLists.txt`
- Create: `tests/unit/evidence/EirParserTest.cpp`
- Modify: `tests/unit/evidence/CMakeLists.txt`

**Interfaces:**
- Produces: `StatusOr<EvidenceCase> ParseEirText(std::string_view, EirParseError*)`
- Consumes: Task 6 tokens
- Consumes: Task 3 validation

- [ ] **Step 1: Write failing parser tests**

Start with the complete overflow case from the formal specification, updated for `eir.v1`:

```cpp
TEST(EirParserTest, ParsesCompleteOverflowCase) {
  EirParseError error;
  auto value = ParseEirText(kOverflowEirText, &error);
  ASSERT_TRUE(value.ok()) << error.line << ':' << error.column << ' '
                          << error.message;
  EXPECT_EQ(value->schema_version, "eir.v1");
  EXPECT_EQ(value->level, EvidenceLevel::kL1);
  EXPECT_EQ(value->verification_state,
            VerificationState::kPossibleDefect);
  EXPECT_EQ(value->primary_claim.kind, ClaimKind::kBufferOverflow);
  EXPECT_EQ(value->dependencies.size(), 2U);
  EXPECT_EQ(value->omissions.size(), 1U);
}
```

Add individual tests for every member declaration, expression precedence (`not` > comparison > `and` > `or` > `implies`), right-associative implication, quantifier scope, comments, repeated comparison rejection, duplicate top-level properties, missing required context, unsupported schema, unknown keyword, and dangling reference validation.

The unsupported-schema and unknown-required-syntax cases implement `REP-005`.
The invalid escape, unterminated input, repeated comparison, precedence, and
one-based location cases implement `REP-007` using the reviewed malformed
corpus under `tests/fixtures/evidence/invalid/`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `ctest --test-dir build -R EirParserTest --output-on-failure`

Expected: FAIL because `ParseEirText` does not exist.

- [ ] **Step 3: Implement recursive-descent parser structure**

Use one parse method per production:

```text
ParseEvidenceCase
ParseContext
ParseClaim / ParseEntity / ParseFact / ParseAssumption / ParseHypothesis
ParseUnknown / ParseEdge / ParsePath / ParseConstraint / ParseProvenance
ParseVerification / ParseSummary / ParseDependency / ParseOmission
ParsePredicate -> ParseImplication -> ParseOr -> ParseAnd
               -> ParseComparison -> ParseUnary -> ParsePrimary
```

Use `Consume(kind)`, `ConsumeIdentifier(text)`, `Match`, and `Fail` helpers. `Fail` records the current token's line/column once and returns `InvalidArgument`.

- [ ] **Step 4: Lower syntax directly into domain records**

Parse stable-ID strings through `core::ParseStableId`, parse textual enums through Task 2 helpers, retain case-local references as strings, and attach source spans to expressions/members used in diagnostics. Do not construct Protobuf messages in the parser.

- [ ] **Step 5: Validate and verify encoded identity**

After parsing, call `RequireValidEvidenceCase`, compute `EvidenceID`, and assign it to the parsed case. Treat the source top-level identifier as a non-semantic display label; canonical writing replaces it with the digest-derived identifier.

- [ ] **Step 6: Run parser tests**

Run: `ctest --test-dir build -R EirParserTest --output-on-failure`

Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add include/veritas/evidence/EirText.h src/evidence/EirParser.cpp \
  src/evidence/EirSyntax.h src/evidence/CMakeLists.txt tests/unit/evidence
git commit -m "feat: parse EIR-T"
```

---

### Task 8: Write Canonical and Pretty EIR-T

**Files:**
- Create: `src/evidence/EirWriter.cpp`
- Modify: `include/veritas/evidence/EirText.h`
- Modify: `src/evidence/CMakeLists.txt`
- Create: `tests/unit/evidence/EirWriterTest.cpp`
- Modify: `tests/unit/evidence/CMakeLists.txt`

**Interfaces:**
- Produces: `enum class EirTextStyle { kCanonical, kPretty }`
- Produces: `StatusOr<std::string> WriteEirText(const EvidenceCase&, EirTextStyle)`
- Consumes: Task 3 validation and Task 4 canonical identity

- [ ] **Step 1: Write failing writer and round-trip tests**

```cpp
TEST(EirWriterTest, CanonicalTextRoundTripsIdentity) {
  auto input = MakeOverflowEvidenceCase();
  ASSERT_TRUE(FinalizeEvidenceIdentity(&input).ok());
  auto text = WriteEirText(input, EirTextStyle::kCanonical);
  ASSERT_TRUE(text.ok()) << text.status().message();
  EirParseError error;
  auto output = ParseEirText(*text, &error);
  ASSERT_TRUE(output.ok()) << error.message;
  EXPECT_EQ(output->evidence_id, input.evidence_id);
  EXPECT_EQ(*WriteEirText(*output, EirTextStyle::kCanonical), *text);
}
```

Also test deterministic pretty output, string escaping, precedence-preserving parentheses, member ordering, lower-case EIR enum spellings where required, path order, and rejection of invalid/unfinalized cases.

The canonical write/parse/write assertion is `REP-001`: both passes must retain
identical canonical semantic bytes, `EvidenceID`, and canonical EIR-T. Pretty
formatting may differ in whitespace only and must parse to the same semantic
bytes.

- [ ] **Step 2: Run the test to verify it fails**

Run: `ctest --test-dir build -R EirWriterTest --output-on-failure`

Expected: FAIL because the writer does not exist.

- [ ] **Step 3: Implement one ordered writer over the semantic model**

Validate and recompute `EvidenceID` before output. Derive the top-level case identifier as `E_` plus the 64 lowercase digest characters. Emit top-level properties in `schema`, `level`, `state`, `context` order, then members in this order:

```text
entity, claim, fact, assumption, hypothesis, unknown, edge, path,
constraint, provenance, verify, summary, dependency, omission
```

Within each member category, use canonicalizer order. Pretty mode changes indentation and line breaks only.

- [ ] **Step 4: Implement precedence-aware expression writing**

Assign binding powers `atom=60`, `not=50`, `comparison=40`, `and=30`, `or=20`, `implies=10`, `quantifier=0`. Add parentheses whenever the child binding power is lower, when a comparison contains another comparison, or when implication associativity would change.

- [ ] **Step 5: Run all EIR-T tests**

Run: `ctest --test-dir build -R "EirLexer|EirParser|EirWriter" --output-on-failure`

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add include/veritas/evidence/EirText.h src/evidence/EirWriter.cpp \
  src/evidence/CMakeLists.txt tests/unit/evidence
git commit -m "feat: write canonical EIR-T"
```

---

### Task 9: Assemble M10B Inputs into L0/L1/L2 Evidence Cases

**Files:**
- Create: `include/veritas/evidence/EvidencePredicateMapper.h`
- Create: `src/evidence/EvidencePredicateMapper.cpp`
- Create: `include/veritas/evidence/EvidenceCaseBuilder.h`
- Create: `src/evidence/EvidenceCaseBuilder.cpp`
- Modify: `src/evidence/CMakeLists.txt`
- Create: `tests/unit/evidence/EvidencePredicateMapperTest.cpp`
- Create: `tests/unit/evidence/EvidenceCaseBuilderTest.cpp`
- Modify: `tests/unit/evidence/CMakeLists.txt`

**Interfaces:**
- Consumes: M10B `EvidenceBuildInput` wrapped by M10C `EvidenceBuildRequest`
- Produces: `StatusOr<Fact> EvidencePredicateMapper::MapFact(const facts::Fact&, std::string local_id) const`
- Produces: `StatusOr<EvidenceCase> EvidenceCaseBuilder::Build(const EvidenceBuildRequest&) const`

- [ ] **Step 1: Write failing predicate-mapping tests**

Use typed M9/M10A fact fixtures and assert exact EIR expressions:

```cpp
TEST(EvidencePredicateMapperTest, MapsRangeWithoutStrengtheningEpistemic) {
  auto input = MakeRangeFact(
      "value:sha256:0000000000000000000000000000000000000000000000000000000000000001",
      0, 65535,
                             facts::EpistemicState::kMay);
  auto output = EvidencePredicateMapper().MapFact(input, "F_range");
  ASSERT_TRUE(output.ok()) << output.status().message();
  EXPECT_EQ(output->predicate.kind, Expression::Kind::kCall);
  EXPECT_EQ(output->predicate.text, "range");
  EXPECT_EQ(output->epistemic, EpistemicState::kMay);
  EXPECT_EQ(output->provenance_id, "PR_range");
}
```

Cover `range`, `capacity`, `reachability`, `alias`, and `memory_effect`; reject unsupported relation names with `InvalidArgument`/`UNSUPPORTED` text instead of dropping them.

- [ ] **Step 2: Write failing builder-level tests**

```cpp
TEST(EvidenceCaseBuilderTest, TruncatedNoCheckBecomesUnknown) {
  auto request = EvidenceScenarioBuilder()
      .WithTruncatedDominatingChecks(TruncationReason::kMaxPaths)
      .BuildRequest(EvidenceLevel::kL1);
  auto output = EvidenceCaseBuilder().Build(request);
  ASSERT_TRUE(output.ok()) << output.status().message();
  EXPECT_THAT(output->unknowns,
              testing::Contains(testing::Field(&Unknown::reason,
                                                "dominating-check query truncated")));
  EXPECT_THAT(output->facts,
              testing::Not(testing::Contains(
                  testing::Field(&Fact::epistemic,
                                 EpistemicState::kMustNot))));
}
```

Add tests that L0 contains only the claim summary and primary evidence references, L1 contains the causal flow/range/capacity/dominance/provenance slice, and L2 includes available detailed evidence plus explicit omissions for unavailable expansion targets.

Implement the complete `BLD` catalog in this file:

```text
BLD-001  unsafe complete input builds valid L1
BLD-002  safe dominating check remains counterevidence without a verdict
BLD-003  sibling checks and mixed paths never become universal checks
BLD-004  opaque validation produces a blocking unknown
BLD-005  uncertain alias prevents strengthened capacity reasoning
BLD-006  truncated empty check output cannot become negative evidence
BLD-007  complete closed-world absence requires matching query provenance
BLD-008  L0 declares an omission for every removed member
BLD-009  L2 retains detail and marks unavailable expansion
BLD-010  unsupported relations and mixed contexts are rejected
```

For `BLD-007`, parameterize missing completion fact, mismatched run/scope/budget,
missing selected witness, and one fully valid complete result. Only the valid
case may emit a negative fact, and its provenance input must be the completion
fact ID.

- [ ] **Step 3: Run mapper and builder tests to verify they fail**

Run:

```bash
ctest --test-dir build -R "EvidencePredicateMapper|EvidenceCaseBuilder" \
  --output-on-failure
```

Expected: FAIL because the mapper and builder do not exist.

- [ ] **Step 4: Implement the versioned predicate mapper**

Dispatch on the M9 relation ID, verify arity and typed cells through the relation registry, map stable references to case-local entity IDs, copy epistemic/confidence/provenance fields exactly, and set `derived` from producer metadata. Keep each built-in relation in a focused private method.

Before mapping domain facts, resolve and validate every query completion fact
against the supplied payload and metadata. Do not call FactStore,
ProvenanceStore, CPG, or any analyzer from the mapper or builder.

The mapping table is:

| M9/M10A relation | EIR predicate |
| --- | --- |
| range | `range(@value, min, max)` |
| capacity | `capacity(@memory, bytes)` |
| reachability | `reachable(@from, @to)` |
| alias | `alias(@left, @right)` |
| memory effect | `reads(@function, @memory)` or `writes(@function, @memory)` |

- [ ] **Step 5: Implement deterministic entity and local-ID assignment**

Collect every stable entity referenced by the claim, flow path, facts, summaries, provenance, and omissions. Sort by canonical stable-ID text, assign local IDs from kind plus the first collision-free digest prefix, and never derive IDs from source filenames or line numbers.

- [ ] **Step 6: Implement claim and proof-obligation construction**

For `buffer_overflow`, build:

```text
claim predicate: value(@copy_length) > capacity(@destination)
case state: POSSIBLE_DEFECT
proof goal: forall path reaching @sink:
              value(@copy_length) <= capacity(@destination)
proof status: PENDING
verifier kinds: [range_analysis, smt, symbolic_execution]
```

Unknown claim kinds return `InvalidArgument`; they are not converted into generic claims.

For the registered closed-world dominating-check query only, a complete empty
result with a valid completion fact and selected witness may derive
`MUST_NOT dominates_bounds_check(scope, sink)`. Create its provenance with rule
ID `evidence.closed_world.dominating_check_absence.v1` and the completion fact
as an input. Every open-world or truncated empty result becomes an explicit
unknown or omission.

- [ ] **Step 7: Implement level projection and omission rules**

Build one complete internal case, then project:

- L0 retains program identity, claim, severity, primary facts/path reference, primary unknown, proof obligation, dependencies, and omissions for every removed L1/L2 member.
- L1 retains every causally relevant M10B entity/edge/path, supporting and contradicting fact, constraint, summary, provenance root, unknown, and truncation marker.
- L2 adds supplied provenance DAGs, alias sets, constraints, and analyzer expressions; every unavailable requested expansion becomes `Omission{expandable=true}`.

Validate and call `FinalizeEvidenceIdentity` after projection.

- [ ] **Step 8: Run mapper and builder tests**

Run:

```bash
ctest --test-dir build -R "EvidencePredicateMapper|EvidenceCaseBuilder" \
  --output-on-failure
```

Expected: PASS.

- [ ] **Step 9: Commit**

```bash
git add include/veritas/evidence/EvidencePredicateMapper.h \
  include/veritas/evidence/EvidenceCaseBuilder.h \
  src/evidence/EvidencePredicateMapper.cpp \
  src/evidence/EvidenceCaseBuilder.cpp src/evidence/CMakeLists.txt \
  tests/unit/evidence
git commit -m "feat: build Evidence IR from evidence slices"
```

---

### Task 10: Emit Deterministic Full-EIR JSON

**Files:**
- Create: `include/veritas/evidence/EvidenceJson.h`
- Create: `src/evidence/EvidenceJson.cpp`
- Modify: `src/evidence/CMakeLists.txt`
- Create: `tests/unit/evidence/EvidenceJsonTest.cpp`
- Modify: `tests/unit/evidence/CMakeLists.txt`

**Interfaces:**
- Produces: `StatusOr<std::string> ToEvidenceJson(const EvidenceCase&)`
- Consumes: validated and finalized `EvidenceCase`

- [ ] **Step 1: Write the failing JSON determinism test**

```cpp
TEST(EvidenceJsonTest, EmitsFullCaseInCanonicalOrder) {
  auto value = MakeOverflowEvidenceCase();
  ASSERT_TRUE(FinalizeEvidenceIdentity(&value).ok());
  auto first = ToEvidenceJson(value);
  auto second = ToEvidenceJson(value);
  ASSERT_TRUE(first.ok()) << first.status().message();
  ASSERT_TRUE(second.ok()) << second.status().message();
  EXPECT_EQ(*first, *second);
  EXPECT_THAT(*first, testing::HasSubstr("\"schema_version\": \"eir.v1\""));
  EXPECT_THAT(*first, testing::HasSubstr("\"evidence_id\": \"evidence:sha256:"));
  EXPECT_THAT(*first, testing::HasSubstr("\"omissions\""));
}
```

Also verify insertion-order invariance, JSON escaping, explicit truncation/omissions, and rejection of invalid or stale `evidence_id` values.

This group implements `REP-004`. Parse the emitted JSON into a typed semantic
oracle and compare every member; substring checks alone do not satisfy the
case. Reverse independent member insertion and require byte-identical output
with exactly one final newline.

- [ ] **Step 2: Run the test to verify it fails**

Run: `ctest --test-dir build -R EvidenceJsonTest --output-on-failure`

Expected: FAIL because `ToEvidenceJson` does not exist.

- [ ] **Step 3: Implement JSON using LLVM's ordered stream**

Follow `build::ToDiagnosticJson`: use `llvm::json::OStream` and `llvm::raw_string_ostream`, emit the canonicalized member order, use lower-case schema spellings consistently, append one final newline, and expose every semantic field. Do not reuse M10B's `FlowSlice` JSON serializer.

Link `LLVMSupport` privately from `veritas_evidence` and add LLVM include directories as `SYSTEM PRIVATE`, matching `src/build/CMakeLists.txt`.

- [ ] **Step 4: Run JSON tests**

Run: `ctest --test-dir build -R EvidenceJsonTest --output-on-failure`

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/veritas/evidence/EvidenceJson.h src/evidence/EvidenceJson.cpp \
  src/evidence/CMakeLists.txt tests/unit/evidence
git commit -m "feat: emit full Evidence IR JSON"
```

---

### Task 11: Add Full-EIR CLI Formats and Overflow Goldens

**Files:**
- Modify: `src/tools/veritas-query.cpp`
- Modify: `src/tools/CMakeLists.txt`
- Create: `tests/integration/evidence/VeritasQueryEirTest.cpp`
- Create: `tests/golden/evidence/overflow_unsafe.l0.eir`
- Create: `tests/golden/evidence/overflow_unsafe.l1.eir`
- Create: `tests/golden/evidence/overflow_unsafe.l1.eir.json`
- Create: `tests/golden/evidence/overflow_safe.l1.eir`
- Create: `tests/golden/evidence/overflow_truncated.l1.eir`
- Modify: `tests/integration/evidence/CMakeLists.txt`

**Interfaces:**
- Produces: `--level l0|l1|l2`
- Produces: `--format eir-t|eir-json|protobuf`
- Produces: failure-atomic `--output <path>` for binary Protobuf
- Preserves: M10B `--format json` slice output

- [ ] **Step 1: Write failing CLI integration tests**

Register the built `veritas-query` path through `VERITAS_QUERY_BINARY`. Define
a local `CliResult { int exit_code; std::string stdout_text; std::string
stderr_text; }` and `RunVeritasQuery(std::vector<std::string>)` using the same
temporary-file and `std::system` pattern as
`tests/integration/build/VeritasBuildAnalyzeCliTest.cpp`; quote every argument
through that test's existing shell-quoting helper. Then test:

```cpp
TEST(VeritasQueryEirTest, EmitsCanonicalOverflowEirText) {
  const auto result = RunVeritasQuery({
      "evidence", "overflow", "--sink", "memcpy", "--level", "l1",
      "--format", "eir-t"});
  ASSERT_EQ(result.exit_code, 0) << result.stderr_text;
  EXPECT_EQ(result.stdout_text, ReadGolden("overflow_unsafe.l1.eir"));
}

TEST(VeritasQueryEirTest, ProtobufRequiresOutputPath) {
  const auto result = RunVeritasQuery({
      "evidence", "overflow", "--sink", "memcpy", "--level", "l1",
      "--format", "protobuf"});
  EXPECT_NE(result.exit_code, 0);
  EXPECT_THAT(result.stderr_text,
              testing::HasSubstr("protobuf requires --output"));
}
```

Also test `eir-json` golden output, all three levels, invalid level/format, EIR-T versus Protobuf `EvidenceID` equality, preservation of slice `json`, and failure cleanup when an output destination cannot be replaced.

Implement `DEM-002` through `DEM-006`: validated L0/L1/L2 projection, semantic
agreement among text/JSON/Protobuf, required and failure-atomic Protobuf output,
distinct unsafe/safe/truncated outputs, and repeated runs in alternate checkout
roots. `DEM-003` parses or decodes every representation and compares canonical
semantic bytes; golden text alone is not its oracle.

- [ ] **Step 2: Run the CLI test to verify it fails**

Run: `ctest --test-dir build -R VeritasQueryEirTest --output-on-failure`

Expected: FAIL because the formats are not recognized.

- [ ] **Step 3: Extend CLI parsing without changing M10B JSON behavior**

Add `--level`, `--format`, and `--output` parsing to the `evidence` command. Default level is `l1`; retain M10B's existing `json` meaning. Reject `--output` for text formats unless the command explicitly supports text-file output, and reject Protobuf without it.

- [ ] **Step 4: Build and serialize the case**

Call the M10B input builder, pass the immutable input to `EvidenceCaseBuilder`, and dispatch:

```cpp
if (format == "eir-t") return WriteEirText(value, EirTextStyle::kCanonical);
if (format == "eir-json") return ToEvidenceJson(value);
if (format == "protobuf") return EncodeEvidenceProto(value);
```

Do not reconstruct EIR independently inside the CLI.

- [ ] **Step 5: Implement failure-atomic binary output**

Write Protobuf bytes to a uniquely named sibling temporary file, flush and close successfully, then replace the destination with `std::filesystem::rename`. On any error, remove only that exact temporary file. Never truncate the destination before successful encoding and validation.

- [ ] **Step 6: Generate and review canonical goldens**

Generate the five M10C goldens from isolated unsafe, safe, and mixed-path stores.
Review typed output before accepting text. The unsafe L1 forms include one
claim, range/capacity facts, value-flow path, completeness-qualified dominance
result, provenance, summaries, dependencies, one `PENDING` proof obligation,
and visible omissions. The safe golden contains positive dominating-check
counterevidence without `VERIFIED_SAFE`; the truncated golden contains the
stable reason and no negative fact.

- [ ] **Step 7: Run CLI integration tests**

Run: `ctest --test-dir build -R VeritasQueryEirTest --output-on-failure`

Expected: `DEM-002` through `DEM-006` pass. Register the suite with labels
`evidence-integration`, `evidence-roundtrip`, and `evidence-cli`.

- [ ] **Step 8: Commit**

```bash
git add src/tools/veritas-query.cpp src/tools/CMakeLists.txt \
  tests/integration/evidence tests/golden/evidence
git commit -m "feat: emit Evidence IR from veritas-query"
```

---

### Task 12: M10C End-to-End and Pre-Push Verification

**Files:**
- Verify only; no intended source changes

**Interfaces:**
- Verifies: EIR-T, Protobuf, and EIR JSON semantic equivalence
- Verifies: M10B slice JSON compatibility
- Verifies: all 54 stable companion-contract cases
- Verifies: complete repository build/test policy

- [ ] **Step 1: Run all M10C-focused tests**

```bash
ctest --test-dir build \
  -R "EvidenceCase|EvidenceValidator|EvidenceCanonicalizer|EvidenceProto|EirLexer|EirParser|EirWriter|EvidencePredicateMapper|EvidenceCaseBuilder|EvidenceJson|VeritasQueryEir|veritas_eir_contract_docs" \
  --no-tests=error --output-on-failure
ctest --test-dir build -L evidence-roundtrip --output-on-failure
ctest --test-dir build -L evidence-cli --output-on-failure
```

Audit the test sources against the companion catalog and confirm `BLD-001`–
`BLD-010`, `VID-001`–`VID-008`, `REP-001`–`REP-008`, and `DEM-002`–`DEM-006`
are enabled and passing. Expected: all 31 M10C-owned cases pass with no skips.

- [ ] **Step 2: Run the end-to-end demo in every representation**

```bash
./build/bin/veritas-query evidence overflow --sink memcpy \
  --level l0 --format eir-t
./build/bin/veritas-query evidence overflow --sink memcpy \
  --level l1 --format eir-t
./build/bin/veritas-query evidence overflow --sink memcpy \
  --level l1 --format eir-json
./build/bin/veritas-query evidence overflow --sink memcpy \
  --level l1 --format protobuf --output build/overflow.eir.pb
./build/bin/veritas-query evidence overflow --sink memcpy \
  --level l2 --format eir-t
```

Parse the L1 EIR-T and EIR JSON and decode `build/overflow.eir.pb`; confirm
those three L1 representations have identical canonical semantic bytes and one
`EvidenceID`. Validate L0 and L2 independently because level is semantic and
therefore changes identity.

- [ ] **Step 3: Verify M10B compatibility**

Run:

```bash
./build/bin/veritas-query evidence overflow --sink memcpy --format json
ctest --test-dir build -L evidence-contract --output-on-failure
ctest --test-dir build -L evidence-integration --output-on-failure
```

Expected: the M10B slice output remains diagnostic JSON, and all 23 M10B-owned
`AC`, `QRY`, `HND`, and `DEM-001` cases pass. Together with Step 1, all 54
catalog cases are green.

- [ ] **Step 4: Run the mandatory clean build**

```bash
rm -rf build
cmake --preset default
cmake --build --preset default
```

Expected: clean configure and build complete with zero errors.

- [ ] **Step 5: Run the full test suite**

Run: `ctest --test-dir build --output-on-failure`

Expected: 100% pass with no catalog-case skips and only repository-approved
non-catalog skips.

- [ ] **Step 6: Run formatting and license checks**

```bash
git diff --check main...HEAD
missing=$(git diff --name-only main...HEAD \
  | while IFS= read -r file; do
      case "$file" in
        *.h|*.hpp|*.hh|*.c|*.cc|*.cpp|*.cxx|*.proto|CMakeLists.txt|*.cmake)
          head -20 "$file" | grep -q \
            'Licensed under the Apache License, Version 2.0' || echo "$file"
          ;;
      esac
    done)
test -z "$missing"
```

Expected: no whitespace errors and no missing license headers.

- [ ] **Step 7: Verify final branch state**

```bash
git status --porcelain
git diff --stat main...HEAD
git log --oneline main..HEAD
```

Expected: clean worktree; branch contains only M10C grammar, semantic-model, serialization, assembly, CLI, tests, and golden changes.
