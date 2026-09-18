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

// EvidenceCaseBuilderTest.cpp — the M10B handoff to `eir.v1` assembly contract.
//
// Every test here is written so that disabling the one rule it names turns that
// test red and leaves its siblings green. The rule names are the design spec's
// BLD-001..BLD-010 identifiers.
//
// The builder's whole authority is bounded by what the handoff carries. Where
// the request is silent the case is silent: it does not invent an assumption, a
// hypothesis, a summary reference, or a source anchor, and it never resolves an
// open dominating-check question by asserting its negation.

#include <algorithm>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "evidence/EvidenceScenario.h"
#include "veritas/core/Ids.h"
#include "veritas/cpg/CpgTypes.h"
#include "veritas/evidence/EvidenceCanonicalizer.h"
#include "veritas/evidence/EvidenceCaseBuilder.h"
#include "veritas/evidence/EvidenceQueryService.h"
#include "veritas/evidence/EvidenceValidator.h"
#include "veritas/facts/AnalysisFact.h"

namespace veritas::evidence {
namespace {

namespace sem = analysis::semantic;
using testing::EvidenceScenarioBuilder;

constexpr std::string_view kAbsencePredicate = "dominates_bounds_check";

// A spelling EIR-T cannot carry in a `Producer` position. `QualifiedId` is
// `Identifier { "." Identifier }` over `[A-Za-z][A-Za-z0-9_]*`, so a hyphen is
// outside the alphabet — the reason the L49 translation exists.
bool IsWritableProducer(std::string_view text) {
  if (text.empty()) {
    return false;
  }
  for (char c : text) {
    const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    const bool digit = c >= '0' && c <= '9';
    if (!(letter || digit || c == '_' || c == '.')) {
      return false;
    }
  }
  return text.front() != '.' && text.back() != '.';
}

// --- Request and build helpers ----------------------------------------------

// The pristine handoff at `level`: every query complete, every certificate
// bound, every witness selected.
EvidenceBuildRequest PristineRequest(EvidenceLevel level) {
  return EvidenceScenarioBuilder().BuildRequest(level);
}

// The DEM-001 handoff: the dominating-check query is a truncated-empty result,
// which is what `MakeOverflowEvidenceCase` is projected from.
EvidenceBuildRequest TruncatedRequest(EvidenceLevel level) {
  EvidenceScenarioBuilder scenario;
  scenario.WithTruncatedDominatingChecks(TruncationReason::kMaxPaths);
  return scenario.BuildRequest(level);
}

EvidenceCase BuildOrFail(const EvidenceBuildRequest& request) {
  auto built = EvidenceCaseBuilder().Build(request);
  if (!built.ok()) {
    ADD_FAILURE() << "the build failed: " << built.status().message();
    return EvidenceCase{};
  }
  return std::move(built).value();
}

// A build that is expected to be refused.
Status BuildStatus(const EvidenceBuildRequest& request) {
  auto built = EvidenceCaseBuilder().Build(request);
  if (built.ok()) {
    return Status::Ok();
  }
  return built.status();
}

// --- Member finders ---------------------------------------------------------

std::vector<const Fact*> FindFacts(const EvidenceCase& value,
                                   std::string_view predicate) {
  std::vector<const Fact*> found;
  for (const Fact& fact : value.facts) {
    if (fact.predicate.text == predicate) {
      found.push_back(&fact);
    }
  }
  return found;
}

const Fact* FindFactWith(const EvidenceCase& value, std::string_view predicate,
                         EpistemicState epistemic) {
  for (const Fact& fact : value.facts) {
    if (fact.predicate.text == predicate && fact.epistemic == epistemic) {
      return &fact;
    }
  }
  return nullptr;
}

std::vector<const Omission*> FindOmissions(const EvidenceCase& value,
                                           std::string_view kind) {
  std::vector<const Omission*> found;
  for (const Omission& omission : value.omissions) {
    if (omission.kind == kind) {
      found.push_back(&omission);
    }
  }
  return found;
}

const Provenance* FindProvenance(const EvidenceCase& value,
                                 std::string_view rule) {
  for (const Provenance& record : value.provenance) {
    if (record.rule == rule) {
      return &record;
    }
  }
  return nullptr;
}

// The record declared under a given case-local handle. Distinct from the
// rule-based finder above, which is what the L49 test wants; here the handles
// are the point, so this one matches `id`.
const Provenance* FindProvenanceById(const EvidenceCase& value,
                                     std::string_view id) {
  for (const Provenance& record : value.provenance) {
    if (record.id == id) {
      return &record;
    }
  }
  return nullptr;
}

std::set<std::string> IdsOf(const std::vector<Entity>& members) {
  std::set<std::string> ids;
  for (const Entity& member : members) ids.insert(member.id);
  return ids;
}
std::set<std::string> IdsOf(const std::vector<Edge>& members) {
  std::set<std::string> ids;
  for (const Edge& member : members) ids.insert(member.id);
  return ids;
}
std::set<std::string> IdsOf(const std::vector<Path>& members) {
  std::set<std::string> ids;
  for (const Path& member : members) ids.insert(member.id);
  return ids;
}
std::set<std::string> IdsOf(const std::vector<Fact>& members) {
  std::set<std::string> ids;
  for (const Fact& member : members) ids.insert(member.id);
  return ids;
}
std::set<std::string> IdsOf(const std::vector<Assumption>& members) {
  std::set<std::string> ids;
  for (const Assumption& member : members) ids.insert(member.id);
  return ids;
}
std::set<std::string> IdsOf(const std::vector<Hypothesis>& members) {
  std::set<std::string> ids;
  for (const Hypothesis& member : members) ids.insert(member.id);
  return ids;
}
std::set<std::string> IdsOf(const std::vector<Unknown>& members) {
  std::set<std::string> ids;
  for (const Unknown& member : members) ids.insert(member.id);
  return ids;
}
std::set<std::string> IdsOf(const std::vector<Constraint>& members) {
  std::set<std::string> ids;
  for (const Constraint& member : members) ids.insert(member.id);
  return ids;
}
std::set<std::string> IdsOf(const std::vector<Provenance>& members) {
  std::set<std::string> ids;
  for (const Provenance& member : members) ids.insert(member.id);
  return ids;
}
std::set<std::string> IdsOf(const std::vector<ProofObligation>& members) {
  std::set<std::string> ids;
  for (const ProofObligation& member : members) ids.insert(member.id);
  return ids;
}
std::set<std::string> IdsOf(const std::vector<SummaryReference>& members) {
  std::set<std::string> ids;
  for (const SummaryReference& member : members) ids.insert(member.id);
  return ids;
}
std::set<std::string> IdsOf(const std::vector<Dependency>& members) {
  std::set<std::string> ids;
  for (const Dependency& member : members) ids.insert(member.id);
  return ids;
}
std::set<std::string> IdsOf(const std::vector<Omission>& members) {
  std::set<std::string> ids;
  for (const Omission& member : members) ids.insert(member.id);
  return ids;
}

// Every case-local identifier the case declares, across all member families.
std::set<std::string> AllMemberIds(const EvidenceCase& value) {
  std::set<std::string> ids;
  auto merge = [&ids](const std::set<std::string>& other) {
    ids.insert(other.begin(), other.end());
  };
  merge(IdsOf(value.entities));
  merge(IdsOf(value.edges));
  merge(IdsOf(value.paths));
  merge(IdsOf(value.facts));
  merge(IdsOf(value.assumptions));
  merge(IdsOf(value.hypotheses));
  merge(IdsOf(value.unknowns));
  merge(IdsOf(value.constraints));
  merge(IdsOf(value.provenance));
  merge(IdsOf(value.proof_obligations));
  merge(IdsOf(value.summaries));
  merge(IdsOf(value.dependencies));
  merge(IdsOf(value.omissions));
  return ids;
}

std::set<std::string> Difference(const std::set<std::string>& left,
                                 const std::set<std::string>& right) {
  std::set<std::string> out;
  std::set_difference(left.begin(), left.end(), right.begin(), right.end(),
                      std::inserter(out, out.begin()));
  return out;
}

// --- The binding -------------------------------------------------------------

// The builder reproduces `BindProgram`'s mapping for every binding it was
// given, and the case it returns is bound to exactly one analysis run — the run
// every provenance record also belongs to. The run is carried four times over
// in the handoff (the flow slice's metadata, every fact set's metadata, the
// provenance graph, and each completion binding), so agreement is checkable
// rather than assumed.
TEST(EvidenceCaseBuilderTest, BindsTheCaseToTheProgramIdentityItWasGiven) {
  const EvidenceBuildRequest request = PristineRequest(EvidenceLevel::kL1);
  auto built = EvidenceCaseBuilder().Build(request);
  ASSERT_TRUE(built.ok()) << built.status().message();
  const EvidenceCase& value = built.value();

  EXPECT_TRUE(RequireValidEvidenceCase(value).ok());

  EXPECT_EQ(value.program.repository_id, request.context.repository_id);
  EXPECT_EQ(value.program.revision_id, request.context.revision_id);
  EXPECT_EQ(value.program.build_variant_id, request.context.build_variant_id);
  EXPECT_EQ(value.program.target_triple, request.context.target_triple);

  // The two fields `build::ProgramContext` cannot carry (ruling L51): both are
  // non-empty and both come from the request, never from a literal.
  EXPECT_FALSE(value.program.analysis_configuration_id.empty());
  EXPECT_EQ(value.program.analysis_configuration_id,
            request.analysis_configuration_id);
  EXPECT_FALSE(value.program.type_layout_id.empty());
  EXPECT_EQ(value.program.type_layout_id, request.context.type_layout_hash);
  EXPECT_EQ(value.program.analyzer_versions, request.analyzer_versions);

  ASSERT_TRUE(value.program.analysis_run_id.has_value());
  const core::StableId run = *value.program.analysis_run_id;
  EXPECT_EQ(run, request.input.flow_slice.metadata.analysis_run_id);
  EXPECT_FALSE(value.provenance.empty());
  for (const Provenance& record : value.provenance) {
    ASSERT_TRUE(record.analysis_run_id.has_value())
        << "provenance '" << record.id << "' carries no run";
    EXPECT_EQ(*record.analysis_run_id, run)
        << "provenance '" << record.id << "' belongs to another run";
  }

  // The case is identified, and the identity is the canonical bytes' digest.
  ASSERT_TRUE(value.evidence_id.has_value());
  auto canonical = CanonicalEvidenceBytes(value);
  ASSERT_TRUE(canonical.ok());
  auto expected = ComputeEvidenceId(value);
  ASSERT_TRUE(expected.ok());
  EXPECT_EQ(*value.evidence_id, *expected);
}

// --- BLD-001 -----------------------------------------------------------------

// A complete, unsafe input builds a valid L1 case: the causal slice, the bound
// facts, the open questions, the constraint, the obligation, and the program
// binding, all present and mutually resolvable.
TEST(EvidenceCaseBuilderTest, Bld001UnsafeCompleteInputBuildsAValidL1Case) {
  const EvidenceCase value = BuildOrFail(PristineRequest(EvidenceLevel::kL1));

  EXPECT_EQ(value.level, EvidenceLevel::kL1);
  EXPECT_EQ(value.schema_version, std::string(kEvidenceSchemaVersion));
  EXPECT_TRUE(RequireValidEvidenceCase(value).ok());
  EXPECT_EQ(value.verification_state, VerificationState::kPossibleDefect);

  EXPECT_EQ(value.primary_claim.kind, ClaimKind::kBufferOverflow);
  EXPECT_EQ(value.primary_claim.severity, Severity::kHigh);
  EXPECT_FALSE(value.primary_claim.subject.empty());

  // The three domain relations the handoff carried are all stated.
  EXPECT_EQ(FindFacts(value, "range").size(), 1u);
  EXPECT_EQ(FindFacts(value, "capacity").size(), 1u);
  EXPECT_EQ(FindFacts(value, "alias").size(), 1u);

  // The obligation is pending and names the verifiers that may decide it.
  ASSERT_EQ(value.proof_obligations.size(), 1u);
  EXPECT_EQ(value.proof_obligations.front().status, ProofStatus::kPending);
  EXPECT_FALSE(value.proof_obligations.front().verifier_kinds.empty());

  // The path is the ordered value-flow chain to the sink.
  ASSERT_EQ(value.paths.size(), 1u);
  EXPECT_EQ(value.paths.front().kind, PathKind::kValueFlow);
  EXPECT_GE(value.paths.front().entity_ids.size(), 2u);
}

// --- BLD-002 -----------------------------------------------------------------

// A check the query did return is counterevidence. The case records it and
// stops: no verdict follows from presence alone, so nothing concludes the sink
// is safe and nothing concludes the check dominates.
TEST(EvidenceCaseBuilderTest, Bld002SafeDominatingCheckStaysCounterevidence) {
  EvidenceBuildRequest request = PristineRequest(EvidenceLevel::kL1);
  request.input.dominating_checks.facts.push_back(
      EvidenceScenarioBuilder().MakeCheckFact("dominating_check:memcpy_site"));

  const EvidenceCase value = BuildOrFail(request);
  EXPECT_TRUE(RequireValidEvidenceCase(value).ok());

  // The absence is not asserted: a returned check is not a missing check.
  EXPECT_EQ(FindFactWith(value, kAbsencePredicate, EpistemicState::kMustNot),
            nullptr);
  EXPECT_EQ(FindProvenance(value, kClosedWorldAbsenceRuleId), nullptr);

  // The check is recorded, with the epistemic state the analysis stated.
  const std::vector<const Fact*> counterevidence =
      FindFacts(value, kAbsencePredicate);
  ASSERT_EQ(counterevidence.size(), 1u);
  EXPECT_EQ(counterevidence.front()->epistemic, EpistemicState::kMust);
  EXPECT_EQ(counterevidence.front()->producer, "analysis.soundness_coverage");
  EXPECT_EQ(counterevidence.front()->confidence, Confidence::kExact);

  // Presence alone decides nothing about the case.
  EXPECT_EQ(value.verification_state, VerificationState::kPossibleDefect);
  for (const ProofObligation& obligation : value.proof_obligations) {
    EXPECT_EQ(obligation.status, ProofStatus::kPending);
    EXPECT_TRUE(obligation.result_id.empty());
  }
}

// --- BLD-003 -----------------------------------------------------------------

// A result returned for a sibling scope says nothing about this sink. The
// builder re-scopes the question rather than promoting the sibling's answer to
// a universal check over the claim.
TEST(EvidenceCaseBuilderTest, Bld003SiblingCheckScopeNeverBecomesUniversal) {
  EvidenceBuildRequest request = PristineRequest(EvidenceLevel::kL1);
  // The alias query is scoped to the claim's subject, not to the sink.
  request.input.dominating_checks.metadata.query_provenance_id =
      request.input.aliases.metadata.query_provenance_id;

  const EvidenceCase value = BuildOrFail(request);
  EXPECT_TRUE(RequireValidEvidenceCase(value).ok());

  EXPECT_EQ(FindFactWith(value, kAbsencePredicate, EpistemicState::kMustNot),
            nullptr);
  ASSERT_EQ(FindOmissions(value, "dominating_check").size(), 1u);
  const Omission* omission = FindOmissions(value, "dominating_check").front();
  EXPECT_TRUE(omission->expandable);
  EXPECT_NE(omission->reason.find("scoped to another member"),
            std::string::npos);
}

// More than one value-flow chain reaching the slice means the path states the
// one it followed and not all of them. A sibling chain never becomes the
// universal subject of the claim.
TEST(EvidenceCaseBuilderTest, Bld003MixedPathsNeverBecomeAUniversalCheck) {
  EvidenceBuildRequest request = PristineRequest(EvidenceLevel::kL1);
  const cpg::CpgNode* entry = nullptr;
  const cpg::CpgNode* vendor = nullptr;
  for (const cpg::CpgNode& node : request.input.flow_slice.nodes) {
    if (node.label == "entry") entry = &node;
    if (node.label == "vendor_validate") vendor = &node;
  }
  ASSERT_NE(entry, nullptr);
  ASSERT_NE(vendor, nullptr);
  // A second, independent chain: the entry function calls the vendor validator.
  request.input.flow_slice.edges.push_back(
      EvidenceScenarioBuilder().MakeEdge("flows:entry:vendor_validate",
                                         cpg::EdgeKind::kFlowsTo,
                                         entry->node_id, vendor->node_id));

  const EvidenceCase value = BuildOrFail(request);
  EXPECT_TRUE(RequireValidEvidenceCase(value).ok());

  const std::vector<const Omission*> mixed = FindOmissions(value, "mixed_paths");
  ASSERT_EQ(mixed.size(), 1u);
  EXPECT_TRUE(mixed.front()->expandable);
  EXPECT_NE(mixed.front()->reason.find("more than one distinct value-flow"),
            std::string::npos);
  // The omission's subject is the path the case did follow.
  ASSERT_EQ(value.paths.size(), 1u);
  EXPECT_EQ(mixed.front()->subject, value.paths.front().id);
}

// --- BLD-004 -----------------------------------------------------------------

// An opaque validator produces an unknown that blocks the claim, not a
// conclusion about it. The reason code is one M10B can actually supply — never
// an interrupted-query code, which would claim the case carries the M9 producer
// that certifies one.
TEST(EvidenceCaseBuilderTest, Bld004OpaqueValidationProducesABlockingUnknown) {
  const EvidenceCase value = BuildOrFail(PristineRequest(EvidenceLevel::kL1));
  ASSERT_FALSE(value.unknowns.empty());

  std::vector<const Unknown*> opaque;
  for (const Unknown& unknown : value.unknowns) {
    if (unknown.reason_code == UnknownReasonCode::kExternalFunction) {
      opaque.push_back(&unknown);
    }
  }
  ASSERT_EQ(opaque.size(), 1u) << "the external validator is not reported";
  const Unknown& unknown = *opaque.front();

  EXPECT_NE(unknown.reason_code, UnknownReasonCode::kAnalysisTimeout);
  EXPECT_NE(unknown.reason_code, UnknownReasonCode::kStateExplosion);
  EXPECT_EQ(unknown.reason, "EXTERNAL_FUNCTION");

  // It blocks the claim and every derived fact the case still reports, so the
  // open question is visible on the conclusion it undermines.
  ASSERT_FALSE(unknown.blocking_ids.empty());
  EXPECT_NE(std::find(unknown.blocking_ids.begin(), unknown.blocking_ids.end(),
                      value.primary_claim.id),
            unknown.blocking_ids.end());
  const std::set<std::string> declared = AllMemberIds(value);
  for (const std::string& blocked : unknown.blocking_ids) {
    EXPECT_TRUE(blocked == value.primary_claim.id ||
                declared.count(blocked) != 0)
        << "the unknown blocks '" << blocked << "', which is neither the "
        << "claim nor a member the case declares";
  }
}

// --- BLD-005 -----------------------------------------------------------------

// An alias the analysis did not establish as MUST is carried as a *condition*
// on the path that depends on it. The case records the uncertainty where it
// belongs instead of strengthening it into a supporting premise.
TEST(EvidenceCaseBuilderTest, Bld005UncertainAliasPreventsStrengthenedReasoning) {
  const EvidenceCase value = BuildOrFail(PristineRequest(EvidenceLevel::kL1));

  const std::vector<const Fact*> aliases = FindFacts(value, "alias");
  ASSERT_EQ(aliases.size(), 1u);
  EXPECT_EQ(aliases.front()->epistemic, EpistemicState::kMay);
  EXPECT_EQ(aliases.front()->confidence, Confidence::kMedium);
  EXPECT_EQ(FindFactWith(value, "alias", EpistemicState::kMust), nullptr);

  // The uncertainty is stated on the path, as a condition, and nowhere
  // promoted: the constraint the obligation asks a verifier to prove is a
  // requirement, so it cannot be a MUST either.
  ASSERT_EQ(value.paths.size(), 1u);
  ASSERT_FALSE(value.paths.front().conditions.empty());
  const Expression& condition = value.paths.front().conditions.front();
  EXPECT_EQ(condition.kind, Expression::Kind::kCall);
  EXPECT_EQ(condition.text, "alias");
  ASSERT_EQ(condition.operands.size(), 2u);
  EXPECT_EQ(condition.operands[0].kind, Expression::Kind::kReference);
  EXPECT_EQ(condition.operands[1].kind, Expression::Kind::kReference);

  ASSERT_EQ(value.constraints.size(), 1u);
  EXPECT_EQ(value.constraints.front().epistemic, EpistemicState::kMay);
}

// --- BLD-006 -----------------------------------------------------------------

// A truncated-empty result is a withheld answer, never negative evidence. The
// case states the unknown naming the budget that cut the query short, plus the
// omission that can recover it — and asserts no absence.
TEST(EvidenceCaseBuilderTest, Bld006TruncatedEmptyCheckIsNeverNegativeEvidence) {
  const EvidenceCase value = BuildOrFail(TruncatedRequest(EvidenceLevel::kL1));
  EXPECT_TRUE(RequireValidEvidenceCase(value).ok());

  EXPECT_EQ(FindFactWith(value, kAbsencePredicate, EpistemicState::kMustNot),
            nullptr);
  EXPECT_EQ(FindProvenance(value, kClosedWorldAbsenceRuleId), nullptr);

  const std::vector<const Omission*> truncated =
      FindOmissions(value, "truncated_query");
  ASSERT_EQ(truncated.size(), 1u);
  EXPECT_TRUE(truncated.front()->expandable);

  bool found_unknown = false;
  for (const Unknown& unknown : value.unknowns) {
    if (unknown.id != truncated.front()->subject) {
      continue;
    }
    found_unknown = true;
    EXPECT_EQ(unknown.reason_code, UnknownReasonCode::kMissingSpecification);
    EXPECT_NE(unknown.reason.find("truncated"), std::string::npos);
  }
  EXPECT_TRUE(found_unknown) << "the truncated query states no unknown";
}

// --- BLD-007 -----------------------------------------------------------------

// The builder performs exactly one derivation of its own, and the tests below
// each remove one of its six preconditions. None of them may produce a negative
// fact: only the fully valid case does.

// The completion certificate the dominating-check result names.
facts::AnalysisFact* CheckCertificate(EvidenceBuildRequest* request) {
  const core::StableId id =
      request->input.dominating_checks.metadata.query_provenance_id;
  for (facts::AnalysisFact& fact : request->input.query_completion_facts) {
    if (fact.fact_id == id) {
      return &fact;
    }
  }
  return nullptr;
}

// Asserts that a request produced an open question rather than an absence.
void ExpectOpenCheck(const EvidenceCase& value) {
  EXPECT_EQ(FindFactWith(value, kAbsencePredicate, EpistemicState::kMustNot),
            nullptr)
      << "an uncertified closed-world query was turned into negative evidence";
  EXPECT_EQ(FindProvenance(value, kClosedWorldAbsenceRuleId), nullptr);
  ASSERT_FALSE(value.unknowns.empty());
  const std::vector<const Omission*> omissions =
      FindOmissions(value, "dominating_check");
  ASSERT_FALSE(omissions.empty());
  EXPECT_TRUE(omissions.front()->expandable);
}

TEST(EvidenceCaseBuilderTest, Bld007MissingCompletionCertificateStaysOpen) {
  EvidenceBuildRequest request = PristineRequest(EvidenceLevel::kL1);
  const core::StableId named =
      request.input.dominating_checks.metadata.query_provenance_id;
  auto& facts = request.input.query_completion_facts;
  facts.erase(std::remove_if(facts.begin(), facts.end(),
                             [&named](const facts::AnalysisFact& fact) {
                               return fact.fact_id == named;
                             }),
              facts.end());
  // The binding and the witness for a certificate the handoff no longer carries
  // must not be enough to reconstruct it.
  EXPECT_EQ(CheckCertificate(&request), nullptr);

  ExpectOpenCheck(BuildOrFail(request));
}

// A certificate that does not re-derive from its own cells was patched after
// publication. That is a malformed handoff, not an open question, and it is
// refused before any member is built — never read as an absence.
TEST(EvidenceCaseBuilderTest, Bld007UnrederivableCertificateIsRefused) {
  EvidenceBuildRequest request = PristineRequest(EvidenceLevel::kL1);
  facts::AnalysisFact* certificate = CheckCertificate(&request);
  ASSERT_NE(certificate, nullptr);
  // The budget cell is edited after publication: the row no longer re-derives
  // from its own cells, so the certificate fails its own identity check.
  ASSERT_EQ(certificate->row.cells.size(), 9u);
  certificate->row.cells[2] = std::string("9,256,5,64,8");

  const Status status = BuildStatus(request);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kInvalidArgument);
  EXPECT_NE(status.message().find("does not re-derive"), std::string::npos);
}

TEST(EvidenceCaseBuilderTest, Bld007MismatchedScopeStaysOpen) {
  EvidenceBuildRequest request = PristineRequest(EvidenceLevel::kL1);
  request.input.dominating_checks.metadata.query_provenance_id =
      request.input.capacities.metadata.query_provenance_id;

  ExpectOpenCheck(BuildOrFail(request));
}

TEST(EvidenceCaseBuilderTest, Bld007MismatchedRunIsRefused) {
  EvidenceBuildRequest request = PristineRequest(EvidenceLevel::kL1);
  request.input.dominating_checks.metadata.analysis_run_id =
      EvidenceScenarioBuilder().Id(core::IdKind::kAnalysisRun, "another-run");

  // The run is carried four ways and the input must agree with itself, so a
  // result from another run is refused at the boundary — never silently
  // rebased onto this case and never read as an absence.
  const Status status = BuildStatus(request);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kInvalidArgument);
  EXPECT_NE(status.message().find("analysis runs"), std::string::npos);
}

