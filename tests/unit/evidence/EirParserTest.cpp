// Copyright 2026 VERITAS Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// EirParserTest.cpp — `ParseEirText`, the EIR-T parser and lowerer.
//
// The suite is organized around two properties the milestone plan makes
// falsifiable:
//
//   * REP-005 and REP-007 — every construct the grammar does not admit, and
//     every construct it admits but the `eir.v1` model cannot carry, is
//     refused. `Rejects*` holds one test per reject site, each asserting the
//     specific diagnostic rather than merely that the call failed, so a
//     mutation that neutralises one site turns exactly one test red.
//   * §3.1's label rule — the case `Identifier` is a display label with no
//     semantic content. `CaseLabelCarriesNoSemanticContent` proves it by
//     parsing the same case with and without one and comparing the computed
//     `EvidenceID`.
//
// The positive tests are written against the shared fixture
// `tests/support/evidence/EirFixtureText.h`, which the writer task consumes
// too: a fixture owned by one side would let the parser and the writer drift
// apart, and REP-001 would then be asserted against two different documents.

#include "veritas/evidence/EirText.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "ProjectFixture.h"
#include "evidence/EirFixtureText.h"
#include "evidence/EirSyntax.h"
#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/evidence/EvidenceCase.h"

namespace veritas::evidence {
namespace {

namespace fs = std::filesystem;

// --- Source construction ----------------------------------------------------

// The amended context block of §3.1, complete: every one of the seven
// single-valued properties plus one repeatable `analyzer`. A malformed document
// built on top of it therefore cannot fail for a missing program context, and
// each `Rejects*` test can assert the error code it is actually about.
constexpr std::string_view kContextBlock = R"EIR(    context {
        repository = "radio-stack";
        revision = "a87f03e";
        build_variant = "ARM64_RELEASE";
        target = "aarch64-unknown-linux-gnu";
        analyzer_configuration = "veritas.default";
        type_layout = "layout:aapcs64";
        analysis_run = "run:sha256:0d5c1e2f3a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5";
    }

)EIR";

// Two entities: the members a document needs so its claim has a subject, and
// no claim yet. Keeping the claim out of the base lets a test inject one extra
// member without accidentally injecting a second primary claim, which would
// mask the defect the test is about.
constexpr std::string_view kBaseEntities = R"EIR(    entity E_len : value {
        origin = "packet.length";
    }

    entity E_sink : callsite {
        function = "memcpy";
    }

)EIR";

// The one primary claim, cited by `kBaseEntities`'s second entity.
constexpr std::string_view kPrimaryClaim = R"EIR(    claim C1 {
        kind = buffer_overflow;
        subject = @E_sink;
        predicate = reachable(@E_sink);
        severity = high;
    }

)EIR";

// A complete case built from the standard header, the standard context, and
// `members`.
std::string Case(std::string_view members) {
  return std::string("evidence {\n") +
         "    schema = \"eir.v1\";\n"
         "    level = l1;\n"
         "    state = POSSIBLE_DEFECT;\n\n" +
         std::string(kContextBlock) + std::string(members) + "}\n";
}

// The smallest complete case: the two entities and the primary claim.
std::string MinimalCase() {
  return Case(std::string(kBaseEntities) + std::string(kPrimaryClaim));
}

// The minimal case with `members` inserted before the primary claim, so the
// claim remains the document's only one whatever the caller adds.
std::string CasePlus(std::string_view members) {
  return Case(std::string(kBaseEntities) + std::string(members) +
              std::string(kPrimaryClaim));
}

// `CasePlus` with exactly one extra member.
std::string CaseWith(std::string_view member) { return CasePlus(member); }

// The minimal case whose claim carries `predicate` instead of the default.
std::string ClaimCase(std::string_view predicate) {
  return Case(std::string(kBaseEntities) +
              "    claim C1 {\n"
              "        kind = buffer_overflow;\n"
              "        subject = @E_sink;\n"
              "        predicate = " +
              std::string(predicate) +
              ";\n"
              "        severity = high;\n"
              "    }\n");
}

// A case whose only predicate sits in a constraint, for the shape tests that
// need a parser error at a known point in the expression grammar rather than a
// validator error about the case.
std::string PredicateOnlyCase(std::string_view predicate) {
  return Case(std::string(kBaseEntities) +
              "    constraint K1 {\n"
              "        expr = " +
              std::string(predicate) +
              ";\n"
              "        epistemic = must;\n"
              "    }\n" +
              std::string(kPrimaryClaim));
}

// --- Result inspection ------------------------------------------------------

struct Rejection {
  bool rejected = false;
  EirParseError error;
};

Rejection Reject(std::string_view source) {
  EirParseError error;
  StatusOr<EvidenceCase> value = ParseEirText(source, &error);
  Rejection rejection;
  rejection.rejected = !value.ok();
  rejection.error = std::move(error);
  return rejection;
}

// Parses a document that must succeed, reporting the diagnostic when it does
// not. `context` names the test's own source for the failure message.
StatusOr<EvidenceCase> Accept(std::string_view source,
                             std::string_view context) {
  EirParseError error;
  StatusOr<EvidenceCase> value = ParseEirText(source, &error);
  EXPECT_TRUE(value.ok()) << context << ": " << error.line << ':'
                          << error.column << ' ' << error.message;
  return value;
}

// Lexes and parses without the validator gate, returning the lowered case even
// where it is not also a well-formed case. `EirParser::Parse` is the lowering
// step alone; `ParseEirText` is that step plus `FinalizeEvidenceIdentity`. The
// precedence tests read the tree the grammar builds, and one of the groupings
// the precedence table mandates is ill-typed by construction, so the public
// entry point cannot be used to observe it.
StatusOr<EvidenceCase> ParseUnvalidated(std::string_view source) {
  EirParseError error;
  EirLexer lexer(source, &error);
  StatusOr<std::vector<Token>> tokens = lexer.Tokenize();
  if (!tokens.ok()) {
    return tokens.status();
  }
  EirParser parser(tokens.value(), &error);
  return parser.Parse();
}

// The path of one file of the malformed corpus of
// `docs/specs/milestones/m10b-m10c-api-to-evidence-ir-test-design-spec.md` §6.3.
fs::path CorpusPath(std::string_view name) {
  return testing::TestSourceRoot() / "fixtures" / "evidence" / "invalid" / name;
}

std::string ReadCorpus(std::string_view name) {
  std::ifstream stream(CorpusPath(name), std::ios::binary);
  if (!stream.is_open()) {
    return {};
  }
  std::ostringstream contents;
  contents << stream.rdbuf();
  return contents.str();
}

// --- Positive: the shared fixture -------------------------------------------

TEST(EirParserTest, ParsesCompleteOverflowCase) {
  EirParseError error;
  StatusOr<EvidenceCase> value = ParseEirText(testing::kOverflowEirText, &error);
  ASSERT_TRUE(value.ok()) << error.line << ':' << error.column << ' '
                          << error.message;
  EXPECT_EQ(value->schema_version, "eir.v1");
  EXPECT_EQ(value->level, EvidenceLevel::kL1);
  EXPECT_EQ(value->verification_state, VerificationState::kPossibleDefect);
  EXPECT_EQ(value->primary_claim.kind, ClaimKind::kBufferOverflow);
  EXPECT_EQ(value->dependencies.size(), 2U);
  EXPECT_EQ(value->omissions.size(), 1U);
  EXPECT_TRUE(value->evidence_id.has_value());
  EXPECT_EQ(value->evidence_id->kind, core::IdKind::kEvidence);
}

// §3.1: the case `Identifier` is a display label carrying no semantic content.
// The falsification is an identity comparison, not an assertion about intent: a
// parser that folded the label into the case, or into any member, would give
// the two documents different content addresses.
TEST(EirParserTest, CaseLabelCarriesNoSemanticContent) {
  EirParseError error;
  StatusOr<EvidenceCase> labelled =
      ParseEirText(testing::kOverflowEirText, &error);
  ASSERT_TRUE(labelled.ok()) << error.message;
  StatusOr<EvidenceCase> unlabelled =
      ParseEirText(testing::kOverflowEirTextWithoutLabel, &error);
  ASSERT_TRUE(unlabelled.ok()) << error.message;

  ASSERT_TRUE(labelled->evidence_id.has_value());
  ASSERT_TRUE(unlabelled->evidence_id.has_value());
  EXPECT_EQ(*labelled->evidence_id, *unlabelled->evidence_id);
  EXPECT_EQ(*labelled, *unlabelled);
}

// The label is optional and may be any well-formed identifier; a case must not
// be refused for carrying one, and none of the three declarations may read it.
TEST(EirParserTest, AcceptsAnyWellFormedLabel) {
  const std::string source = MinimalCase();
  const std::string labelled =
      "evidence Some_Label_9 {\n" + source.substr(std::string("evidence {\n").size());
  EirParseError error;
  StatusOr<EvidenceCase> value = ParseEirText(labelled, &error);
  ASSERT_TRUE(value.ok()) << error.message;
  EXPECT_EQ(value->primary_claim.id, "C1");
}