TEST(EvidenceCaseBuilderTest, Bld007MissingSelectedWitnessStaysOpen) {
  EvidenceBuildRequest request = PristineRequest(EvidenceLevel::kL1);
  const core::StableId named =
      request.input.dominating_checks.metadata.query_provenance_id;

  auto* nodes = request.input.provenance.mutable_nodes();
  for (int index = nodes->size() - 1; index >= 0; --index) {
    if (nodes->Get(index).output_fact_id() == core::ToString(named)) {
      nodes->DeleteSubrange(index, 1);
    }
  }
  ASSERT_NE(CheckCertificate(&request), nullptr);

  ExpectOpenCheck(BuildOrFail(request));
}

// The one case that may derive an absence: complete, empty, correctly scoped,
// in this run, with a selected witness. Its provenance input is the completion
// certificate the absence was derived from — not a literal, and not the
// absence itself.
TEST(EvidenceCaseBuilderTest, Bld007CompleteEmptyQueryDerivesAnAbsence) {
  const EvidenceBuildRequest request = PristineRequest(EvidenceLevel::kL1);
  const EvidenceCase value = BuildOrFail(request);
  EXPECT_TRUE(RequireValidEvidenceCase(value).ok());

  const Fact* absence =
      FindFactWith(value, kAbsencePredicate, EpistemicState::kMustNot);
  ASSERT_NE(absence, nullptr);
  EXPECT_EQ(absence->producer, kClosedWorldProducerId);
  EXPECT_TRUE(absence->derived);

  const Provenance* record = FindProvenance(value, kClosedWorldAbsenceRuleId);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(*record->analysis_run_id, *value.program.analysis_run_id);

  // The certificate the absence was derived from is a declared fact member, and
  // it carries the M9 identity of the row the builder re-derived.
  ASSERT_EQ(record->input_fact_ids.size(), 1u);
  const std::vector<const Fact*> certificates =
      FindFacts(value, "query_completion");
  ASSERT_EQ(certificates.size(), 1u);
  EXPECT_EQ(record->input_fact_ids.front(), certificates.front()->id);
  EXPECT_EQ(record->rule, kClosedWorldAbsenceRuleId);
  EXPECT_EQ(*certificates.front()->stable_id,
            request.input.dominating_checks.metadata.query_provenance_id);
}

// --- BLD-008 -----------------------------------------------------------------

// L0 states only what its own question needs, and every member it withheld is
// named by an omission. A withheld slice is never represented by its absence
// alone.
TEST(EvidenceCaseBuilderTest, Bld008L0DeclaresAnOmissionForEveryRemovedMember) {
  const EvidenceCase l0 = BuildOrFail(PristineRequest(EvidenceLevel::kL0));
  const EvidenceCase l1 = BuildOrFail(PristineRequest(EvidenceLevel::kL1));
  EXPECT_TRUE(RequireValidEvidenceCase(l0).ok());
  EXPECT_EQ(l0.level, EvidenceLevel::kL0);

  const std::set<std::string> removed =
      Difference(AllMemberIds(l1), AllMemberIds(l0));
  EXPECT_FALSE(removed.empty()) << "the l0 projection withheld nothing";

  const std::vector<const Omission*> projection =
      FindOmissions(l0, "level_projection");
  EXPECT_EQ(projection.size(), removed.size());

  std::set<std::string> named;
  for (const Omission* omission : projection) {
    EXPECT_TRUE(omission->expandable);
    // The subject is the surviving claim — the member that carries the
    // statement "this was withheld". A subject naming a *removed* member would
    // dangle, and the claim is the one member the projection always keeps.
    EXPECT_TRUE(omission->subject == l0.primary_claim.id ||
                AllMemberIds(l0).count(omission->subject) != 0u)
        << "the omission's subject '" << omission->subject
        << "' is not a member the projected case declares";
    for (const std::string& member : removed) {
      if (omission->reason.find(member) != std::string::npos) {
        named.insert(member);
      }
    }
  }
  EXPECT_EQ(named, removed) << "a withheld member is not named by any omission";

  // L0 states the claim and nothing it cannot support: no constraint, no
  // expansion marker, and only the detail its own question reads.
  EXPECT_TRUE(l0.constraints.empty());
  EXPECT_TRUE(l0.summaries.empty());
}