TEST(EirParserTest, RejectsMalformedLabel) {  // a label that is not an identifier
  const Rejection rejected = Reject("evidence 7 {\n    schema = \"eir.v1\";\n}\n");
  EXPECT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("expected '{', found the integer"),
            std::string::npos)
      << rejected.error.message;
}

// --- Positive: the amended context block ------------------------------------

TEST(EirParserTest, ParsesAmendedContextProperties) {
  StatusOr<EvidenceCase> value =
      Accept(testing::kOverflowEirText, "the overflow fixture");
  ASSERT_TRUE(value.ok());

  EXPECT_EQ(value->program.repository_id, "radio-stack");
  EXPECT_EQ(value->program.revision_id, "a87f03e");
  EXPECT_EQ(value->program.build_variant_id, "ARM64_RELEASE");
  EXPECT_EQ(value->program.target_triple, "aarch64-unknown-linux-gnu");
  EXPECT_EQ(value->program.analysis_configuration_id, "veritas.default");
  // Task 7a made `type_layout` and `analysis_run` writable; neither may be
  // dropped on the way into the model.
  EXPECT_EQ(value->program.type_layout_id, "layout:aapcs64");
  ASSERT_TRUE(value->program.analysis_run_id.has_value());
  EXPECT_EQ(value->program.analysis_run_id->kind, core::IdKind::kAnalysisRun);
  EXPECT_EQ(core::ToString(*value->program.analysis_run_id),
            std::string(testing::kOverflowRunId));
}

// `analyzer` is the one repeatable context property, and `AnalyzerVersion`
// carries up to two optional string arguments. Both must survive.
TEST(EirParserTest, ParsesRepeatableAnalyzerVersions) {
  StatusOr<EvidenceCase> value =
      Accept(testing::kOverflowEirText, "the overflow fixture");
  ASSERT_TRUE(value.ok());

  ASSERT_EQ(value->program.analyzer_versions.size(), 2U);
  EXPECT_EQ(value->program.analyzer_versions[0].producer, "veritas.clang");
  EXPECT_EQ(value->program.analyzer_versions[0].version, "17.0.6");
  EXPECT_EQ(value->program.analyzer_versions[0].configuration,
            "veritas.default");
  EXPECT_EQ(value->program.analyzer_versions[1].producer, "veritas.wpa");
  EXPECT_EQ(value->program.analyzer_versions[1].version, "1.0");
  EXPECT_TRUE(value->program.analyzer_versions[1].configuration.empty());
}

TEST(EirParserTest, ParsesAnalyzerVersionWithNoArguments) {
  const std::string source = MinimalCase();
  const std::string with_analyzer = source.substr(
      0, source.find("    }\n\n")) + "        analyzer = veritas.bare();\n" +
      source.substr(source.find("    }\n\n"));
  StatusOr<EvidenceCase> value = Accept(with_analyzer, "a bare analyzer");
  ASSERT_TRUE(value.ok());
  ASSERT_EQ(value->program.analyzer_versions.size(), 1U);
  EXPECT_EQ(value->program.analyzer_versions[0].producer, "veritas.bare");
  EXPECT_TRUE(value->program.analyzer_versions[0].version.empty());
  EXPECT_TRUE(value->program.analyzer_versions[0].configuration.empty());
}

// --- Positive: the amended member attributes --------------------------------

TEST(EirParserTest, ParsesAmendedEntityAttributes) {
  StatusOr<EvidenceCase> value =
      Accept(testing::kOverflowEirText, "the overflow fixture");
  ASSERT_TRUE(value.ok());

  const Entity* length = nullptr;
  for (const Entity& entity : value->entities) {
    if (entity.id == "E_len") {
      length = &entity;
    }
  }
  ASSERT_NE(length, nullptr);
  ASSERT_TRUE(length->stable_id.has_value());
  EXPECT_EQ(length->stable_id->kind, core::IdKind::kValueRef);
  EXPECT_EQ(length->properties.size(), 1U);
  EXPECT_EQ(length->properties.count("origin"), 1U);
  EXPECT_EQ(length->properties.at("origin").kind, Expression::Kind::kReference);
  EXPECT_EQ(length->properties.at("origin").text, "E_packet_length");
}

// An entity that declares no identity has none: the positional `stable_id` is
// optional, and its absence must not be filled in with an empty one.
TEST(EirParserTest, ParsesEntityWithoutStableId) {
  StatusOr<EvidenceCase> value =
      Accept(testing::kOverflowEirText, "the overflow fixture");
  ASSERT_TRUE(value.ok());
  for (const Entity& entity : value->entities) {
    if (entity.id == "E_sink") {
      EXPECT_FALSE(entity.stable_id.has_value());
    }
  }
}

// §4.1: "the leading attribute binds the identity, and every `stable_id` after
// it is an ordinary entry of the open property bag". The identity occupies a
// position, not a bag key, so a second `stable_id` is legal and lands in the
// bag — the same document carries both, and the model holds both.
TEST(EirParserTest, ParsesAnEntityWithBothAnIdentityAndAStableIdProperty) {
  StatusOr<EvidenceCase> value = Accept(
      Case(std::string(kBaseEntities) +
           "    entity E_ident : value {\n"
           "        stable_id = "
           "\"valref:sha256:1111111111111111111111111111111111111111111111111111"
           "111111111111\";\n"
           "        stable_id = \"the bag entry, which is not the identity\";\n"
           "    }\n" +
           std::string(kPrimaryClaim)),
      "an entity carrying an identity and a same-named property");
  ASSERT_TRUE(value.ok());

  const Entity* named = nullptr;
  for (const Entity& entity : value->entities) {
    if (entity.id == "E_ident") {
      named = &entity;
    }
  }
  ASSERT_NE(named, nullptr);
  // The leading attribute bound the identity.
  ASSERT_TRUE(named->stable_id.has_value());
  EXPECT_EQ(named->stable_id->kind, core::IdKind::kValueRef);
  // The second is an ordinary bag entry, with its own value and its own type.
  ASSERT_EQ(named->properties.count("stable_id"), 1U);
  EXPECT_EQ(named->properties.at("stable_id").kind,
            Expression::Kind::kString);
  EXPECT_EQ(named->properties.at("stable_id").text,
            "the bag entry, which is not the identity");
}

// The bag is a map, so it holds that name once. A third `stable_id` is a
// repeated bag key rather than a second identity, and is refused as one.
TEST(EirParserTest, RejectsAThirdStableIdOnAnEntity) {
  const Rejection rejected = Reject(Case(
      std::string(kBaseEntities) +
      "    entity E_ident : value {\n"
      "        stable_id = "
      "\"valref:sha256:1111111111111111111111111111111111111111111111111111"
      "111111111111\";\n"
      "        stable_id = \"first bag entry\";\n"
      "        stable_id = \"second bag entry\";\n"
      "    }\n" +
      std::string(kPrimaryClaim)));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("the entity attribute 'stable_id' "
                                        "appears more than once"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, ParsesAmendedFactAttributes) {
  StatusOr<EvidenceCase> value =
      Accept(testing::kOverflowEirText, "the overflow fixture");
  ASSERT_TRUE(value.ok());

  const Fact* range = nullptr;
  const Fact* missing = nullptr;
  for (const Fact& fact : value->facts) {
    if (fact.id == "F_range") {
      range = &fact;
    }
    if (fact.id == "F_missing_check") {
      missing = &fact;
    }
  }
  ASSERT_NE(range, nullptr);
  ASSERT_NE(missing, nullptr);
  ASSERT_TRUE(range->stable_id.has_value());
  EXPECT_EQ(range->stable_id->kind, core::IdKind::kFact);
  EXPECT_FALSE(range->derived);
  EXPECT_EQ(range->producer, "clang");
  EXPECT_EQ(range->epistemic, EpistemicState::kMust);
  EXPECT_EQ(range->confidence, Confidence::kExact);
  // `derived = true` survives, and the provenance it names lowers to the bare
  // handle.
  EXPECT_TRUE(missing->derived);
  EXPECT_EQ(missing->provenance_id, "PR_check");
}

TEST(EirParserTest, ParsesAmendedUnknownAttributes) {
  StatusOr<EvidenceCase> value =
      Accept(testing::kOverflowEirText, "the overflow fixture");
  ASSERT_TRUE(value.ok());

  ASSERT_EQ(value->unknowns.size(), 1U);
  const Unknown& unknown = value->unknowns[0];
  EXPECT_EQ(unknown.reason_code, UnknownReasonCode::kExternalFunction);
  // Both halves of the classification are kept: the closed code and the
  // observed sentence the analysis recorded.
  EXPECT_EQ(unknown.reason, "vendor_validate has no body in this build variant");
  // `blocking` is a `ReferenceList`; the `@` belongs to the syntax.
  ASSERT_EQ(unknown.blocking_ids.size(), 1U);
  EXPECT_EQ(unknown.blocking_ids[0], "F_missing_check");
  // `suggested_resolution` is a `ResolutionAction`, a `FunctionCall`, and its
  // model carrier is a plain string holding the canonical EIR-T spelling.
  EXPECT_EQ(unknown.suggested_resolution, "infer_contract(E_vendor_validate)");
}

TEST(EirParserTest, ParsesAmendedProvenanceAttributes) {
  StatusOr<EvidenceCase> value =
      Accept(testing::kOverflowEirText, "the overflow fixture");
  ASSERT_TRUE(value.ok());

  const Provenance* record = nullptr;
  for (const Provenance& candidate : value->provenance) {
    if (candidate.id == "PR_check") {
      record = &candidate;
    }
  }
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->producer, "veritas.wpa");
  EXPECT_EQ(record->rule, "dominates.absence");
  // `inputs` is a `FactReferenceList`; the `$` belongs to the syntax too.
  ASSERT_EQ(record->input_fact_ids.size(), 2U);
  EXPECT_EQ(record->input_fact_ids[0], "F_range");
  EXPECT_EQ(record->input_fact_ids[1], "F_capacity");
  EXPECT_EQ(record->source_anchor_id, "decode.cpp:281:9");
  EXPECT_EQ(record->version, "1.0");
  EXPECT_EQ(record->configuration, "veritas.default");
  // The run is declared, never derived from the case binding.
  ASSERT_TRUE(record->analysis_run_id.has_value());
  EXPECT_EQ(*record->analysis_run_id, *value->program.analysis_run_id);
}