// --- BLD-009 -----------------------------------------------------------------

// L2 retains L1's detail and adds an explicit marker for each expansion it
// could not perform; L1 carries no such marker, because it never claims the
// detail a marker would withhold.
TEST(EvidenceCaseBuilderTest, Bld009L2RetainsDetailAndMarksUnavailableExpansion) {
  const EvidenceCase l1 = BuildOrFail(PristineRequest(EvidenceLevel::kL1));
  const EvidenceCase l2 = BuildOrFail(PristineRequest(EvidenceLevel::kL2));
  EXPECT_TRUE(RequireValidEvidenceCase(l2).ok());

  EXPECT_TRUE(FindOmissions(l1, "summary_expansion").empty())
      << "l1 claims no detail it would have to withhold";

  ASSERT_EQ(l2.entities.size(), l1.entities.size());
  ASSERT_EQ(l2.edges.size(), l1.edges.size());
  ASSERT_EQ(l2.paths.size(), l1.paths.size());
  ASSERT_EQ(l2.facts.size(), l1.facts.size());
  ASSERT_EQ(l2.provenance.size(), l1.provenance.size());
  EXPECT_EQ(IdsOf(l2.facts), IdsOf(l1.facts));

  const std::vector<const Omission*> expansions =
      FindOmissions(l2, "summary_expansion");
  std::size_t functions = 0;
  for (const Entity& entity : l2.entities) {
    if (entity.kind == EntityKind::kFunction) ++functions;
  }
  EXPECT_EQ(expansions.size(), functions)
      << "an unexpanded function is not marked";
  for (const Omission* omission : expansions) {
    EXPECT_TRUE(omission->expandable);
    EXPECT_NE(AllMemberIds(l2).count(omission->subject), 0u);
  }
}

// --- BLD-010 -----------------------------------------------------------------

// A relation the mapper cannot lower is a typed failure, never a dropped fact:
// a silently omitted fact would leave the case claiming a completeness it does
// not have.
TEST(EvidenceCaseBuilderTest, Bld010UnsupportedRelationIsRejected) {
  EvidenceBuildRequest request = PristineRequest(EvidenceLevel::kL1);
  ASSERT_FALSE(request.input.query_completion_facts.empty());
  // A relation the fact sets exist to carry, placed in a bucket that must lower
  // every member it holds.
  request.input.ranges.facts.push_back(request.input.query_completion_facts[0]);

  const Status status = BuildStatus(request);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kInvalidArgument);
  EXPECT_NE(status.message().find("no built-in EIR predicate mapping"),
            std::string::npos);
}

// The run is carried four times over, and the case is bound to one. A handoff
// that mixes runs is rejected, never rebased onto whichever run was seen first.
TEST(EvidenceCaseBuilderTest, Bld010MixedAnalysisRunIsRejected) {
  const EvidenceScenarioBuilder scenario;
  EvidenceBuildRequest request = PristineRequest(EvidenceLevel::kL1);
  request.input.aliases.metadata.analysis_run_id =
      scenario.Id(core::IdKind::kAnalysisRun, "another-run");

  const Status status = BuildStatus(request);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kInvalidArgument);
  EXPECT_NE(status.message().find("analysis runs"), std::string::npos);
}

// An edge whose kind the model cannot carry is refused rather than dropped: a
// thinner graph would misrepresent the flow the query returned.
TEST(EvidenceCaseBuilderTest, Bld010UnrepresentableEdgeKindIsRejected) {
  EvidenceBuildRequest request = PristineRequest(EvidenceLevel::kL1);
  const cpg::CpgNode* decode = nullptr;
  const cpg::CpgNode* srcbuf = nullptr;
  for (const cpg::CpgNode& node : request.input.flow_slice.nodes) {
    if (node.label == "decode") decode = &node;
    if (node.label == "srcbuf") srcbuf = &node;
  }
  ASSERT_NE(decode, nullptr);
  ASSERT_NE(srcbuf, nullptr);
  request.input.flow_slice.edges.push_back(
      EvidenceScenarioBuilder().MakeEdge("contains:decode:srcbuf",
                                         cpg::EdgeKind::kContains,
                                         decode->node_id, srcbuf->node_id));

  const Status status = BuildStatus(request);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kInvalidArgument);
  EXPECT_NE(status.message().find("cannot represent"), std::string::npos);
}

// An expandable edge is a summary edge: the model requires it to name the
// summary reference whose expansion was withheld. The handoff carries no
// summary identity, so the builder refuses rather than naming an expansion it
// cannot show.
TEST(EvidenceCaseBuilderTest, Bld010ExpandableSummaryEdgeIsRejected) {
  EvidenceBuildRequest request = PristineRequest(EvidenceLevel::kL1);
  const cpg::CpgNode* decode = nullptr;
  const cpg::CpgNode* vendor = nullptr;
  for (const cpg::CpgNode& node : request.input.flow_slice.nodes) {
    if (node.label == "decode") decode = &node;
    if (node.label == "vendor_validate") vendor = &node;
  }
  ASSERT_NE(decode, nullptr);
  ASSERT_NE(vendor, nullptr);
  cpg::CpgEdge edge =
      EvidenceScenarioBuilder().MakeEdge("calls:decode:vendor_validate",
                                         cpg::EdgeKind::kCalls,
                                         decode->node_id, vendor->node_id);
  edge.expandable = true;
  request.input.flow_slice.edges.push_back(std::move(edge));

  const Status status = BuildStatus(request);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kInvalidArgument);
  EXPECT_NE(status.message().find("expandable summary edge"),
            std::string::npos);
}

// --- The L49 producer translation --------------------------------------------

// The M9 witness names its producer `evidence-query`, and a hyphen is outside
// the EIR-T `QualifiedId` alphabet. `Producer` was deliberately not widened
// (ruling L43), so the builder is the layer that names the producer in EIR's
// vocabulary: `evidence.query`, the spelling its sibling rule
// `evidence.query_completion.v1` already uses.
TEST(EvidenceCaseBuilderTest, L49TranslatesTheQueryProducerIntoEirSpelling) {
  EXPECT_EQ(TranslateProducer("evidence-query"), "evidence.query");
  EXPECT_EQ(TranslateProducer("evidence.query"), "evidence.query");
  EXPECT_EQ(TranslateProducer("analysis.value_range"), "analysis.value_range");
  EXPECT_EQ(TranslateProducer(""), "");

  const EvidenceCase value = BuildOrFail(PristineRequest(EvidenceLevel::kL1));
  bool translated = false;
  for (const Provenance& record : value.provenance) {
    EXPECT_EQ(record.producer.find('-'), std::string::npos)
        << "provenance '" << record.id << "' carries a producer EIR-T cannot "
        << "spell: " << record.producer;
    EXPECT_EQ(record.rule.find('-'), std::string::npos);
    if (record.producer == kEvidenceQueryProducerId) {
      translated = true;
    }
    EXPECT_NE(record.producer, "evidence-query");
  }
  EXPECT_TRUE(translated)
      << "the case records no query-completion provenance at all";

  // The rule the witness carried rides through untranslated: it is the rule,
  // not the producer, and it never contained a hyphen.
  const Provenance* query =
      FindProvenance(value, std::string(kQueryCompletionRuleId));
  ASSERT_NE(query, nullptr);
  EXPECT_EQ(query->producer, kEvidenceQueryProducerId);
  EXPECT_EQ(query->source_anchor_id,
            std::string());  // the witness's anchor, which is empty
}

// --- L52: the difference from the hand-authored fixture ----------------------

// Compares the two cases family by family and returns the families that
// differ. Every model record in this header declares a defaulted
// `operator<=>`, so each comparison below is a *complete* content comparison —
// every field of every member, not just the handle it is declared under. A
// family is named here only when its content genuinely differs.
std::vector<std::string> DifferingFamilies(const EvidenceCase& built,
                                           const EvidenceCase& fixture) {
  std::vector<std::string> differing;
  auto note = [&differing](std::string name, bool same) {
    if (!same) differing.push_back(std::move(name));
  };

  note("program", built.program == fixture.program);
  note("primary_claim", built.primary_claim == fixture.primary_claim);
  note("level", built.level == fixture.level);
  note("schema_version", built.schema_version == fixture.schema_version);
  note("verification_state",
       built.verification_state == fixture.verification_state);

  note("entities", built.entities == fixture.entities);
  note("edges", built.edges == fixture.edges);
  note("paths", built.paths == fixture.paths);
  note("facts", built.facts == fixture.facts);
  note("assumptions", built.assumptions == fixture.assumptions);
  note("hypotheses", built.hypotheses == fixture.hypotheses);
  note("unknowns", built.unknowns == fixture.unknowns);
  note("constraints", built.constraints == fixture.constraints);
  note("provenance", built.provenance == fixture.provenance);
  note("proof_obligations",
       built.proof_obligations == fixture.proof_obligations);
  note("summaries", built.summaries == fixture.summaries);
  note("dependencies", built.dependencies == fixture.dependencies);
  note("omissions", built.omissions == fixture.omissions);

  // Sorted, so the comparison below is a set comparison over a stable order and
  // a failure names the families rather than their declaration order.
  std::sort(differing.begin(), differing.end());
  return differing;
}

// One expression in the fixture golden's own spelling, so a content pin below
// reads as the statement the case makes rather than as an operand walk. It is
// deliberately local to this file: the assertions it feeds are about what these
// two cases say, and the golden is written in this spelling.
std::string ExpressionText(const Expression& value) {
  auto joined = [&value](std::string_view separator) {
    std::string out;
    for (std::size_t i = 0; i < value.operands.size(); ++i) {
      if (i != 0) out += separator;
      out += ExpressionText(value.operands[i]);
    }
    return out;
  };
  switch (value.kind) {
    case Expression::Kind::kBool:
      return value.boolean ? "true" : "false";
    case Expression::Kind::kInteger:
      return std::to_string(value.integer);
    case Expression::Kind::kString:
      return "\"" + value.text + "\"";
    case Expression::Kind::kSymbol:
      return value.text;
    case Expression::Kind::kReference:
      return "@" + value.text;
    case Expression::Kind::kCall:
      return value.text + "(" + joined(", ") + ")";
    case Expression::Kind::kNot:
      return "!" + joined(", ");
    case Expression::Kind::kCompare:
      return joined(" " + value.text + " ");
    case Expression::Kind::kAnd:
      return "(" + joined(" and ") + ")";
    case Expression::Kind::kOr:
      return "(" + joined(" or ") + ")";
    case Expression::Kind::kImplies:
      return "(" + joined(" => ") + ")";
    case Expression::Kind::kForAll:
      return "forall " + value.text + ". " + joined(", ");
    case Expression::Kind::kExists:
      return "exists " + value.text + ". " + joined(", ");
    case Expression::Kind::kUnspecified:
      break;
  }
  return "<unspecified>";
}