TEST(EirParserTest, ParsesAmendedVerificationAttributes) {
  StatusOr<EvidenceCase> value =
      Accept(testing::kOverflowEirText, "the overflow fixture");
  ASSERT_TRUE(value.ok());

  ASSERT_EQ(value->proof_obligations.size(), 1U);
  const ProofObligation& obligation = value->proof_obligations[0];
  EXPECT_EQ(obligation.id, "V1");
  EXPECT_EQ(obligation.goal_kind, ProofGoalKind::kProve);
  ASSERT_EQ(obligation.verifier_kinds.size(), 2U);
  EXPECT_EQ(obligation.verifier_kinds[0], "smt");
  EXPECT_EQ(obligation.verifier_kinds[1], "symbolic");
  EXPECT_EQ(obligation.status, ProofStatus::kProved);
  EXPECT_EQ(obligation.result_id, "PR_result");
  EXPECT_EQ(obligation.verification_producer, "veritas.smt");
  ASSERT_EQ(obligation.budget.kind, Expression::Kind::kInteger);
  EXPECT_EQ(obligation.budget.integer, 5000);
}

TEST(EirParserTest, ParsesAmendedSummaryAndDependencyAttributes) {
  StatusOr<EvidenceCase> value =
      Accept(testing::kOverflowEirText, "the overflow fixture");
  ASSERT_TRUE(value.ok());

  ASSERT_EQ(value->summaries.size(), 1U);
  EXPECT_EQ(value->summaries[0].function_id, "E_vendor_validate");
  EXPECT_EQ(value->summaries[0].summary_id.kind, core::IdKind::kFunctionSummary);
  ASSERT_EQ(value->summaries[0].components.size(), 2U);
  EXPECT_EQ(value->summaries[0].components[0], "range");
  EXPECT_EQ(value->summaries[0].components[1], "value_flow");

  ASSERT_EQ(value->dependencies.size(), 2U);
  EXPECT_EQ(value->dependencies[0].kind, DependencyKind::kSummary);
  EXPECT_EQ(value->dependencies[1].kind, DependencyKind::kConfiguration);
  EXPECT_EQ(value->dependencies[1].stable_id.kind, core::IdKind::kModel);
}

TEST(EirParserTest, ParsesThePathExpressionInSourceOrder) {
  StatusOr<EvidenceCase> value =
      Accept(testing::kOverflowEirText, "the overflow fixture");
  ASSERT_TRUE(value.ok());

  ASSERT_EQ(value->paths.size(), 1U);
  const Path& path = value->paths[0];
  EXPECT_EQ(path.kind, PathKind::kValueFlow);
  ASSERT_EQ(path.entity_ids.size(), 2U);
  EXPECT_EQ(path.entity_ids[0], "E_len");
  EXPECT_EQ(path.entity_ids[1], "E_sink");
  ASSERT_EQ(path.conditions.size(), 1U);
  EXPECT_EQ(path.conditions[0].kind, Expression::Kind::kCompare);
  EXPECT_EQ(path.feasibility, Feasibility::kSat);
  EXPECT_EQ(path.provenance_id, "PR_check");
}

// `AssumptionSource` and `Scope` have plain-string model carriers, so both
// spellings that reach them must lower to the same canonical form the writer
// will reproduce.
TEST(EirParserTest, LowersCallShapedCarriersToCanonicalEirText) {
  StatusOr<EvidenceCase> value =
      Accept(testing::kOverflowEirText, "the overflow fixture");
  ASSERT_TRUE(value.ok());

  ASSERT_EQ(value->assumptions.size(), 1U);
  EXPECT_EQ(value->assumptions[0].source, "infer_contract(E_vendor_validate)");
  EXPECT_EQ(value->assumptions[0].scope, "decode_frame(E_sink)");
  ASSERT_EQ(value->constraints.size(), 1U);
  EXPECT_EQ(value->constraints[0].scope, "global");
  EXPECT_EQ(value->constraints[0].provenance_id, "PR_check");
}

// The rendered carrier is a *fixpoint* of the lowering, not merely a string the
// `@`-argument spelling happens to produce: reading the rendered text back
// yields a carrier that renders to the same text. The test above covers the
// `@`-argument direction; this one feeds the output back in as source, which is
// the half a writer's round trip actually depends on. The multi-argument form
// pins the `", "` separator and the quoted string argument too.
TEST(EirParserTest, CanonicalCallSpellingIsAFixpoint) {
  constexpr std::string_view kSingle = "infer_contract(E_vendor_validate)";
  StatusOr<EvidenceCase> single = Accept(CasePlus(
      "    assumption A1 {\n"
      "        predicate = reachable(@E_sink);\n"
      "        source = " + std::string(kSingle) + ";\n"
      "    }\n"),
      "the rendered single-argument spelling, fed back in");
  ASSERT_TRUE(single.ok());
  ASSERT_EQ(single->assumptions.size(), 1U);
  EXPECT_EQ(single->assumptions[0].source, kSingle);

  constexpr std::string_view kMany =
      "infer_contract(E_vendor_validate, \"packet\", 1)";
  StatusOr<EvidenceCase> many = Accept(CasePlus(
      "    assumption A1 {\n"
      "        predicate = reachable(@E_sink);\n"
      "        source = " + std::string(kMany) + ";\n"
      "    }\n"),
      "the rendered multi-argument spelling, fed back in");
  ASSERT_TRUE(many.ok());
  ASSERT_EQ(many->assumptions.size(), 1U);
  EXPECT_EQ(many->assumptions[0].source, kMany);
}

// --- Positive: precedence and associativity ---------------------------------

const Expression& SolePredicate(const StatusOr<EvidenceCase>& value) {
  return value->primary_claim.predicate;
}

TEST(EirParserTest, NotBindsTighterThanComparison) {
  // `not` is the tightest of the logical operators, so `not a == b` groups as
  // `(not a) == b`. That grouping puts a `not` formula in a value position,
  // which the validator refuses as ill-typed — so the grouping is observed
  // through the lowering step alone.
  EirParseError error;
  StatusOr<EvidenceCase> value =
      ParseUnvalidated(ClaimCase("not @E_sink == @E_len"));
  ASSERT_TRUE(value.ok()) << error.message;
  const Expression& predicate = value->primary_claim.predicate;
  ASSERT_EQ(predicate.kind, Expression::Kind::kCompare);
  EXPECT_EQ(predicate.text, "==");
  ASSERT_EQ(predicate.operands.size(), 2U);
  EXPECT_EQ(predicate.operands[0].kind, Expression::Kind::kNot);
  EXPECT_EQ(predicate.operands[1].kind, Expression::Kind::kReference);

  // The same precedence read from a case that is well-typed: `not` binds
  // tighter than `and`, so the conjunction is the root and the negation is one
  // of its operands.
  StatusOr<EvidenceCase> typed =
      Accept(ClaimCase("not reachable(@E_sink) and reachable(@E_len)"),
             "`not a and b`");
  ASSERT_TRUE(typed.ok());
  const Expression& conjunction = SolePredicate(typed);
  ASSERT_EQ(conjunction.kind, Expression::Kind::kAnd);
  ASSERT_EQ(conjunction.operands.size(), 2U);
  EXPECT_EQ(conjunction.operands[0].kind, Expression::Kind::kNot);
}

TEST(EirParserTest, AndBindsTighterThanOr) {
  StatusOr<EvidenceCase> value =
      Accept(ClaimCase("@E_sink and @E_len or @E_sink"), "`a and b or c`");
  ASSERT_TRUE(value.ok());
  const Expression& predicate = SolePredicate(value);
  ASSERT_EQ(predicate.kind, Expression::Kind::kOr);
  ASSERT_EQ(predicate.operands.size(), 2U);
  EXPECT_EQ(predicate.operands[0].kind, Expression::Kind::kAnd);
  EXPECT_EQ(predicate.operands[1].kind, Expression::Kind::kReference);
}