// Ruling L52: the builder must reproduce `BindProgram`'s mapping for every
// binding, and the case it builds must be traceable to the fixture at exactly
// one point — the L49 producer translation.
//
// The program binding is asserted field by field, not as an aggregate, so a
// disagreement names the field. It is *identical*, which is the whole of L52's
// first half: the builder and the fixture agree on all eight bindings,
// `analyzer_versions` included.
//
// The second half is stated rather than asserted as a whole-case inequality:
// the families that differ are enumerated below, each one named and each one
// accounted for. The producer translation is asserted exactly and separately,
// because it is the point the two cases exist to differ at.
TEST(EvidenceCaseBuilderTest, L52ReproducesTheProgramBindingAndNamesItsOnePoint) {
  const EvidenceCase fixture = testing::MakeOverflowEvidenceCase();
  const EvidenceBuildRequest request = TruncatedRequest(EvidenceLevel::kL1);
  const EvidenceCase built = BuildOrFail(request);

  EXPECT_TRUE(RequireValidEvidenceCase(built).ok());
  EXPECT_EQ(built.level, fixture.level);

  // --- L52, first half: every binding reproduced, field by field.
  EXPECT_EQ(built.program.repository_id, fixture.program.repository_id);
  EXPECT_EQ(built.program.revision_id, fixture.program.revision_id);
  EXPECT_EQ(built.program.build_variant_id, fixture.program.build_variant_id);
  EXPECT_EQ(built.program.target_triple, fixture.program.target_triple);
  EXPECT_EQ(built.program.analysis_configuration_id,
            fixture.program.analysis_configuration_id);
  EXPECT_EQ(built.program.type_layout_id, fixture.program.type_layout_id);
  EXPECT_EQ(built.program.analysis_run_id, fixture.program.analysis_run_id);
  EXPECT_EQ(built.program.analyzer_versions,
            fixture.program.analyzer_versions);

  // --- The one point: the producer translation, named.
  //
  // The fixture carries the M9 witness identifier verbatim
  // (`evidence::kQueryCompletionProducerId`, which is `evidence-query`). The
  // builder carries its EIR spelling. `TranslateProducer` is the relation
  // between them, so the difference is a translation and not a substitution:
  // applying it to the fixture's producer yields the builder's, exactly.
  const Provenance* fixture_query = nullptr;
  const Provenance* built_query = nullptr;
  for (const Provenance& record : fixture.provenance) {
    if (record.rule == std::string(kQueryCompletionRuleId)) {
      fixture_query = &record;
    }
  }
  for (const Provenance& record : built.provenance) {
    if (record.rule == std::string(kQueryCompletionRuleId)) {
      built_query = &record;
    }
  }
  ASSERT_NE(fixture_query, nullptr);
  ASSERT_NE(built_query, nullptr);
  EXPECT_EQ(fixture_query->producer, kQueryCompletionProducerId);
  EXPECT_EQ(built_query->producer, kEvidenceQueryProducerId);
  EXPECT_EQ(TranslateProducer(fixture_query->producer), built_query->producer);
  EXPECT_FALSE(IsWritableProducer(fixture_query->producer))
      << "the fixture's producer is already EIR-T-writable, so the translation "
         "would be a no-op and this test would prove nothing";
  EXPECT_TRUE(IsWritableProducer(built_query->producer));

  // --- L52, second half: every other family that differs, named.
  //
  // The residual is *not* empty, and the exact statement of it is the point of
  // this test. Each entry is accounted for by what the handoff cannot carry:
  //
  //   * entities — the builder declares one entity the fixture does not: the
  //     M8R.2 memory slot the range row actually keys on
  //     (`E_memory_object_46d5ae3e`). The fixture states the same range window
  //     over the value handle `E_copy_length` and declares no memory-object
  //     entity at all, so the two cases carry the window under different
  //     subjects.
  //   * edges — the fixture marks the call into `vendor_validate` expandable
  //     and gives it a `summarized_by` summary reference. The handoff carries
  //     no summary identity, so the builder refuses to name an expansion it
  //     cannot show and the edge is not expandable.
  //   * facts — both cases state **three**, and they differ in content, not in
  //     count. The count is three because this test builds the *truncated*
  //     request, whose dominating-check result is withheld, so the builder
  //     derives no closed-world pair and adds no fact; a complete request does
  //     reach five, the three plus the completion certificate and the absence
  //     derived from it. What differs here is the range fact's subject — the
  //     builder's memory slot against the fixture's `E_copy_length`, as above —
  //     and the alias fact's handle (`F_alias` against `F_alias_may`), the
  //     statement itself being identical in both.
  //   * paths — one path in both, and it differs in two fields: `feasibility`
  //     and `provenance_id`. Neither difference is a spelling. See below.
  //   * constraints — the fixture carries `K_value_within_capacity` as a MUST
  //     justified by a specification record; the builder carries
  //     `K_path_safety` as a MAY justified by the query witness. Handle,
  //     epistemic state, and provenance all differ.
  //   * unknowns — the fixture states two: a truncated dominating-check unknown
  //     over `dominates(@E_vendor_validate, @E_memcpy)` and the
  //     vendor-validate postcondition unknown. The builder states three: the
  //     two function-effect unknowns the handoff's unknown slot carries, plus
  //     the truncated dominating-check unknown over the sink. The content of
  //     those three is pinned by
  //     `UnknownsFamilyPinsItsPropertyHandles`, not by this list.
  //   * omissions — both state three, and they are different three: the fixture
  //     carries a `summary_expansion` (recoverable, its summary reference) that
  //     the builder cannot emit for want of a summary identity, and its
  //     truncated-query omission names the fixture's unknown.
  //   * provenance — the fixture hand-authors eight records with per-family
  //     producers and rules (`analysis.value_range` / `range.known.v1`) and
  //     `anchor:*` source anchors; the builder derives six, each carrying the
  //     producer and rule of the M9 witness it was handed and no anchor. (That
  //     the builder's records all carry one producer is the design question
  //     carried forward separately; this test pins the count and the path
  //     record's attribution, not every record's.)
  //   * proof_obligations — one `prove` obligation, PENDING in both. Its goal
  //     differs where the entity handles do: the fixture quantifies over
  //     `feasible_paths(@E_entry, ...)` and the builder over
  //     `feasible_paths(@E_srcbuf, ...)`, the first member of the value-flow
  //     chain it was handed.
  //   * dependencies — the fixture carries five, the builder three: the summary
  //     and specification dependencies have no source in the handoff.
  //   * summaries, assumptions, hypotheses — one each in the fixture, none in
  //     the builder. The handoff carries no summary identity, no assumption, and
  //     no hypothesis, and the builder invents none of them.
  EXPECT_EQ(DifferingFamilies(built, fixture),
            (std::vector<std::string>{
                "assumptions",   "constraints", "dependencies", "edges",
                "entities",      "facts",       "hypotheses",   "omissions",
                "paths",         "proof_obligations", "provenance", "summaries",
                "unknowns"}));

  // The families that do *not* differ are the load-bearing ones: the claim, the
  // graph's identity space, and the path's spine.
  EXPECT_EQ(IdsOf(built.paths), IdsOf(fixture.paths));
  EXPECT_EQ(built.paths.front().entity_ids, fixture.paths.front().entity_ids);
  EXPECT_EQ(built.paths.front().kind, fixture.paths.front().kind);
  EXPECT_EQ(built.primary_claim.subject, fixture.primary_claim.subject);
  EXPECT_EQ(built.primary_claim.predicate, fixture.primary_claim.predicate);

  // The path differs in exactly two fields. Neither is a cosmetic spelling.
  //
  // `feasibility` is the first: the fixture is a hand-authored model instance
  // and says `SAT`, while the builder says `UNKNOWN`, because the M10B handoff
  // carries no feasibility for a path and `SAT` is reserved for a path an
  // SMT/symbolic model was found for (EIR architecture §22). Stating `SAT` here
  // would be the builder asserting a result nothing gave it; the model admits
  // `UNKNOWN`, and the validator accepts it.
  EXPECT_EQ(fixture.paths.front().feasibility, Feasibility::kSat);
  EXPECT_EQ(built.paths.front().feasibility, Feasibility::kUnknown)
      << "the builder asserted a feasibility the handoff never carried";

  // `provenance_id` is the second: `PR_value_flow` against `PR_flow`. Those are
  // handles, but the two handles do not name equivalent records:
  //
  //   * `PR_flow` (fixture) — producer `analysis.value_flow`, rule
  //     `value_flow.interprocedural.v1`, anchor `anchor:memcpy`
  //   * `PR_value_flow` (builder) — producer `evidence.query`, rule
  //     `evidence.query_completion.v1`, no anchor
  //
  // Both the producer and the rule differ, and the reason is structural: the
  // M10B handoff's only provenance for the facts it carries is the query
  // witness, so the builder attributes every record it derives to that witness
  // and has no `analysis.value_flow` record to point at. Whether the builder's
  // attribution or the fixture's is the better one is the design question
  // carried forward separately; what this test pins is that the difference is
  // an attribution and not a renaming.
  //
  // Everything the path *states* about the flow is identical, and the
  // load-bearing part is the condition: an alias the analysis could only
  // establish as MAY is carried as a condition on the path that depends on it,
  // never promoted to a premise (BLD-005). Both cases say
  // `alias(@E_srcbuf, @E_dstbuf)`.
  ASSERT_EQ(built.paths.front().conditions.size(), 1u);
  EXPECT_EQ(built.paths.front().conditions, fixture.paths.front().conditions);
  EXPECT_EQ(built.paths.front().provenance_id, "PR_value_flow");
  EXPECT_EQ(fixture.paths.front().provenance_id, "PR_flow");

  const Provenance* fixture_path_record = FindProvenanceById(fixture, "PR_flow");
  const Provenance* built_path_record =
      FindProvenanceById(built, "PR_value_flow");
  ASSERT_NE(fixture_path_record, nullptr);
  ASSERT_NE(built_path_record, nullptr);
  EXPECT_EQ(fixture_path_record->producer, "analysis.value_flow");
  EXPECT_EQ(fixture_path_record->rule, "value_flow.interprocedural.v1");
  EXPECT_EQ(built_path_record->producer, std::string(kEvidenceQueryProducerId));
  EXPECT_EQ(built_path_record->rule, std::string(kQueryCompletionRuleId));
  EXPECT_NE(fixture_path_record->producer, built_path_record->producer)
      << "the two records agree on their producer, so the handle difference "
         "would be a spelling after all";
  EXPECT_NE(fixture_path_record->rule, built_path_record->rule);

  // --- F8: the residual above names *which* families differ, never how their
  // members differ, so a value changed inside a listed-differing family would
  // stay green — which is exactly what the three facts of this case did before
  // this block existed. These pins are the missing half: they state the built
  // members' content exactly, so a within-family value error is as visible as a
  // new family would be.
  ASSERT_EQ(built.facts.size(), 3u);
  auto fact_text = [&built](std::string_view id) {
    for (const Fact& fact : built.facts) {
      if (fact.id == id) {
        return ExpressionText(fact.predicate);
      }
    }
    return std::string("<no such fact>");
  };
  EXPECT_EQ(fact_text("F_range"), "range(@E_memory_object_46d5ae3e, 0, 65535)")
      << "the range fact's subject or its window bounds moved";
  EXPECT_EQ(fact_text("F_capacity"), "capacity(@E_dstbuf, 2048)");
  EXPECT_EQ(fact_text("F_alias"), "alias(@E_srcbuf, @E_dstbuf)");

  // The alias fact is the one the case could weaken: it is the MAY premise the
  // whole path condition rests on, so its state is pinned as content too.
  for (const Fact& fact : built.facts) {
    if (fact.id != "F_alias") {
      continue;
    }
    EXPECT_EQ(fact.epistemic, EpistemicState::kMay)
        << "the alias premise was strengthened past the MAY the analysis "
           "established (BLD-005)";
    EXPECT_TRUE(fact.derived);
  }

  // The path family's own content: one path, one condition, the feasibility the
  // builder is entitled to state, and the record it points at. `IdsOf` above
  // only says the two cases agree on the handle.
  ASSERT_EQ(built.paths.size(), 1u);
  EXPECT_EQ(built.paths.front().id, "P_value_flow");
  EXPECT_EQ(built.paths.front().provenance_id, "PR_value_flow");
  EXPECT_EQ(built.paths.front().feasibility, Feasibility::kUnknown);

  // The provenance family is listed as differing, so its count is pinned here
  // too: six records, all attributed to the one witness the handoff carried.
  EXPECT_EQ(built.provenance.size(), 6u);
  for (const Provenance& record : built.provenance) {
    EXPECT_EQ(record.producer, std::string(kEvidenceQueryProducerId))
        << "record '" << record.id << "' is attributed to a producer the "
        << "handoff never carried";
    EXPECT_EQ(record.rule, std::string(kQueryCompletionRuleId));
    EXPECT_TRUE(record.source_anchor_id.empty())
        << "record '" << record.id << "' names a source anchor the handoff "
        << "never carried";
  }
}

// --- The dominating-check absence's two handles -------------------------------

// BLD-003 and BLD-007. The dominating-check query is issued over an *ordered*
// ref list, and the absence the builder derives from a complete-empty result is
// binary: `dominates_bounds_check(scope, sink)`. The scope is the query's own
// list head and the sink is the claim's, so when the query is scoped to an
// enclosing entity the two operands are different handles. Naming the sink
// twice regardless would understate what the query actually covered — and would
// silently agree with the demo request, whose scope really is the sink alone,
// which is why this test also builds that one.
//
// The operand order is load-bearing in the other direction too: the scope must
// be the *query's* head, not the claim's source. It happens that a scope of
// `{source, sink}` makes those coincide here, so the test states the query's
// head explicitly and leaves the claim's source as the value it was given.
TEST(EvidenceCaseBuilderTest, Bld007ScopeIsTheQuerysHeadAndTheSinkIsItsSubject) {
  EvidenceScenarioBuilder scenario;
  const EvidenceBuildRequest defaults =
      scenario.BuildRequest(EvidenceLevel::kL1);
  const core::StableId source = defaults.input.claim_seed.source_ref;
  const core::StableId sink = defaults.input.claim_seed.sink_ref;
  ASSERT_NE(source, sink) << "the fixture's source and sink are one handle, so "
                             "this test could not tell them apart";

  // The default scope: the sink alone, as the M10B producer issues it.
  const EvidenceCase sink_scoped = BuildOrFail(defaults);
  const std::vector<const Fact*> sink_facts =
      FindFacts(sink_scoped, kAbsencePredicate);
  ASSERT_EQ(sink_facts.size(), 1u);
  EXPECT_EQ(ExpressionText(sink_facts.front()->predicate),
            "dominates_bounds_check(@E_memcpy, @E_memcpy)")
      << "the sink-scoped query no longer states the sink twice, so the "
         "enclosing-scoped assertion below would prove nothing";

  // The enclosing scope: the source first, the sink still among the refs.
  // Built twice, because the builder emits this predicate from two places and
  // both must name the query's scope: BLD-007 derives the closed-world absence
  // from a complete-*empty* result, and BLD-002 records a check it *found* as
  // counterevidence. The demo request reaches the first; the second needs the
  // query to return something, which is what `WithDominatingCheckFound` asks
  // for. A test that built only the demo request would leave BLD-002's operand
  // free to regress to the sink unnoticed.
  for (bool check_found : {false, true}) {
    EvidenceScenarioBuilder scoped;
    if (check_found) {
      scoped.WithDominatingCheckFound();
    }
    scoped.WithDominatingCheckScope({source, sink});
    const EvidenceCase enclosing_scoped =
        BuildOrFail(scoped.BuildRequest(EvidenceLevel::kL1));
    EXPECT_TRUE(RequireValidEvidenceCase(enclosing_scoped).ok());

    const std::vector<const Fact*> enclosing_facts =
        FindFacts(enclosing_scoped, kAbsencePredicate);
    ASSERT_EQ(enclosing_facts.size(), 1u)
        << (check_found ? "the found-check path emitted no coverage fact"
                        : "the closed-world path emitted no absence fact");
    EXPECT_EQ(ExpressionText(enclosing_facts.front()->predicate),
              "dominates_bounds_check(@E_copy_length, @E_memcpy)")
        << (check_found
                ? "the recorded check named the sink twice instead of the "
                  "scope the query was issued over"
                : "the derived absence named the sink twice instead of the "
                  "scope the query was issued over");
    EXPECT_NE(enclosing_facts.front()->predicate.operands[0].text,
              enclosing_facts.front()->predicate.operands[1].text)
        << "the two operands are the same handle, so the fact still states the "
           "sink twice";
  }
}

// --- The unknowns family -----------------------------------------------------