TEST(EirParserTest, ConjunctionsAreFlattenedIntoOneOperandList) {
  StatusOr<EvidenceCase> value =
      Accept(ClaimCase("@E_sink and @E_len and @E_sink"), "`a and b and c`");
  ASSERT_TRUE(value.ok());
  const Expression& predicate = SolePredicate(value);
  ASSERT_EQ(predicate.kind, Expression::Kind::kAnd);
  // Left-associative grouping would build `and(and(a, b), c)`. The model is
  // n-ary flat and the validator rejects a directly nested same-kind operand,
  // so the two spellings must lower to one node — and therefore to one
  // canonical encoding.
  EXPECT_EQ(predicate.operands.size(), 3U);
}

TEST(EirParserTest, ParenthesisedConjunctionIsSplicedNotNested) {
  StatusOr<EvidenceCase> value = Accept(
      ClaimCase("(@E_sink and @E_len) and @E_sink"), "`(a and b) and c`");
  ASSERT_TRUE(value.ok());
  const Expression& predicate = SolePredicate(value);
  ASSERT_EQ(predicate.kind, Expression::Kind::kAnd);
  EXPECT_EQ(predicate.operands.size(), 3U);
  for (const Expression& operand : predicate.operands) {
    EXPECT_NE(operand.kind, Expression::Kind::kAnd);
  }
}

TEST(EirParserTest, ImplicationIsRightAssociative) {
  StatusOr<EvidenceCase> value = Accept(
      ClaimCase("@E_sink implies @E_len implies @E_sink"), "`a implies b implies c`");
  ASSERT_TRUE(value.ok());
  const Expression& predicate = SolePredicate(value);
  ASSERT_EQ(predicate.kind, Expression::Kind::kImplies);
  ASSERT_EQ(predicate.operands.size(), 2U);
  EXPECT_EQ(predicate.operands[0].kind, Expression::Kind::kReference);
  EXPECT_EQ(predicate.operands[1].kind, Expression::Kind::kImplies);
}

TEST(EirParserTest, ImplicationBindsLoosestOfTheLogicalOperators) {
  StatusOr<EvidenceCase> value = Accept(
      ClaimCase("@E_sink and @E_len implies @E_sink"), "`a and b implies c`");
  ASSERT_TRUE(value.ok());
  const Expression& predicate = SolePredicate(value);
  ASSERT_EQ(predicate.kind, Expression::Kind::kImplies);
  ASSERT_EQ(predicate.operands.size(), 2U);
  EXPECT_EQ(predicate.operands[0].kind, Expression::Kind::kAnd);
}

TEST(EirParserTest, QuantifierOwnsEverythingAfterItsColon) {
  StatusOr<EvidenceCase> value = Accept(
      ClaimCase("forall x in entities(@E_sink): @E_sink and @E_len"),
      "`forall x in D: P and Q`");
  ASSERT_TRUE(value.ok());
  const Expression& predicate = SolePredicate(value);
  ASSERT_EQ(predicate.kind, Expression::Kind::kForAll);
  EXPECT_EQ(predicate.text, "x");
  ASSERT_EQ(predicate.operands.size(), 2U);
  EXPECT_EQ(predicate.operands[0].kind, Expression::Kind::kCall);
  EXPECT_EQ(predicate.operands[0].text, "entities");
  EXPECT_EQ(predicate.operands[1].kind, Expression::Kind::kAnd);
}

TEST(EirParserTest, QuantifierDomainMayBeAReference) {
  StatusOr<EvidenceCase> value = Accept(
      ClaimCase("exists v in @E_len: reachable(v)"), "`exists v in @E: P`");
  ASSERT_TRUE(value.ok());
  const Expression& predicate = SolePredicate(value);
  ASSERT_EQ(predicate.kind, Expression::Kind::kExists);
  ASSERT_EQ(predicate.operands.size(), 2U);
  EXPECT_EQ(predicate.operands[0].kind, Expression::Kind::kReference);
  EXPECT_EQ(predicate.operands[0].text, "E_len");
}

// A dotted qualified identifier is one token whose text keeps its dots, so it
// reaches the model whole. `veritas.clang` is the producer of an analyzer
// version and is asserted in `ParsesRepeatableAnalyzerVersions`; the omission
// kind is the other carrier.
TEST(EirParserTest, DottedQualifiedIdIsOneIdentifier) {
  StatusOr<EvidenceCase> value =
      Accept(testing::kOverflowEirText, "the overflow fixture");
  ASSERT_TRUE(value.ok());
  ASSERT_EQ(value->omissions.size(), 1U);
  EXPECT_EQ(value->omissions[0].kind, "analyzer_expansion");
  EXPECT_EQ(value->omissions[0].subject, "S1");
  EXPECT_TRUE(value->omissions[0].expandable);
}

// --- Positive: lexical and literal handling ---------------------------------

TEST(EirParserTest, CommentsAreIgnored) {
  const std::string source =
      "// a line comment at the top\n"
      "evidence /* and a block comment before the brace */ {\n"
      "    schema = \"eir.v1\"; // trailing\n"
      "    level = l1;\n"
      "    state = POSSIBLE_DEFECT;\n" +
      std::string(kContextBlock) + std::string(kBaseEntities) +
      std::string(kPrimaryClaim) +
      "}\n// a trailing line comment with no final newline";
  StatusOr<EvidenceCase> value = Accept(source, "a commented case");
  ASSERT_TRUE(value.ok());
  EXPECT_EQ(value->primary_claim.id, "C1");
}

TEST(EirParserTest, TheSignOfAnIntegerIsPartOfTheLiteral) {
  const std::string source = CasePlus(
                                  "    constraint K1 {\n"
                                  "        expr = @E_len > -1;\n"
                                  "        epistemic = must;\n"
                                  "    }\n"
                                  "    verify V1 {\n"
                                  "        prove = @E_len > -1;\n"
                                  "        budget = -5;\n"
                                  "        status = PENDING;\n"
                                  "    }\n");
  StatusOr<EvidenceCase> value = Accept(source, "a negative integer");
  ASSERT_TRUE(value.ok());
  ASSERT_EQ(value->proof_obligations.size(), 1U);
  ASSERT_EQ(value->proof_obligations[0].budget.kind,
            Expression::Kind::kInteger);
  EXPECT_EQ(value->proof_obligations[0].budget.integer, -5);
  ASSERT_EQ(value->constraints.size(), 1U);
  ASSERT_EQ(value->constraints[0].expression.operands.size(), 2U);
  EXPECT_EQ(value->constraints[0].expression.operands[1].kind,
            Expression::Kind::kInteger);
  EXPECT_EQ(value->constraints[0].expression.operands[1].integer, -1);
}

// The per-kind contract carries an integer in `integer` alone; `text` stays
// empty so a parsed case and an assembled one canonicalize to the same bytes.
TEST(EirParserTest, IntegerLiteralDoesNotAlsoCarryItsText) {
  StatusOr<EvidenceCase> value =
      Accept(testing::kOverflowEirText, "the overflow fixture");
  ASSERT_TRUE(value.ok());
  for (const Fact& fact : value->facts) {
    if (fact.id != "F_range") {
      continue;
    }
    ASSERT_EQ(fact.predicate.kind, Expression::Kind::kCall);
    ASSERT_EQ(fact.predicate.operands.size(), 3U);
    ASSERT_EQ(fact.predicate.operands[2].kind, Expression::Kind::kInteger);
    EXPECT_EQ(fact.predicate.operands[2].integer, 65535);
    EXPECT_TRUE(fact.predicate.operands[2].text.empty());
  }
}

// §5.1's `AtomicPredicate` has no bare-identifier alternative, yet the model's
// `kSymbol` exists for exactly that value and §15's own example uses one
// (`@packet.type == EXTENSION`). The widening is asserted rather than assumed,
// because a writer that emitted a symbol could not otherwise read it back.
TEST(EirParserTest, BareIdentifierLowersToSymbol) {
  StatusOr<EvidenceCase> value =
      Accept(ClaimCase("@E_sink == EXTENSION"), "`@e == EXTENSION`");
  ASSERT_TRUE(value.ok());
  const Expression& predicate = SolePredicate(value);
  ASSERT_EQ(predicate.kind, Expression::Kind::kCompare);
  ASSERT_EQ(predicate.operands.size(), 2U);
  EXPECT_EQ(predicate.operands[1].kind, Expression::Kind::kSymbol);
  EXPECT_EQ(predicate.operands[1].text, "EXTENSION");
}

TEST(EirParserTest, BareIdentifierLowersToSymbolInAPropertyBag) {
  const std::string source = CasePlus(
                                  "    entity E_kind : value {\n"
                                  "        domain = EXTENSION;\n"
                                  "    }\n");
  StatusOr<EvidenceCase> value = Accept(source, "a symbol property");
  ASSERT_TRUE(value.ok());
  for (const Entity& entity : value->entities) {
    if (entity.id != "E_kind") {
      continue;
    }
    ASSERT_EQ(entity.properties.count("domain"), 1U);
    EXPECT_EQ(entity.properties.at("domain").kind, Expression::Kind::kSymbol);
    EXPECT_EQ(entity.properties.at("domain").text, "EXTENSION");
  }
}

TEST(EirParserTest, BooleanLiteralsLowerToBool) {
  const std::string source = CasePlus(
                                  "    fact F_observed {\n"
                                  "        predicate = reachable(@E_sink);\n"
                                  "        epistemic = must;\n"
                                  "        confidence = exact;\n"
                                  "        derived = false;\n"
                                  "    }\n"
                                  "    fact F_derived {\n"
                                  "        predicate = reachable(@E_len);\n"
                                  "        epistemic = inferred;\n"
                                  "        confidence = exact;\n"
                                  "        derived = true;\n"
                                  "        provenance = @PR1;\n"
                                  "    }\n"
                                  "    provenance PR1 {\n"
                                  "        producer = veritas.wpa;\n"
                                  "        analysis_run = \"run:sha256:0d5c1e2f3a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5\";\n"
                                  "    }\n");
  StatusOr<EvidenceCase> value = Accept(source, "boolean flags");
  ASSERT_TRUE(value.ok());
  ASSERT_EQ(value->facts.size(), 2U);
  EXPECT_FALSE(value->facts[0].derived);
  EXPECT_TRUE(value->facts[1].derived);
}

TEST(EirParserTest, StringEscapesAreDecoded) {
  const std::string source = CasePlus(
                                  "    entity E_text : value {\n"
                                  "        quote = \"a \\\" b \\\\ c \\n\";\n"
                                  "    }\n");
  StatusOr<EvidenceCase> value = Accept(source, "an escaped string");
  ASSERT_TRUE(value.ok());
  for (const Entity& entity : value->entities) {
    if (entity.id != "E_text") {
      continue;
    }
    ASSERT_EQ(entity.properties.count("quote"), 1U);
    EXPECT_EQ(entity.properties.at("quote").text, "a \" b \\ c \n");
  }
}

// `kBool` likewise carries only `boolean`.
TEST(EirParserTest, BoolLiteralDoesNotAlsoCarryItsText) {
  const std::string source = CasePlus(
                                  "    entity E_flag : value {\n"
                                  "        flag = true;\n"
                                  "    }\n");
  StatusOr<EvidenceCase> value = Accept(source, "a boolean property");
  ASSERT_TRUE(value.ok());
  for (const Entity& entity : value->entities) {
    if (entity.id != "E_flag") {
      continue;
    }
    ASSERT_EQ(entity.properties.count("flag"), 1U);
    EXPECT_EQ(entity.properties.at("flag").kind, Expression::Kind::kBool);
    EXPECT_TRUE(entity.properties.at("flag").boolean);
    EXPECT_TRUE(entity.properties.at("flag").text.empty());
  }
}

// --- Positive: failure coordinates ------------------------------------------

TEST(EirParserTest, ReportsOneBasedLineAndByteColumn) {
  const std::string source = "evidence {\n    level = 7;\n";
  const Rejection rejected = Reject(source);
  ASSERT_TRUE(rejected.rejected);
  EXPECT_EQ(rejected.error.offset, source.find("level"));
  EXPECT_EQ(rejected.error.line, 2U);
  EXPECT_EQ(rejected.error.column, 5U);
}