// F8, generalized past the one family the first round pinned.
//
// `DifferingFamilies` compares family *equality*: it can see that `unknowns`
// differs, never what a member of it says. The first round added content pins
// for `facts` and left `unknowns` on the set-only assertion, so the reviewer
// could change the truncated branch's unknown property to a different predicate
// and the whole suite stayed green. The surface that slipped through is exactly
// the one F3 decided — which handles the `dominates_bounds_check` property names
// — so the pin below states every member's content, not the family's presence.
//
// The truncated branch keeps the sink-scoped `(sink, sink)` spelling — the query
// it reports on was issued over the sink alone — and the pin below fixes that
// value, so a move to any other handle reddens it. What the pin *cannot* do is
// tell that spelling apart from `(scope, sink)` in this fixture: both resolve to
// the same handle here. The note at the operand assertions states that limit in
// full.
TEST(EvidenceCaseBuilderTest, UnknownsFamilyPinsItsPropertyHandles) {
  const EvidenceCase value = BuildOrFail(TruncatedRequest(EvidenceLevel::kL1));
  ASSERT_TRUE(RequireValidEvidenceCase(value).ok());

  // Every member, in declaration order: its handle, the property it states with
  // its operand handles spelled out, and the reason code that justifies it. The
  // order is part of the pin, so a reordering within the family is visible too.
  //
  // Two of these spellings have no spec basis, and the pin is a change-detector
  // for them rather than a conformance check: `effect_of` appears in no document
  // in this repository — it is invented by the builder's `AddUnknown` — and the
  // `U_<subject>_<CODE>` handle scheme is the allocator's, while the
  // hand-authored DEM-001 case spells the analogous member
  // `postcondition(@vendor_validate)`. Renaming either is a free choice rather
  // than a violation: this pin exists so a rename is noticed, not so it is
  // forbidden.
  std::vector<std::string> stated;
  for (const Unknown& unknown : value.unknowns) {
    stated.push_back(unknown.id + " = " + ExpressionText(unknown.property) +
                     " (" + std::string(ToString(unknown.reason_code)) + ")");
  }
  EXPECT_EQ(stated,
            (std::vector<std::string>{
                "U_decode_EXTERNAL_FUNCTION = effect_of(@E_decode) "
                "(EXTERNAL_FUNCTION)",
                "U_decode_UNRESOLVED_CALL = effect_of(@E_decode) "
                "(UNRESOLVED_CALL)",
                "U_memcpy_missing_specification = dominates_bounds_check("
                "@E_memcpy, @E_memcpy) (MISSING_SPECIFICATION)"}));

  // The dominating-check unknown is the one whose operands F3 turned into a
  // decision, so its two handles are named individually as well: the rendered
  // string above would still read as a two-operand predicate if either operand
  // were swapped for its sibling.
  //
  // The limit of those two assertions, stated so a later reader does not
  // overread them: in this fixture the query's scope *is* the sink, so both
  // operands resolve to the same handle and a change that swaps one for the
  // other is value-preserving here and stays green. That is not a gap in the pin
  // — the pin fixes the value, and a change to any other handle reddens it — it
  // is a limit of the value this pin fixes.
  //
  // The question it leaves unposed is whether the truncated branch should name
  // an enclosing scope when there is one. This pin does not pose it, and
  // `TruncatedRequest` cannot — the question is not unposable: the scenario
  // builder reaches that handoff by combining `WithTruncatedDominatingChecks`
  // with `WithDominatingCheckScope`. Doing so is deliberately left out of this
  // pin, and the behaviour such a case would expose is Task 12's to decide.
  const Unknown* check_unknown = nullptr;
  for (const Unknown& unknown : value.unknowns) {
    if (unknown.reason_code == UnknownReasonCode::kMissingSpecification) {
      check_unknown = &unknown;
    }
  }
  ASSERT_NE(check_unknown, nullptr)
      << "the truncated request no longer states a missing-specification "
         "unknown, so this test no longer covers the F3 surface";
  EXPECT_EQ(check_unknown->property.text, std::string(kAbsencePredicate));
  ASSERT_EQ(check_unknown->property.operands.size(), 2u);
  EXPECT_EQ(check_unknown->property.operands[0].text, "E_memcpy");
  EXPECT_EQ(check_unknown->property.operands[1].text, "E_memcpy");
  // The recovery target is the query's own scope, as a bare handle: the `@` is
  // EIR-T spelling and does not live in the model's text.
  EXPECT_EQ(check_unknown->suggested_resolution,
            "expand_dominating_check_query(E_memcpy)");
}

// --- The path's feasibility --------------------------------------------------

// F5. The M10B handoff carries no feasibility for a path, so the builder has
// nothing to copy: it states `UNKNOWN`, the model's open value. `SAT` is
// reserved for a path an SMT or symbolic model was found for (EIR architecture
// §22) and this layer runs no solver, so asserting it would be the builder
// inventing a result.
//
// The other half of the same choice: `UNSPECIFIED` is not the honest value
// either. It is the model's *no-value* sentinel, and the validator rejects a
// path that carries it, which is why the builder cannot simply decline to
// answer. This test pins both halves.
TEST(EvidenceCaseBuilderTest, PathFeasibilityIsUnknownAndNoValueIsRejected) {
  EvidenceCase value = BuildOrFail(PristineRequest(EvidenceLevel::kL1));
  ASSERT_EQ(value.paths.size(), 1u);
  EXPECT_EQ(value.paths.front().feasibility, Feasibility::kUnknown)
      << "the builder stated a feasibility the handoff never carried";
  EXPECT_NE(value.paths.front().feasibility, Feasibility::kSat);
  EXPECT_TRUE(RequireValidEvidenceCase(value).ok());

  value.paths.front().feasibility = Feasibility::kUnspecified;
  const Status status = RequireValidEvidenceCase(value);
  EXPECT_FALSE(status.ok())
      << "the validator accepted a path that declares no feasibility at all, "
         "so declining to answer would have been available to the builder";
  EXPECT_EQ(status.code(), StatusCode::kInvalidArgument);
  EXPECT_NE(status.message().find("declares no feasibility"), std::string::npos)
      << "the refusal did not come from the missing-feasibility rule: "
      << status.message();
}

// --- The analyzer list --------------------------------------------------------

// F7. `EvidenceBuildRequest::analyzer_versions` may be empty, and the binding
// says so rather than refusing: M10C reports the analyzers it was told about
// and does not synthesize a set. An input taken from a snapshot that recorded
// no analyzer identity is still a valid case.
TEST(EvidenceCaseBuilderTest, AnEmptyAnalyzerListIsCarriedNotRefused) {
  EvidenceBuildRequest request = PristineRequest(EvidenceLevel::kL1);
  ASSERT_FALSE(request.analyzer_versions.empty())
      << "the fixture no longer carries analyzers, so this test could not tell "
         "an empty list from a dropped one";
  request.analyzer_versions.clear();

  const EvidenceCase value = BuildOrFail(request);
  EXPECT_TRUE(value.program.analyzer_versions.empty());
  EXPECT_EQ(value.program.analyzer_versions,
            (std::vector<AnalyzerVersion>{}));
  EXPECT_TRUE(RequireValidEvidenceCase(value).ok())
      << "a case bound to a snapshot with no recorded analyzer identity was "
      << "refused";
}

// --- Determinism -------------------------------------------------------------

// Reverses every input vector the handoff carries. The builder derives handles
// from sorted stable-ID sequences, so the insertion order of any input must not
// reach the output.
void ReverseAllInputs(EvidenceBuildRequest* request) {
  EvidenceBuildInput& input = request->input;
  EvidenceScenarioBuilder::Reverse(input.flow_slice.nodes);
  EvidenceScenarioBuilder::Reverse(input.flow_slice.edges);
  EvidenceScenarioBuilder::Reverse(input.flow_slice.supporting_facts);
  EvidenceScenarioBuilder::Reverse(input.flow_slice.contradicting_facts);
  EvidenceScenarioBuilder::Reverse(input.flow_slice.unknowns);
  EvidenceScenarioBuilder::Reverse(input.flow_slice.provenance_refs);
  EvidenceScenarioBuilder::Reverse(input.ranges.facts);
  EvidenceScenarioBuilder::Reverse(input.capacities.facts);
  EvidenceScenarioBuilder::Reverse(input.aliases.facts);
  EvidenceScenarioBuilder::Reverse(input.dominating_checks.facts);
  EvidenceScenarioBuilder::Reverse(input.unknowns.facts);
  EvidenceScenarioBuilder::Reverse(input.query_completion_facts);
  EvidenceScenarioBuilder::Reverse(input.query_completion_bindings);

  auto* nodes = input.provenance.mutable_nodes();
  std::vector<fact::v1::FactWitness> witnesses(nodes->begin(), nodes->end());
  std::reverse(witnesses.begin(), witnesses.end());
  nodes->Clear();
  for (const fact::v1::FactWitness& witness : witnesses) {
    *nodes->Add() = witness;
  }
}

TEST(EvidenceCaseBuilderTest, ReversingEveryInputVectorYieldsTheSameIdentity) {
  for (EvidenceLevel level :
       {EvidenceLevel::kL0, EvidenceLevel::kL1, EvidenceLevel::kL2}) {
    const EvidenceBuildRequest forward = PristineRequest(level);
    EvidenceBuildRequest reversed = forward;
    ReverseAllInputs(&reversed);

    const EvidenceCase a = BuildOrFail(forward);
    const EvidenceCase b = BuildOrFail(reversed);

    auto bytes_a = CanonicalEvidenceBytes(a);
    auto bytes_b = CanonicalEvidenceBytes(b);
    ASSERT_TRUE(bytes_a.ok()) << bytes_a.status().message();
    ASSERT_TRUE(bytes_b.ok()) << bytes_b.status().message();
    EXPECT_EQ(bytes_a.value(), bytes_b.value())
        << "level " << static_cast<int>(level)
        << " depends on input insertion order";
    EXPECT_EQ(a.evidence_id, b.evidence_id);
  }
}

}  // namespace
}  // namespace veritas::evidence