// A well-formedness rejection is about the case as a whole and names its own
// member, so it reports the start of the document rather than a token.
TEST(EirParserTest, WellFormednessRejectionReportsTheStartOfTheDocument) {
  const std::string source =
      std::string("evidence {\n    schema = \"eir.v1\";\n    level = l1;\n"
                  "    state = POSSIBLE_DEFECT;\n\n") +
      std::string(kContextBlock) +
      "    entity E_sink : callsite {\n        function = \"memcpy\";\n    }\n"
      "}\n";
  const Rejection rejected = Reject(source);
  ASSERT_TRUE(rejected.rejected);
  EXPECT_EQ(rejected.error.offset, 0U);
  EXPECT_EQ(rejected.error.line, 1U);
  EXPECT_EQ(rejected.error.column, 1U);
  EXPECT_NE(rejected.error.message.find("primary_claim_count"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, AnEmptySourceIsRejected) {
  const Rejection rejected = Reject("");
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("end of input"), std::string::npos)
      << rejected.error.message;
}

// --- Reject sites: case structure -------------------------------------------

TEST(EirParserTest, RejectsMissingHeaderBrace) {
  const Rejection rejected = Reject("evidence\n");
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("'{'"), std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsRepeatedSchemaDeclaration) {
  const Rejection rejected = Reject(
      "evidence {\n"
      "    schema = \"eir.v1\";\n"
      "    schema = \"eir.v1\";\n"
      "    level = l1;\n"
      "    state = POSSIBLE_DEFECT;\n}\n");
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message
                .find("expected the 'level' declaration after the schema"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsRepeatedLevelDeclaration) {
  const Rejection rejected = Reject(
      "evidence {\n"
      "    schema = \"eir.v1\";\n"
      "    level = l1;\n"
      "    level = l2;\n"
      "    state = POSSIBLE_DEFECT;\n}\n");
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message
                .find("expected the 'state' declaration after the level"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsRepeatedStateDeclaration) {
  const Rejection rejected = Reject(
      "evidence {\n"
      "    schema = \"eir.v1\";\n"
      "    level = l1;\n"
      "    state = POSSIBLE_DEFECT;\n"
      "    state = INCONCLUSIVE;\n}\n");
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message
                .find("expected the 'context' declaration after the state"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsMemberBeforeContext) {
  const Rejection rejected = Reject(
      "evidence {\n"
      "    schema = \"eir.v1\";\n"
      "    level = l1;\n"
      "    state = POSSIBLE_DEFECT;\n" +
      std::string(kBaseEntities) + std::string(kPrimaryClaim) + "}\n");
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message
                .find("expected the 'context' declaration after the state"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsSecondPrimaryClaim) {
  const Rejection rejected = Reject(CaseWith(
      "    claim C2 {\n"
      "        kind = buffer_overflow;\n"
      "        subject = @E_sink;\n"
      "        predicate = reachable(@E_len);\n"
      "        severity = low;\n"
      "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("second primary claim"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsNonIdentifierWhereAMemberIsExpected) {
  const Rejection rejected = Reject(Case(std::string(kBaseEntities) +
                                         "    7\n" +
                                         std::string(kPrimaryClaim)));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("expected an evidence member, found "
                                        "the integer literal"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsUnknownEvidenceMember) {
  const Rejection rejected = Reject(CaseWith("    bogus\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("unknown evidence member 'bogus'"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsUnterminatedCase) {
  std::string source = MinimalCase();
  source.pop_back();  // the final newline
  source.pop_back();  // the closing brace
  const Rejection rejected = Reject(source);
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("unterminated evidence case"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsTrailingTokensAfterTheCase) {
  const Rejection rejected =
      Reject(MinimalCase() + "evidence\n");
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("expected the end of the document"),
            std::string::npos)
      << rejected.error.message;
}

// --- Reject sites: the attribute bodies -------------------------------------

TEST(EirParserTest, RejectsUnknownAttributeInAMemberBody) {
  const Rejection rejected = Reject(CaseWith(
      "    claim C2 {\n"
      "        kind = buffer_overflow;\n"
      "        subject = @E_sink;\n"
      "        predicate = reachable(@E_len);\n"
      "        severity = low;\n"
      "        colour = \"red\";\n"
      "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("unknown claim attribute 'colour'"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsRepeatedAttributeInAMemberBody) {
  const Rejection rejected = Reject(CaseWith(
      "    claim C2 {\n"
      "        kind = buffer_overflow;\n"
      "        kind = null_dereference;\n"
      "        subject = @E_sink;\n"
      "        predicate = reachable(@E_len);\n"
      "        severity = low;\n"
      "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("appears more than once"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsMissingRequiredAttributeInAMemberBody) {
  const Rejection rejected = Reject(CaseWith(
      "    claim C2 {\n"
      "        kind = buffer_overflow;\n"
      "        subject = @E_sink;\n"
      "        predicate = reachable(@E_len);\n"
      "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("omits its required attribute "
                                        "'severity'"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsUnterminatedMemberBody) {
  const Rejection rejected = Reject(
      std::string("evidence {\n    schema = \"eir.v1\";\n    level = l1;\n"
                  "    state = POSSIBLE_DEFECT;\n\n") +
      std::string(kContextBlock) +
      "    claim C1 {\n        kind = buffer_overflow;\n");
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("unterminated claim declaration"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsAttributeWithoutEqual) {
  const Rejection rejected = Reject(CaseWith(
      "    claim C2 {\n"
      "        kind buffer_overflow;\n"
      "        subject = @E_sink;\n"
      "        predicate = reachable(@E_len);\n"
      "        severity = low;\n"
      "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("'=' after the attribute 'kind'"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsAttributeWithoutSemicolon) {
  const Rejection rejected = Reject(CaseWith(
      "    claim C2 {\n"
      "        kind = buffer_overflow;\n"
      "        subject = @E_sink;\n"
      "        predicate = reachable(@E_len);\n"
      "        severity = low\n"
      "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("';' after the value of 'severity'"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsMissingMemberBodyBrace) {
  const Rejection rejected = Reject(CaseWith(
      "    claim C2\n"
      "        kind = buffer_overflow;\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("'{' after the claim identifier"),
            std::string::npos)
      << rejected.error.message;
}

// --- Reject sites: the unsupported-in-V0.1 family ---------------------------

// §11.1: `ProvenanceDecl`'s `location` is grammar-valid and
// model-unrepresentable. The parser must refuse it, and must never lower the
// value onto `source_anchor_id` and never drop it: either substitution would
// change `EvidenceID` for such an input and break REP-001.
TEST(EirParserTest, RejectsUnsupportedProvenanceLocation) {
  const Rejection rejected = Reject(CaseWith(
      "    provenance PR1 {\n"
      "        producer = veritas.wpa;\n"
      "        location = src(\"decode.cpp\", 281, 9);\n"
      "        analysis_run = \"run:sha256:0d5c1e2f3a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5\";\n"
      "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("unsupported in EIR V0.1"),
            std::string::npos)
      << rejected.error.message;
  EXPECT_NE(rejected.error.message.find("'location'"), std::string::npos)
      << rejected.error.message;
}

// The refusal must name the attribute that was refused, so a reader can tell
// `location` apart from `source_anchor` — the two are distinct, and only the
// latter exists in the model.
TEST(EirParserTest, ProvenanceSourceAnchorIsNotTheUnsupportedAttribute) {
  const std::string source = CasePlus(
                                  "    provenance PR1 {\n"
                                  "        producer = veritas.wpa;\n"
                                  "        source_anchor = \"decode.cpp:281:9\";\n"
                                  "        analysis_run = \"run:sha256:0d5c1e2f3a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5\";\n"
                                  "    }\n");
  StatusOr<EvidenceCase> value = Accept(source, "a source anchor");
  ASSERT_TRUE(value.ok());
  ASSERT_EQ(value->provenance.size(), 1U);
  EXPECT_EQ(value->provenance[0].source_anchor_id, "decode.cpp:281:9");
}

TEST(EirParserTest, RejectsUnsupportedEdgeCondition) {
  const Rejection rejected = Reject(CaseWith(
      "    edge ED1 {\n"
      "        from = @E_len;\n"
      "        to = @E_sink;\n"
      "        kind = FLOWS_TO;\n"
      "        condition = @E_len > 0;\n"
      "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("unsupported in EIR V0.1"),
            std::string::npos)
      << rejected.error.message;
  EXPECT_NE(rejected.error.message.find("'condition'"), std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsUnsupportedEdgeTransfer) {
  const Rejection rejected = Reject(CaseWith(
      "    edge ED1 {\n"
      "        from = @E_len;\n"
      "        to = @E_sink;\n"
      "        kind = FLOWS_TO;\n"
      "        transfer = \"identity\";\n"
      "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("unsupported in EIR V0.1"),
            std::string::npos)
      << rejected.error.message;
  EXPECT_NE(rejected.error.message.find("'transfer'"), std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsUnsupportedEdgeSummary) {
  const Rejection rejected = Reject(CaseWith(
      "    edge ED1 {\n"
      "        from = @E_len;\n"
      "        to = @E_sink;\n"
      "        kind = FLOWS_TO;\n"
      "        summary = infer_contract(@E_sink);\n"
      "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("unsupported in EIR V0.1"),
            std::string::npos)
      << rejected.error.message;
  EXPECT_NE(rejected.error.message.find("'summary'"), std::string::npos)
      << rejected.error.message;
}

// The two refusal families are distinct: an attribute the grammar admits and
// the model cannot carry is not the same diagnostic as an attribute the
// production does not list.
TEST(EirParserTest, UnsupportedAndUnknownAttributesAreDistinctDiagnostics) {
  const Rejection unsupported = Reject(CaseWith(
      "    edge ED1 {\n"
      "        from = @E_len;\n"
      "        to = @E_sink;\n"
      "        kind = FLOWS_TO;\n"
      "        condition = @E_len > 0;\n"
      "    }\n"));
  const Rejection unknown = Reject(CaseWith(
      "    edge ED1 {\n"
      "        from = @E_len;\n"
      "        to = @E_sink;\n"
      "        kind = FLOWS_TO;\n"
      "        conditon = @E_len > 0;\n"
      "    }\n"));
  ASSERT_TRUE(unsupported.rejected);
  ASSERT_TRUE(unknown.rejected);
  EXPECT_NE(unsupported.error.message.find("unsupported in EIR V0.1"),
            std::string::npos);
  EXPECT_EQ(unknown.error.message.find("unsupported in EIR V0.1"),
            std::string::npos)
      << unknown.error.message;
  EXPECT_NE(unknown.error.message.find("unknown edge attribute 'conditon'"),
            std::string::npos)
      << unknown.error.message;
}

// --- Reject sites: the context block ----------------------------------------

TEST(EirParserTest, RejectsUnknownContextProperty) {
  const Rejection rejected = Reject(
      "evidence {\n    schema = \"eir.v1\";\n    level = l1;\n"
      "    state = POSSIBLE_DEFECT;\n\n"
      "    context {\n        repository = \"radio-stack\";\n"
      "        bogus = \"x\";\n    }\n}\n");
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("unknown context property 'bogus'"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsRepeatedContextProperty) {
  const Rejection rejected = Reject(
      "evidence {\n    schema = \"eir.v1\";\n    level = l1;\n"
      "    state = POSSIBLE_DEFECT;\n\n"
      "    context {\n        repository = \"radio-stack\";\n"
      "        repository = \"radio-stack\";\n    }\n}\n");
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("'repository' appears more than once"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsUnterminatedContextDeclaration) {
  const Rejection rejected = Reject(
      "evidence {\n    schema = \"eir.v1\";\n    level = l1;\n"
      "    state = POSSIBLE_DEFECT;\n\n"
      "    context {\n        repository = \"radio-stack\";\n");
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("unterminated context declaration"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsNonCanonicalAnalysisRun) {
  const Rejection rejected = Reject(
      "evidence {\n    schema = \"eir.v1\";\n    level = l1;\n"
      "    state = POSSIBLE_DEFECT;\n\n"
      "    context {\n        analysis_run = \"not-a-stable-id\";\n    }\n}\n");
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("not a stable ID of the form"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsMalformedAnalyzerVersion) {
  const Rejection rejected = Reject(
      "evidence {\n    schema = \"eir.v1\";\n    level = l1;\n"
      "    state = POSSIBLE_DEFECT;\n\n"
      "    context {\n        analyzer = veritas.clang(\"1.0\";\n    }\n}\n");
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("')' closing the analyzer version"),
            std::string::npos)
      << rejected.error.message;
}

// --- Reject sites: textual enums --------------------------------------------

TEST(EirParserTest, RejectsEnumTerminalOutsideTheV01Subset) {
  // `LevelDecl` is closed to `l0`, `l1`, `l2`; the grammar's other spellings
  // are the full EIR-T 1.0 surface and are not part of the model's subset.
  const Rejection rejected = Reject(
      "evidence {\n    schema = \"eir.v1\";\n    level = l9;\n"
      "    state = POSSIBLE_DEFECT;\n}\n");
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("expected evidence level, found "
                                        "\"l9\", which is not a valid evidence "
                                        "level"),
            std::string::npos)
      << rejected.error.message;
}

// Every enum family goes through one `TakeEnum`, and each call names its own
// family; the name is the part of the diagnostic a reader matches on, so it is
// asserted for a second family rather than assumed from the first.
TEST(EirParserTest, RejectsConfidenceSpellingOutsideTheFamily) {
  const Rejection rejected = Reject(CaseWith(
      "    fact F_bogus {\n"
      "        predicate = reachable(@E_len);\n"
      "        epistemic = must;\n"
      "        confidence = unspecified;\n"
      "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("expected confidence, found "
                                        "\"unspecified\", which is not a valid "
                                        "confidence"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsNonIdentifierWhereAnEnumTerminalIsExpected) {
  const Rejection rejected = Reject(
      "evidence {\n    schema = \"eir.v1\";\n    level = l1;\n"
      "    state = \"POSSIBLE_DEFECT\";\n}\n");
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("expected verification state, found a "
                                        "string literal"),
            std::string::npos)
      << rejected.error.message;
}

// --- Reject sites: predicates -----------------------------------------------

TEST(EirParserTest, RejectsChainedComparison) {
  const Rejection rejected = Reject(PredicateOnlyCase("@E_len == @E_len == @E_len"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("a comparison is not associative"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsMissingPredicate) {
  const Rejection rejected = Reject(PredicateOnlyCase(""));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("expected a predicate, found the token "
                                        "';'"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsUnclosedParenthesisInAPredicate) {
  const Rejection rejected = Reject(PredicateOnlyCase("(@E_len"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("')' closing a parenthesised "
                                        "predicate"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsQuantifierWithoutIn) {
  const Rejection rejected =
      Reject(PredicateOnlyCase("forall x @E_len: reachable(x)"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("expected the keyword 'in'"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsQuantifierWithoutColon) {
  const Rejection rejected =
      Reject(PredicateOnlyCase("forall x in entities(@E_len) reachable(x)"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("':' after the quantifier domain"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsPredicateCallWithoutClosingParenthesis) {
  const Rejection rejected = Reject(PredicateOnlyCase("reachable(@E_len"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("')' closing a predicate call"),
            std::string::npos)
      << rejected.error.message;
}

// --- Reject sites: references, lists, and literals --------------------------

TEST(EirParserTest, RejectsReferenceWithoutSigil) {
  const Rejection rejected = Reject(CaseWith(
      "    claim C2 {\n"
      "        kind = buffer_overflow;\n"
      "        subject = E_sink;\n"
      "        predicate = reachable(@E_len);\n"
      "        severity = low;\n"
      "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("introduced by '@'"), std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsReferenceWithoutIdentifier) {
  const Rejection rejected = Reject(CaseWith(
      "    claim C2 {\n"
      "        kind = buffer_overflow;\n"
      "        subject = @;\n"
      "        predicate = reachable(@E_len);\n"
      "        severity = low;\n"
      "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("after '@'"), std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsFactReferenceWithoutSigil) {
  const Rejection rejected = Reject(CaseWith(
      "    provenance PR1 {\n"
      "        producer = veritas.wpa;\n"
      "        inputs = [F_range];\n"
      "        analysis_run = \"run:sha256:0d5c1e2f3a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5\";\n"
      "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("'$' introducing a fact reference"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsListWithoutOpeningBracket) {
  const Rejection rejected = Reject(Case(std::string(kBaseEntities) +
                                         "    verify V1 {\n"
                                         "        prove = reachable(@E_len);\n"
                                         "        using = smt;\n"
                                         "        status = PENDING;\n"
                                         "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("'[' opening the list"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsListWithoutClosingBracket) {
  const Rejection rejected = Reject(Case(std::string(kBaseEntities) +
                                         "    verify V1 {\n"
                                         "        prove = reachable(@E_len);\n"
                                         "        using = [smt;\n"
                                         "        status = PENDING;\n"
                                         "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("']' closing the list"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsReferenceListWithoutClosingBracket) {
  const Rejection rejected = Reject(CaseWith(
      "    unknown U1 {\n"
      "        property = reachable(@E_len);\n"
      "        reason = UNKNOWN_ALIAS;\n"
      "        blocking = [@E_len;\n"
      "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("']' closing the list"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsNonStringWhereAStringIsExpected) {
  const Rejection rejected = Reject(
      "evidence {\n    schema = eir.v1;\n    level = l1;\n"
      "    state = POSSIBLE_DEFECT;\n}\n");
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("expected the schema version string"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsNonStableIdEntityStableId) {
  const Rejection rejected = Reject(Case(std::string(kBaseEntities) +
                                         "    entity E_bad : value {\n"
                                         "        stable_id = \"nonsense\";\n"
                                         "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("not a stable ID of the form"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsNonStableIdFactStableId) {
  const Rejection rejected = Reject(Case(std::string(kBaseEntities) +
                                         "    fact F_bad {\n"
                                         "        predicate = reachable(@E_len);\n"
                                         "        epistemic = must;\n"
                                         "        stable_id = \"nonsense\";\n"
                                         "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("not a stable ID of the form"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsNonStableIdDependencyStableId) {
  const Rejection rejected = Reject(Case(std::string(kBaseEntities) +
                                         "    dependency D1 {\n"
                                         "        kind = summary;\n"
                                         "        stable_id = \"nonsense\";\n"
                                         "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("not a stable ID of the form"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsNonStableIdSummarySummaryId) {
  const Rejection rejected = Reject(CasePlus(
      "    summary S1 {\n"
      "        function = @E_sink;\n"
      "        summary_id = \"nonsense\";\n"
      "        components = [range];\n"
      "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("not a stable ID of the form"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsNonStableIdProvenanceAnalysisRun) {
  const Rejection rejected = Reject(CaseWith(
      "    provenance PR1 {\n"
      "        producer = veritas.wpa;\n"
      "        analysis_run = \"nonsense\";\n"
      "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("not a stable ID of the form"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsNonBooleanWhereABooleanIsExpected) {
  const Rejection rejected = Reject(Case(std::string(kBaseEntities) +
                                         "    fact F_bad {\n"
                                         "        predicate = reachable(@E_len);\n"
                                         "        epistemic = must;\n"
                                         "        derived = \"yes\";\n"
                                         "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("'derived' attribute takes a boolean "
                                        "literal"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsNonCallWhereACallIsExpected) {
  const Rejection rejected = Reject(CaseWith(
      "    unknown U1 {\n"
      "        property = reachable(@E_len);\n"
      "        reason = UNKNOWN_ALIAS;\n"
      "        suggested_resolution = \"infer\";\n"
      "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("takes a function call"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsNonCallWhereAnAssumptionSourceIsExpected) {
  const Rejection rejected = Reject(CasePlus(
      "    assumption A1 {\n"
      "        predicate = reachable(@E_sink);\n"
      "        source = 7;\n"
      "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("the source attribute takes a qualified "
                                        "identifier or a function call"),
            std::string::npos)
      << rejected.error.message;
}

// The goal attribute is the one attribute whose name is its value, so the set
// of spellings admitted is `ParseProofGoalKind`'s own set rather than a second
// literal list in the parser. Both halves of that coupling are pinned: every
// spelling the helper recognises lowers to its enumerator, and a spelling it
// does not is refused as an unknown attribute instead of reaching an unchecked
// `StatusOr::value()`.
TEST(EirParserTest, MapsEveryProofGoalSpellingAndRefusesTheRest) {
  const std::pair<std::string_view, ProofGoalKind> kGoals[] = {
      {"prove", ProofGoalKind::kProve},
      {"refute", ProofGoalKind::kRefute},
      {"check", ProofGoalKind::kCheck},
  };
  for (const auto& [spelling, kind] : kGoals) {
    StatusOr<EvidenceCase> value = Accept(
        CasePlus("    verify V1 {\n"
                 "        " + std::string(spelling) +
                 " = reachable(@E_len);\n"
                 "        status = PENDING;\n"
                 "    }\n"),
        std::string("the goal spelling ") + std::string(spelling));
    ASSERT_TRUE(value.ok());
    ASSERT_EQ(value->proof_obligations.size(), 1U);
    EXPECT_EQ(value->proof_obligations[0].goal_kind, kind);
  }
  // `witness` is not a `ProofGoalKind`, so the branch does not take it and the
  // body reports it as an attribute the declaration does not list. A reader
  // that widened the branch without widening the helper would fail here.
  const Rejection rejected = Reject(CasePlus(
      "    verify V1 {\n"
      "        witness = reachable(@E_len);\n"
      "        status = PENDING;\n"
      "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("unknown verify attribute 'witness'"),
            std::string::npos)
      << rejected.error.message;
}

// `Scope ::= "global" | "function" | "path" | "basic_block" | "callsite" |
// "entity" | FunctionCall`. The model carries a scope as a plain string, so the
// production is the only place the enumeration is enforced: every keyword must
// be admitted, the call form must be admitted, and nothing else may be.
TEST(EirParserTest, AcceptsEveryScopeKeywordAndTheCallForm) {
  constexpr std::string_view kKeywords[] = {"global",      "function", "path",
                                            "basic_block", "callsite", "entity"};
  for (const std::string_view keyword : kKeywords) {
    StatusOr<EvidenceCase> value = Accept(
        CasePlus("    constraint K1 {\n"
                 "        expr = reachable(@E_len);\n"
                 "        scope = " + std::string(keyword) + ";\n"
                 "        epistemic = must;\n"
                 "    }\n"),
        std::string("the scope keyword ") + std::string(keyword));
    ASSERT_TRUE(value.ok());
    ASSERT_EQ(value->constraints.size(), 1U);
    EXPECT_EQ(value->constraints[0].scope, keyword);
  }

  StatusOr<EvidenceCase> call = Accept(
      CasePlus("    constraint K1 {\n"
               "        expr = reachable(@E_len);\n"
               "        scope = decode_frame(@E_sink);\n"
               "        epistemic = must;\n"
               "    }\n"),
      "a call-shaped scope");
  ASSERT_TRUE(call.ok());
  ASSERT_EQ(call->constraints.size(), 1U);
  EXPECT_EQ(call->constraints[0].scope, "decode_frame(E_sink)");
}

TEST(EirParserTest, RejectsAnIdentifierOutsideTheScopeKeywords) {
  const Rejection constraint = Reject(CasePlus(
      "    constraint K1 {\n"
      "        expr = reachable(@E_len);\n"
      "        scope = everywhere;\n"
      "        epistemic = must;\n"
      "    }\n"));
  ASSERT_TRUE(constraint.rejected);
  EXPECT_NE(constraint.error.message.find("one of the six scope keywords"),
            std::string::npos)
      << constraint.error.message;

  // The same attribute name on `assumption`, whose `source` is the open
  // `AssumptionSource` and whose `scope` is the closed `Scope`. The two must
  // not share the looser rule.
  const Rejection assumption = Reject(CasePlus(
      "    assumption A1 {\n"
      "        predicate = reachable(@E_sink);\n"
      "        source = veritas.eval;\n"
      "        scope = everywhere;\n"
      "    }\n"));
  ASSERT_TRUE(assumption.rejected);
  EXPECT_NE(assumption.error.message.find("one of the six scope keywords"),
            std::string::npos)
      << assumption.error.message;
}

TEST(EirParserTest, RejectsSecondVerificationGoal) {
  const Rejection rejected = Reject(Case(std::string(kBaseEntities) +
                                         "    verify V1 {\n"
                                         "        prove = reachable(@E_len);\n"
                                         "        refute = reachable(@E_len);\n"
                                         "        status = PENDING;\n"
                                         "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("declares more than one goal"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsMalformedPathExpression) {
  const Rejection rejected = Reject(Case(std::string(kBaseEntities) +
                                         "    path P1 call {\n"
                                         "        E_len -> @E_sink;\n"
                                         "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("the first path segment"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsUnknownPathAttribute) {
  const Rejection rejected = Reject(Case(std::string(kBaseEntities) +
                                         "    path P1 call {\n"
                                         "        @E_len -> @E_sink;\n"
                                         "        bogus = 1;\n"
                                         "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("unknown path attribute 'bogus'"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsUnterminatedPathDeclaration) {
  // The document stops after the path expression: the path's own `}` is
  // missing, and nothing else is left to close it.
  const Rejection rejected =
      Reject(std::string("evidence {\n    schema = \"eir.v1\";\n"
                         "    level = l1;\n"
                         "    state = POSSIBLE_DEFECT;\n\n") +
             std::string(kContextBlock) + std::string(kBaseEntities) +
             "    path P1 call {\n"
             "        @E_len -> @E_sink;\n");
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("unterminated path declaration"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsUnterminatedPathConditionsBlock) {
  // The document stops inside the conditions block: no `}` closes the
  // conditions, the path, or the case, so the innermost unterminated construct
  // is the one named.
  const Rejection rejected =
      Reject(std::string("evidence {\n    schema = \"eir.v1\";\n"
                         "    level = l1;\n"
                         "    state = POSSIBLE_DEFECT;\n\n") +
             std::string(kContextBlock) + std::string(kBaseEntities) +
             "    path P1 call {\n"
             "        @E_len -> @E_sink;\n"
             "        conditions {\n"
             "            @E_len > 0;\n");
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("unterminated path conditions block"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsRepeatedPathConditionsBlock) {
  const Rejection rejected = Reject(CasePlus(
      "    path P1 call {\n"
      "        @E_len -> @E_sink;\n"
      "        conditions { @E_len > 0; }\n"
      "        conditions { @E_len > 0; }\n"
      "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("the path 'conditions' block appears "
                                        "more than once"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsRepeatedPathFeasibility) {
  const Rejection rejected = Reject(CasePlus(
      "    path P1 call {\n"
      "        @E_len -> @E_sink;\n"
      "        feasible = SAT;\n"
      "        feasible = UNSAT;\n"
      "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("the path 'feasible' attribute appears "
                                        "more than once"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsRepeatedPathProvenance) {
  const Rejection rejected = Reject(CasePlus(
      "    path P1 call {\n"
      "        @E_len -> @E_sink;\n"
      "        provenance = @PR1;\n"
      "        provenance = @PR1;\n"
      "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message
                .find("the path 'provenance' attribute appears more than once"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsMalformedResourceBudget) {
  const Rejection rejected = Reject(CasePlus(
      "    verify V1 {\n"
      "        prove = reachable(@E_len);\n"
      "        budget = ;\n"
      "        status = PENDING;\n"
      "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("expected a resource budget, found the "
                                        "token ';'"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsMalformedPropertyValue) {
  const Rejection rejected = Reject(CasePlus(
      "    entity E_bad : value {\n"
      "        origin = ;\n"
      "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("expected a property value, found the "
                                        "token ';'"),
            std::string::npos)
      << rejected.error.message;
}

TEST(EirParserTest, RejectsNonIdentifierWhereANameIsExpected) {
  const Rejection rejected = Reject(Case(std::string(kBaseEntities) +
                                         "    entity 7 : value {\n"
                                         "        origin = \"x\";\n"
                                         "    }\n"));
  ASSERT_TRUE(rejected.rejected);
  EXPECT_NE(rejected.error.message.find("expected an entity identifier"),
            std::string::npos)
      << rejected.error.message;
}

// An integer literal is range-checked by the lexer, so this site is reachable
// only through a token stream the parser is handed directly. The parser
// re-applies the `std::int64_t` bound rather than trusting a property of its
// input beyond the token contract, and this is the test that fails when it
// stops doing so.
TEST(EirParserTest, RejectsIntegerTokenOutsideTheInt64Range) {
  const std::string source = PredicateOnlyCase("@E_len > 0");
  EirParseError error;
  EirLexer lexer(source, &error);
  StatusOr<std::vector<Token>> tokens = lexer.Tokenize();
  ASSERT_TRUE(tokens.ok()) << error.message;

  std::size_t replaced = 0;
  for (Token& token : tokens.value()) {
    if (token.kind == TokenKind::kInteger) {
      token.text = "99999999999999999999";
      ++replaced;
    }
  }
  ASSERT_EQ(replaced, 1U);

  EirParser parser(tokens.value(), &error);
  StatusOr<EvidenceCase> value = parser.Parse();
  ASSERT_FALSE(value.ok());
  EXPECT_NE(error.message.find("outside the range of a signed 64-bit integer"),
            std::string::npos)
      << error.message;
}

// --- The malformed representation corpus ------------------------------------

// `unsupported_schema.eir` — REP-005. The schema version is a semantic field
// and the only value the model implements is `eir.v1`.
TEST(EirParserTest, CorpusUnsupportedSchemaFailsForItsOwnReason) {
  const std::string source = ReadCorpus("unsupported_schema.eir");
  ASSERT_FALSE(source.empty());
  const Rejection rejected = Reject(source);
  ASSERT_TRUE(rejected.rejected) << "the corpus file parsed";
  EXPECT_NE(rejected.error.message.find("schema_version"), std::string::npos)
      << rejected.error.message;
  EXPECT_NE(rejected.error.message.find("'eir.v2'"), std::string::npos)
      << rejected.error.message;
}

// `duplicate_member_id.eir` — one flat identifier space, so the second `E_sink`
// leaves every `@E_sink` ambiguous.
TEST(EirParserTest, CorpusDuplicateMemberIdFailsForItsOwnReason) {
  const std::string source = ReadCorpus("duplicate_member_id.eir");
  ASSERT_FALSE(source.empty());
  const Rejection rejected = Reject(source);
  ASSERT_TRUE(rejected.rejected) << "the corpus file parsed";
  EXPECT_NE(rejected.error.message.find("duplicate_local_id"), std::string::npos)
      << rejected.error.message;
}

// `dangling_reference.eir` — the document has a complete program context, so
// the missing-context check cannot fire first and mask the real defect.
TEST(EirParserTest, CorpusDanglingReferenceFailsForItsOwnReason) {
  const std::string source = ReadCorpus("dangling_reference.eir");
  ASSERT_FALSE(source.empty());
  const Rejection rejected = Reject(source);
  ASSERT_TRUE(rejected.rejected) << "the corpus file parsed";
  EXPECT_NE(rejected.error.message.find("dangling_reference"), std::string::npos)
      << rejected.error.message;
  EXPECT_EQ(rejected.error.message.find("missing_program_context"),
            std::string::npos)
      << rejected.error.message;
  EXPECT_NE(rejected.error.message.find("E_nowhere"), std::string::npos)
      << rejected.error.message;
}

// `repeated_comparison.eir` — REP-007.
TEST(EirParserTest, CorpusRepeatedComparisonFailsForItsOwnReason) {
  const std::string source = ReadCorpus("repeated_comparison.eir");
  ASSERT_FALSE(source.empty());
  const Rejection rejected = Reject(source);
  ASSERT_TRUE(rejected.rejected) << "the corpus file parsed";
  EXPECT_NE(rejected.error.message.find("a comparison is not associative"),
            std::string::npos)
      << rejected.error.message;
}

// `unterminated_string.eir` — REP-007. The lexical stage refuses it at the
// quote that opened the literal, not at the end of the document.
TEST(EirParserTest, CorpusUnterminatedStringFailsForItsOwnReason) {
  const std::string source = ReadCorpus("unterminated_string.eir");
  ASSERT_FALSE(source.empty());
  const Rejection rejected = Reject(source);
  ASSERT_TRUE(rejected.rejected) << "the corpus file parsed";
  EXPECT_NE(rejected.error.message.find("unterminated string literal"),
            std::string::npos)
      << rejected.error.message;
  EXPECT_EQ(rejected.error.offset, source.find("description = \"") +
                                       std::string("description = ").size())
      << rejected.error.message;
}

}  // namespace
}  // namespace veritas::evidence
