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

#include "evidence/EvidenceScenario.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "veritas/evidence/EvidenceQueryService.h"
#include "veritas/facts/FactProto.h"

namespace veritas::testing {

namespace {

namespace sem = veritas::analysis::semantic;
namespace ev = veritas::evidence;

std::string Suffixed(std::string_view name, std::string_view suffix) {
  std::string out(name);
  out += suffix;
  return out;
}

// Builds a fact from a known-valid row. Test-support setup failures abort
// rather than throw (the project builds with exceptions disabled).
veritas::facts::AnalysisFact MakeFactOrAbort(
    veritas::facts::SemanticRow row) {
  auto fact = veritas::facts::MakeFact(std::move(row));
  if (!fact.ok()) {
    std::abort();
  }
  return std::move(fact).value();
}

core::IdKind NodeIdKind(veritas::cpg::NodeKind kind) {
  switch (kind) {
    case veritas::cpg::NodeKind::kFunction:
      return core::IdKind::kFunctionVariant;
    case veritas::cpg::NodeKind::kParameter:
      return core::IdKind::kValueRef;
    case veritas::cpg::NodeKind::kGlobal:
    case veritas::cpg::NodeKind::kMemoryObject:
      return core::IdKind::kMemoryRef;
    case veritas::cpg::NodeKind::kCallSite:
      return core::IdKind::kCallSite;
    case veritas::cpg::NodeKind::kBasicBlockSummary:
      return core::IdKind::kBasicBlockSummary;
    case veritas::cpg::NodeKind::kSummary:
      return core::IdKind::kFunctionSummary;
    case veritas::cpg::NodeKind::kUnknown:
      return core::IdKind::kUnknownNode;
  }
  return core::IdKind::kUnknownNode;
}

}  // namespace

core::StableId EvidenceScenarioBuilder::Id(core::IdKind kind,
                                           std::string_view name) const {
  return core::MakeStableId(
      kind, std::as_bytes(std::span<const char>(name.data(), name.size())));
}

facts::AnalysisFact EvidenceScenarioBuilder::MakeAliasFact(
    std::string_view name, sem::AliasKind alias,
    sem::EpistemicState epistemic) const {
  facts::SemanticRow row;
  row.relation = facts::RelationId::kAlias;
  row.cells = {Id(core::IdKind::kMemoryRef, Suffixed(name, ":left")),
               Id(core::IdKind::kMemoryRef, Suffixed(name, ":right")), alias,
               epistemic};
  return MakeFactOrAbort(std::move(row));
}

facts::AnalysisFact EvidenceScenarioBuilder::MakeAliasFact(
    core::StableId left, core::StableId right, sem::AliasKind alias,
    sem::EpistemicState epistemic) const {
  facts::SemanticRow row;
  row.relation = facts::RelationId::kAlias;
  row.cells = {std::move(left), std::move(right), alias, epistemic};
  return MakeFactOrAbort(std::move(row));
}

facts::AnalysisFact EvidenceScenarioBuilder::MakeRangeFact(
    std::string_view name, std::int64_t offset, std::uint64_t size) const {
  facts::SemanticRow row;
  row.relation = facts::RelationId::kDirectRead;
  row.cells = {Id(core::IdKind::kFunctionVariant, Suffixed(name, ":fn")),
               Id(core::IdKind::kMemoryRef, Suffixed(name, ":mem")),
               sem::ByteRangeKind::kKnown, offset, size,
               sem::EpistemicState::kMust};
  return MakeFactOrAbort(std::move(row));
}

facts::AnalysisFact EvidenceScenarioBuilder::MakeCapacityFact(
    std::string_view name, std::uint64_t size) const {
  facts::SemanticRow row;
  row.relation = facts::RelationId::kDirectWrite;
  row.cells = {Id(core::IdKind::kFunctionVariant, Suffixed(name, ":fn")),
               Id(core::IdKind::kMemoryRef, Suffixed(name, ":mem")),
               sem::ByteRangeKind::kKnown, std::int64_t{0}, size,
               sem::EpistemicState::kMust};
  return MakeFactOrAbort(std::move(row));
}

facts::AnalysisFact EvidenceScenarioBuilder::MakeUnknownFact(
    std::string_view name, std::string_view reason) const {
  facts::SemanticRow row;
  row.relation = facts::RelationId::kUnknownEffect;
  row.cells = {Id(core::IdKind::kFunctionVariant, Suffixed(name, ":fn")),
               std::string(name), std::string(reason),
               sem::EpistemicState::kUnknown};
  return MakeFactOrAbort(std::move(row));
}

facts::AnalysisFact EvidenceScenarioBuilder::MakeCheckFact(
    std::string_view name) const {
  facts::SemanticRow row;
  row.relation = facts::RelationId::kSoundnessCoverage;
  row.cells = {std::string(name), std::string("dominating_check"),
               std::uint64_t{1}, sem::EpistemicState::kMust};
  return MakeFactOrAbort(std::move(row));
}

StatusOr<facts::AnalysisFact> EvidenceScenarioBuilder::MakeCompletionFact(
    const ev::QueryCompletionDescriptor& descriptor) const {
  return ev::MakeQueryCompletionFact(descriptor);
}

facts::RunFactBinding EvidenceScenarioBuilder::MakeBinding(
    core::StableId run_id, core::StableId fact_id,
    std::string_view witness_id) const {
  facts::RunFactBinding binding;
  binding.run_id = run_id;
  binding.fact_id = fact_id;
  binding.producer_kind = facts::ProducerKind::kWpaSouffle;
  binding.analyzer_run_id = "scenario";
  binding.scope_kind = "scope";
  binding.scope_id = "scenario";
  binding.selected_witness_id = std::string(witness_id);
  binding.is_current = true;
  return binding;
}

facts::FactWitness EvidenceScenarioBuilder::MakeWitness(
    core::StableId run_id, core::StableId fact_id,
    std::string_view witness_id) const {
  facts::FactWitness witness;
  witness.run_id = run_id;
  witness.output_fact_id = fact_id;
  witness.witness_id = std::string(witness_id);
  witness.selected = true;
  witness.producer_kind = facts::ProducerKind::kWpaSouffle;
  witness.producer_id = "evidence-query";
  witness.rule_id = "evidence.query_completion.v1";
  witness.rule_version = "v1";
  witness.analyzer_run_id = "scenario";
  return witness;
}

cpg::CpgNode EvidenceScenarioBuilder::MakeNode(std::string_view name,
                                               cpg::NodeKind kind) const {
  cpg::CpgNode node;
  node.node_id = Id(NodeIdKind(kind), name);
  node.kind = kind;
  node.label = std::string(name);
  return node;
}

cpg::CpgEdge EvidenceScenarioBuilder::MakeEdge(std::string_view name,
                                               cpg::EdgeKind kind,
                                               core::StableId source,
                                               core::StableId target) const {
  cpg::CpgEdge edge;
  edge.edge_id = Id(core::IdKind::kCpgEdge, name);
  edge.kind = kind;
  edge.source_node_id = source;
  edge.target_node_id = target;
  edge.expandable = false;
  return edge;
}

// --- M10C semantic-case fixtures --------------------------------------------

namespace {

namespace fp = veritas::fact::v1;

// The demo program identity. Every value is a deterministic literal: no fixture
// reads the environment, the clock, or a checkout path, and the M10B
// `ProgramContext.project_root` never enters a case.
constexpr std::string_view kRepositoryId = "radio-stack";
constexpr std::string_view kRevisionId = "a87f03e";
constexpr std::string_view kBuildVariantId = "ARM64_RELEASE";
constexpr std::string_view kTargetTriple = "aarch64-unknown-linux-gnu";
constexpr std::string_view kTypeLayoutId = "layout:aapcs64";
constexpr std::string_view kAnalysisConfigurationId =
    "veritas.analysis.default.v1";
constexpr std::string_view kProjectRoot = "/checkout/radio-stack";
constexpr std::string_view kRunName = "demo-run";
constexpr std::string_view kSnapshotFingerprint = "veritas-evidence-demo.v1";

// The demo's CPG labels. A case-local entity ID is `E_<label>` and an edge ID is
// `ED_<from>_<to>`: the labels are EIR-T Identifiers, so the projection from the
// typed flow slice stays readable in diagnostics and goldens.
constexpr std::string_view kDecodeLabel = "decode";
constexpr std::string_view kMemcpyLabel = "memcpy";
constexpr std::string_view kSrcBufLabel = "srcbuf";
constexpr std::string_view kDstBufLabel = "dstbuf";
constexpr std::string_view kCopyLengthLabel = "copy_length";
constexpr std::string_view kEntryLabel = "entry";
constexpr std::string_view kVendorValidateLabel = "vendor_validate";

// The fixture's stable member handles.
constexpr std::string_view kClaimId = "C1";
constexpr std::string_view kFactRangeId = "F_range";
constexpr std::string_view kFactCapacityId = "F_capacity";
constexpr std::string_view kFactAliasId = "F_alias_may";
constexpr std::string_view kPathValueFlowId = "P_value_flow";
constexpr std::string_view kAssumptionId = "A_attacker_controlled_length";
constexpr std::string_view kHypothesisId = "H_vendor_validate_bounds";
constexpr std::string_view kUnknownCheckId = "U_dominating_check_truncated";
constexpr std::string_view kUnknownVendorId = "U_vendor_validate_contract";
constexpr std::string_view kConstraintId = "K_value_within_capacity";
constexpr std::string_view kObligationId = "O1";
constexpr std::string_view kSummaryId = "S_vendor_validate";
constexpr std::string_view kProvenanceRangeId = "PR_range";
constexpr std::string_view kProvenanceCapacityId = "PR_capacity";
constexpr std::string_view kProvenanceAliasId = "PR_alias";
constexpr std::string_view kProvenanceFlowId = "PR_flow";
constexpr std::string_view kProvenanceCallId = "PR_call";
constexpr std::string_view kProvenanceMemoryId = "PR_memory";
constexpr std::string_view kProvenanceQueryId = "PR_query";
constexpr std::string_view kProvenanceSpecificationId = "PR_specification";
constexpr std::string_view kDependencySummaryId = "DEP_summary";
constexpr std::string_view kDependencyFactId = "DEP_fact";
constexpr std::string_view kDependencyTypeLayoutId = "DEP_type_layout";
constexpr std::string_view kDependencyConfigurationId = "DEP_configuration";
constexpr std::string_view kDependencySpecificationId = "DEP_specification";
constexpr std::string_view kOmissionLevelProjectionId = "OM_level_projection";
constexpr std::string_view kOmissionTruncatedId = "OM_truncated_dominating_check";
constexpr std::string_view kOmissionVendorId = "OM_vendor_expansion";
constexpr std::string_view kOmissionSummaryId = "OM_summary_expansion";

// The one truncation text the demo asserts: M10C records a truncated
// dominating-check query as this explicit unknown and omission pair, and never
// as a negative fact.
constexpr std::string_view kTruncationReasonText =
    "dominating-check query truncated";
constexpr std::string_view kVendorUnknownReason = "EXTERNAL_FUNCTION";

// Producers are stable analyzer identities, not display names.
constexpr std::string_view kRangeProducer = "analysis.value_range";
constexpr std::string_view kCapacityProducer = "analysis.memory_object";
constexpr std::string_view kAliasProducer = "analysis.alias";
constexpr std::string_view kFlowProducer = "analysis.value_flow";
constexpr std::string_view kCallProducer = "analysis.call_graph";
constexpr std::string_view kMemoryProducer = "analysis.memory_effect";
constexpr std::string_view kSpecificationProducer = "specification.cwe";
constexpr std::string_view kLocalAnalyzerProducer = "clang.local.analysis";
constexpr std::string_view kWpaAnalyzerProducer = "wpa.souffle";

// The demo's quantities.
constexpr std::int64_t kCopyLengthMin = 0;
constexpr std::int64_t kCopyLengthMax = 65535;
constexpr std::int64_t kDstBufCapacity = 2048;
constexpr std::int64_t kProofBudgetSeconds = 30;

std::string EntityLocalId(std::string_view label) {
  return "E_" + std::string(label);
}

std::string EdgeLocalId(std::string_view from, std::string_view to) {
  return "ED_" + std::string(from) + "_" + std::string(to);
}

fp::ProducerKind ProtoProducerKind(facts::ProducerKind kind) {
  switch (kind) {
    case facts::ProducerKind::kWpaSouffle:
      return fp::PRODUCER_WPA_SOUFFLE;
    case facts::ProducerKind::kWpaCppConformance:
      return fp::PRODUCER_WPA_CPP_CONFORMANCE;
    case facts::ProducerKind::kWpaCppEmergency:
      return fp::PRODUCER_WPA_CPP_EMERGENCY;
    case facts::ProducerKind::kExternal:
      return fp::PRODUCER_EXTERNAL;
  }
  return fp::PRODUCER_UNSPECIFIED;
}

// Test support aborts on a broken fixture: the scenario is fixed, so a fixture
// invariant violation is a programming error, not a runtime condition.
template <typename T>
T ValueOrAbort(veritas::StatusOr<T> value) {
  if (!value.ok()) {
    std::abort();
  }
  return std::move(value).value();
}

// --- Expression builders ---

ev::Expression Integer(std::int64_t value) {
  ev::Expression expression;
  expression.kind = ev::Expression::Kind::kInteger;
  expression.integer = value;
  return expression;
}

ev::Expression Reference(std::string local_id) {
  ev::Expression expression;
  expression.kind = ev::Expression::Kind::kReference;
  expression.text = std::move(local_id);
  return expression;
}

ev::Expression Call(std::string callee,
                    std::vector<ev::Expression> arguments) {
  ev::Expression expression;
  expression.kind = ev::Expression::Kind::kCall;
  expression.text = std::move(callee);
  expression.operands = std::move(arguments);
  return expression;
}

ev::Expression Compare(std::string op, ev::Expression left,
                       ev::Expression right) {
  ev::Expression expression;
  expression.kind = ev::Expression::Kind::kCompare;
  expression.text = std::move(op);
  expression.operands = {std::move(left), std::move(right)};
  return expression;
}

ev::Expression ForAll(std::string variable, ev::Expression domain,
                      ev::Expression body) {
  ev::Expression expression;
  expression.kind = ev::Expression::Kind::kForAll;
  expression.text = std::move(variable);
  expression.operands = {std::move(domain), std::move(body)};
  return expression;
}

ev::Expression ValueOf(std::string_view local_id) {
  return Call("value", {Reference(std::string(local_id))});
}

ev::Expression CapacityOf(std::string_view local_id) {
  return Call("capacity", {Reference(std::string(local_id))});
}

ev::Expression RangePredicate(std::string_view value_id, std::int64_t min,
                              std::int64_t max) {
  return Call("range", {Reference(std::string(value_id)), Integer(min),
                        Integer(max)});
}

ev::Expression CapacityPredicate(std::string_view memory_id,
                                 std::int64_t bytes) {
  return Call("capacity",
              {Reference(std::string(memory_id)), Integer(bytes)});
}

ev::Expression AliasPredicate(std::string_view left, std::string_view right) {
  return Call("alias",
              {Reference(std::string(left)), Reference(std::string(right))});
}

ev::Expression OverflowPredicate(std::string_view value_id,
                                 std::string_view memory_id) {
  return Compare(">", ValueOf(value_id), CapacityOf(memory_id));
}

ev::Expression WithinCapacity(std::string_view value_id,
                              std::string_view memory_id) {
  return Compare("<=", ValueOf(value_id), CapacityOf(memory_id));
}

// --- Case member builders ---

ev::Entity MakeEntity(std::string id, ev::EntityKind kind,
                      core::StableId stable_id) {
  ev::Entity entity;
  entity.id = std::move(id);
  entity.kind = kind;
  entity.stable_id = std::move(stable_id);
  return entity;
}

ev::Edge MakeEirEdge(std::string id, std::string from, std::string to,
                     ev::RelationKind kind, ev::EpistemicState epistemic,
                     std::string provenance_id, std::string summarized_by,
                     bool expandable) {
  ev::Edge edge;
  edge.id = std::move(id);
  edge.from = std::move(from);
  edge.to = std::move(to);
  edge.kind = kind;
  edge.epistemic = epistemic;
  edge.provenance_id = std::move(provenance_id);
  edge.summarized_by = std::move(summarized_by);
  edge.expandable = expandable;
  return edge;
}

ev::Path MakeEirPath(std::string id, ev::PathKind kind,
                     std::vector<std::string> entity_ids,
                     std::vector<ev::Expression> conditions,
                     ev::Feasibility feasibility, std::string provenance_id) {
  ev::Path path;
  path.id = std::move(id);
  path.kind = kind;
  path.entity_ids = std::move(entity_ids);
  path.conditions = std::move(conditions);
  path.feasibility = feasibility;
  path.provenance_id = std::move(provenance_id);
  return path;
}

ev::Fact MakeEirFact(std::string id, std::optional<core::StableId> stable_id,
                     ev::Expression predicate, ev::EpistemicState epistemic,
                     ev::Confidence confidence, std::string producer,
                     std::string provenance_id, bool derived) {
  ev::Fact fact;
  fact.id = std::move(id);
  fact.stable_id = std::move(stable_id);
  fact.predicate = std::move(predicate);
  fact.epistemic = epistemic;
  fact.confidence = confidence;
  fact.producer = std::move(producer);
  fact.provenance_id = std::move(provenance_id);
  fact.derived = derived;
  return fact;
}

ev::Assumption MakeAssumption(std::string id, ev::Expression predicate,
                              std::string source, std::string scope) {
  ev::Assumption assumption;
  assumption.id = std::move(id);
  assumption.predicate = std::move(predicate);
  assumption.source = std::move(source);
  assumption.scope = std::move(scope);
  return assumption;
}

ev::Hypothesis MakeHypothesis(std::string id, ev::Expression predicate,
                              std::string producer, std::string reason,
                              ev::Confidence confidence) {
  ev::Hypothesis hypothesis;
  hypothesis.id = std::move(id);
  hypothesis.predicate = std::move(predicate);
  hypothesis.producer = std::move(producer);
  hypothesis.reason = std::move(reason);
  hypothesis.confidence = confidence;
  return hypothesis;
}

ev::Unknown MakeUnknown(std::string id, ev::Expression property,
                        ev::UnknownReasonCode reason_code, std::string reason,
                        std::vector<std::string> blocking_ids,
                        std::string suggested_resolution) {
  ev::Unknown unknown;
  unknown.id = std::move(id);
  unknown.property = std::move(property);
  unknown.reason_code = reason_code;
  unknown.reason = std::move(reason);
  unknown.blocking_ids = std::move(blocking_ids);
  unknown.suggested_resolution = std::move(suggested_resolution);
  return unknown;
}

ev::Constraint MakeConstraint(std::string id, ev::Expression expression,
                              std::string scope, ev::EpistemicState epistemic,
                              std::string provenance_id) {
  ev::Constraint constraint;
  constraint.id = std::move(id);
  constraint.expression = std::move(expression);
  constraint.scope = std::move(scope);
  constraint.epistemic = epistemic;
  constraint.provenance_id = std::move(provenance_id);
  return constraint;
}

ev::Provenance MakeProvenanceRecord(
    std::string id, std::string_view producer, std::string_view rule,
    std::string_view source_anchor, core::StableId run_id,
    std::vector<std::string> input_fact_ids,
    std::string_view version = "0.1") {
  ev::Provenance record;
  record.id = std::move(id);
  record.producer = std::string(producer);
  record.rule = std::string(rule);
  record.input_fact_ids = std::move(input_fact_ids);
  record.source_anchor_id = std::string(source_anchor);
  record.analysis_run_id = run_id;
  record.version = std::string(version);
  record.configuration = std::string(kAnalysisConfigurationId);
  return record;
}

ev::ProofObligation MakeProofObligation(
    std::string id, ev::Expression predicate,
    std::vector<std::string> verifier_kinds, ev::Expression budget) {
  ev::ProofObligation obligation;
  obligation.id = std::move(id);
  obligation.goal_kind = ev::ProofGoalKind::kProve;
  obligation.predicate = std::move(predicate);
  obligation.verifier_kinds = std::move(verifier_kinds);
  obligation.budget = std::move(budget);
  obligation.status = ev::ProofStatus::kPending;
  return obligation;
}

ev::SummaryReference MakeSummaryReference(
    std::string id, std::string function_id, core::StableId summary_id,
    std::vector<std::string> components) {
  ev::SummaryReference summary;
  summary.id = std::move(id);
  summary.function_id = std::move(function_id);
  summary.summary_id = std::move(summary_id);
  summary.components = std::move(components);
  return summary;
}

ev::Dependency MakeDependency(std::string id, ev::DependencyKind kind,
                              core::StableId stable_id) {
  ev::Dependency dependency;
  dependency.id = std::move(id);
  dependency.kind = kind;
  dependency.stable_id = std::move(stable_id);
  return dependency;
}

ev::Omission MakeOmission(std::string id, std::string kind,
                          std::string subject, std::string reason,
                          bool expandable) {
  ev::Omission omission;
  omission.id = std::move(id);
  omission.kind = std::move(kind);
  omission.subject = std::move(subject);
  omission.reason = std::move(reason);
  omission.expandable = expandable;
  return omission;
}

// --- Program identity ---

build::ProgramContext MakeProgramContext() {
  build::ProgramContext context;
  context.repository_id = std::string(kRepositoryId);
  context.revision_id = std::string(kRevisionId);
  context.build_variant_id = std::string(kBuildVariantId);
  context.project_root = std::string(kProjectRoot);
  context.vcs_kind = "git";
  context.vcs_revision = std::string(kRevisionId);
  context.source_tree_hash = "tree:demo";
  context.compilation_database_hash = "commands:demo";
  context.target_triple = std::string(kTargetTriple);
  context.compiler_id = "clang";
  context.compiler_version = "17.0.6";
  context.compile_options_hash = "options:demo";
  context.macro_set_hash = "macros:demo";
  context.include_closure_hash = "includes:demo";
  context.type_layout_hash = std::string(kTypeLayoutId);
  return context;
}

// The case-level projection of the M10B program context. The checkout path is
// deliberately dropped: it is never part of a case's identity.
ev::ProgramBinding BindProgram(const build::ProgramContext& context,
                               core::StableId run_id) {
  ev::ProgramBinding binding;
  binding.repository_id = context.repository_id;
  binding.revision_id = context.revision_id;
  binding.build_variant_id = context.build_variant_id;
  binding.target_triple = context.target_triple;
  binding.analysis_configuration_id = std::string(kAnalysisConfigurationId);
  binding.type_layout_id = context.type_layout_hash;
  binding.analysis_run_id = run_id;
  binding.analyzer_versions = {
      ev::AnalyzerVersion{std::string(kLocalAnalyzerProducer), "0.1",
                          std::string(kAnalysisConfigurationId)},
      ev::AnalyzerVersion{std::string(kWpaAnalyzerProducer), "2.5",
                          "souffle@pinned"},
  };
  return binding;
}

// --- Typed CPG to case projection ---

ev::EvidenceQueryBudget DemoBudget() {
  ev::EvidenceQueryBudget budget;
  budget.max_depth = 8;
  budget.max_nodes = 256;
  budget.max_paths = 5;
  budget.max_facts_per_query = 64;
  budget.max_provenance_depth = 8;
  return budget;
}

std::optional<ev::EntityKind> EntityKindFor(cpg::NodeKind kind) {
  switch (kind) {
    case cpg::NodeKind::kFunction:
      return ev::EntityKind::kFunction;
    case cpg::NodeKind::kCallSite:
      return ev::EntityKind::kCallSite;
    case cpg::NodeKind::kParameter:
      return ev::EntityKind::kValue;
    case cpg::NodeKind::kGlobal:
    case cpg::NodeKind::kMemoryObject:
      return ev::EntityKind::kMemoryObject;
    case cpg::NodeKind::kBasicBlockSummary:
      return ev::EntityKind::kBasicBlock;
    case cpg::NodeKind::kSummary:
    case cpg::NodeKind::kUnknown:
      return std::nullopt;
  }
  return std::nullopt;
}

// The demo's relation subset. A relation the case cannot represent is reported
// as absent rather than silently dropped (the factory aborts, since the
// scenario is fixed and only supported relations appear in it).
std::optional<ev::RelationKind> RelationKindFor(const cpg::CpgEdge& edge) {
  switch (edge.kind) {
    case cpg::EdgeKind::kCalls:
      return ev::RelationKind::kCalls;
    case cpg::EdgeKind::kReads:
      return ev::RelationKind::kReads;
    case cpg::EdgeKind::kWrites:
      return ev::RelationKind::kWrites;
    case cpg::EdgeKind::kFlowsTo:
      return ev::RelationKind::kFlowsTo;
    case cpg::EdgeKind::kDominatesSummary:
      return ev::RelationKind::kDominates;
    case cpg::EdgeKind::kAliases:
      if (edge.alias_state == cpg::AliasState::kMayAlias) {
        return ev::RelationKind::kMayAlias;
      }
      return std::nullopt;
    case cpg::EdgeKind::kContains:
    case cpg::EdgeKind::kDeclares:
    case cpg::EdgeKind::kMayCall:
    case cpg::EdgeKind::kSummarizedBy:
    case cpg::EdgeKind::kUnknownAt:
      return std::nullopt;
  }
  return std::nullopt;
}

// A may-alias relation is the only relation the demo does not establish must.
ev::EpistemicState EdgeEpistemic(ev::RelationKind kind) {
  return kind == ev::RelationKind::kMayAlias ? ev::EpistemicState::kMay
                                             : ev::EpistemicState::kMust;
}

std::string_view ProvenanceForRelation(ev::RelationKind kind) {
  switch (kind) {
    case ev::RelationKind::kCalls:
      return kProvenanceCallId;
    case ev::RelationKind::kFlowsTo:
      return kProvenanceFlowId;
    case ev::RelationKind::kReads:
    case ev::RelationKind::kWrites:
      return kProvenanceMemoryId;
    case ev::RelationKind::kMayAlias:
      return kProvenanceAliasId;
    case ev::RelationKind::kDominates:
    case ev::RelationKind::kUnspecified:
      return kProvenanceCallId;
  }
  return kProvenanceCallId;
}

struct CaseGraph {
  std::vector<ev::Entity> entities;
  std::vector<ev::Edge> edges;
};

// Projects a typed flow slice into case-local entities and edges. An empty
// `selected_labels` keeps every node; otherwise only the listed labels (and the
// edges between them) survive, which is how the L0 fixture drops the causal
// slice it does not declare. A non-empty `summary_id` attaches the summary
// reference — and with it expandability — to the call edges that reach the
// withheld `summarized_function`.
CaseGraph ProjectGraph(const evidence::EvidenceBuildInput& input,
                       const std::set<std::string>& selected_labels,
                       std::string_view summary_id,
                       std::string_view summarized_function) {
  const evidence::FlowSlice& slice = input.flow_slice;
  CaseGraph graph;
  std::map<core::StableId, std::string> labels;
  for (const cpg::CpgNode& node : slice.nodes) {
    if (!selected_labels.empty() && selected_labels.count(node.label) == 0) {
      continue;
    }
    const std::optional<ev::EntityKind> kind = EntityKindFor(node.kind);
    if (!kind.has_value()) {
      std::abort();
    }
    labels.emplace(node.node_id, node.label);
    graph.entities.push_back(
        MakeEntity(EntityLocalId(node.label), *kind, node.node_id));
  }

  for (const cpg::CpgEdge& edge : slice.edges) {
    const auto from = labels.find(edge.source_node_id);
    const auto to = labels.find(edge.target_node_id);
    if (from == labels.end() || to == labels.end()) {
      continue;
    }
    const std::optional<ev::RelationKind> kind = RelationKindFor(edge);
    if (!kind.has_value()) {
      std::abort();
    }
    std::string summarized_by;
    if (!summary_id.empty() && to->second == summarized_function) {
      summarized_by = std::string(summary_id);
    }
    // Only an edge whose summary was withheld is expandable: a call into an
    // unmodeled external has nothing to expand.
    const bool expandable = !summarized_by.empty();
    graph.edges.push_back(MakeEirEdge(
        EdgeLocalId(from->second, to->second), EntityLocalId(from->second),
        EntityLocalId(to->second), *kind, EdgeEpistemic(*kind),
        std::string(ProvenanceForRelation(*kind)), std::move(summarized_by),
        expandable));
  }
  return graph;
}

// --- Typed query results ---

struct QueryFixture {
  ev::EvidenceFactSet set;
  facts::AnalysisFact completion_fact;
  facts::RunFactBinding binding;
  facts::FactWitness witness;
  ev::QueryCompletionDescriptor descriptor;
};

const facts::AnalysisFact& SingleFactOrAbort(const ev::EvidenceFactSet& set) {
  if (set.facts.size() != 1) {
    std::abort();
  }
  return set.facts.front();
}

// One bounded query result plus the completion certificate, run binding, and
// selected witness that certify it. The descriptor, the certificate, and the
// metadata are written together from the same values, so the certificate
// re-derives rather than being patched into agreement.
QueryFixture MakeQueryFixture(const EvidenceScenarioBuilder& builder,
                              std::string_view query_kind, core::StableId run_id,
                              std::vector<core::StableId> scope_refs,
                              ev::QueryCompleteness completeness,
                              std::vector<ev::TruncationReason> reasons,
                              std::size_t examined_items, std::string digest) {
  // Reasons are recorded in canonical enum order from the start, so the
  // certificate's ordered list and the metadata agree exactly.
  std::sort(reasons.begin(), reasons.end());

  QueryFixture fixture;
  fixture.descriptor.query_kind = std::string(query_kind);
  fixture.descriptor.ordered_scope_refs = std::move(scope_refs);
  fixture.descriptor.budget = DemoBudget();
  fixture.descriptor.query_implementation_version =
      std::string(ev::kQueryImplementationVersion);
  fixture.descriptor.input_snapshot_fingerprint =
      std::string(kSnapshotFingerprint);
  fixture.descriptor.completeness = completeness;
  fixture.descriptor.ordered_truncation_reasons = std::move(reasons);
  fixture.descriptor.examined_items = examined_items;
  fixture.descriptor.returned_member_digest = std::move(digest);

  fixture.completion_fact =
      ValueOrAbort(ev::MakeQueryCompletionFact(fixture.descriptor));
  fixture.binding = builder.MakeBinding(
      run_id, fixture.completion_fact.fact_id, ev::kQueryCompletionWitnessId);
  fixture.witness = builder.MakeWitness(
      run_id, fixture.completion_fact.fact_id, ev::kQueryCompletionWitnessId);

  fixture.set.metadata.completeness = completeness;
  fixture.set.metadata.truncation_reasons =
      fixture.descriptor.ordered_truncation_reasons;
  fixture.set.metadata.examined_items = examined_items;
  fixture.set.metadata.analysis_run_id = run_id;
  fixture.set.metadata.query_provenance_id = fixture.completion_fact.fact_id;
  return fixture;
}

// A fixture whose certificate, binding, selected witness, and metadata do not
// re-derive together is a broken fixture, not a runtime condition.
void SealQueryFixture(const QueryFixture& fixture) {
  if (!ev::ValidateQueryCompletion(fixture.completion_fact, fixture.binding,
                                   fixture.witness, fixture.set.metadata,
                                   fixture.descriptor)
           .ok()) {
    std::abort();
  }
}

QueryFixture MakeFactQueryFixture(const EvidenceScenarioBuilder& builder,
                                  std::string_view query_kind,
                                  core::StableId run_id,
                                  std::vector<core::StableId> scope_refs,
                                  std::vector<facts::AnalysisFact> facts,
                                  ev::QueryCompleteness completeness,
                                  std::vector<ev::TruncationReason> reasons,
                                  std::size_t examined_items) {
  QueryFixture fixture =
      MakeQueryFixture(builder, query_kind, run_id, std::move(scope_refs),
                       completeness, std::move(reasons), examined_items,
                       ev::ReturnedMemberDigest(facts));
  std::sort(facts.begin(), facts.end(),
            [](const facts::AnalysisFact& a, const facts::AnalysisFact& b) {
              return a.fact_id < b.fact_id;
            });
  fixture.set.facts = std::move(facts);
  SealQueryFixture(fixture);
  return fixture;
}

// The value-flow query is the one result that is not an EvidenceFactSet: its
// returned members are the slice's nodes and edges, so its digest covers those.
QueryFixture MakeFlowQueryFixture(const EvidenceScenarioBuilder& builder,
                                  core::StableId run_id,
                                  std::vector<core::StableId> scope_refs,
                                  const ev::FlowSlice& slice) {
  QueryFixture fixture = MakeQueryFixture(
      builder, "value_flow", run_id, std::move(scope_refs),
      ev::QueryCompleteness::kComplete, {},
      slice.nodes.size() + slice.edges.size(), ev::ReturnedMemberDigest(slice));
  SealQueryFixture(fixture);
  return fixture;
}

void FillProtoBinding(fp::RunFactBinding* out,
                      const facts::RunFactBinding& binding) {
  out->set_analysis_run_id(core::ToString(binding.run_id));
  out->set_fact_id(core::ToString(binding.fact_id));
  if (binding.confidence.has_value()) {
    out->set_confidence(*binding.confidence);
  }
  out->set_producer_kind(ProtoProducerKind(binding.producer_kind));
  out->set_analyzer_run_id(binding.analyzer_run_id);
  out->set_scope_kind(binding.scope_kind);
  out->set_scope_id(binding.scope_id);
  out->set_selected_witness_id(binding.selected_witness_id);
  out->set_is_current(binding.is_current);
}

void FillProtoWitness(fp::FactWitness* out, const facts::FactWitness& witness) {
  out->set_analysis_run_id(core::ToString(witness.run_id));
  out->set_output_fact_id(core::ToString(witness.output_fact_id));
  out->set_witness_id(witness.witness_id);
  out->set_selected(witness.selected);
  out->set_producer_kind(ProtoProducerKind(witness.producer_kind));
  out->set_producer_id(witness.producer_id);
  out->set_rule_id(witness.rule_id);
  out->set_rule_version(witness.rule_version);
  out->set_analyzer_run_id(witness.analyzer_run_id);
  out->set_source_anchor_id(witness.source_anchor_id);
  out->set_summary_id(witness.summary_id);
  out->set_description(witness.description);
}

// --- The demo program and its typed queries ---

// The program identity and CPG nodes every demo fixture is built from. The
// labels are EIR-T Identifiers, so the projected case-local IDs stay readable.
struct DemoProgram {
  core::StableId run_id;
  build::ProgramContext context;
  cpg::CpgNode decode;
  cpg::CpgNode memcpy_site;
  cpg::CpgNode srcbuf;
  cpg::CpgNode dstbuf;
  cpg::CpgNode copy_length;
  // The memory identity of the slot holding the copied length. The CPG models
  // the length as a parameter (a value node) because that is what the flow path
  // traverses; the M8R.2 range relation, however, keys a byte range on a
  // memory reference, so the fixture names the slot separately. It is the
  // `value_ref` of the range query and the memory column of the range fact,
  // exactly as the M10B range tests pair them.
  core::StableId copy_length_memory;
  cpg::CpgNode entry;
  cpg::CpgNode vendor_validate;
};

DemoProgram MakeDemoProgram(const EvidenceScenarioBuilder& builder) {
  DemoProgram program;
  program.run_id = builder.Id(core::IdKind::kAnalysisRun, kRunName);
  program.context = MakeProgramContext();
  program.decode = builder.MakeNode(kDecodeLabel, cpg::NodeKind::kFunction);
  program.memcpy_site = builder.MakeNode(kMemcpyLabel, cpg::NodeKind::kCallSite);
  program.srcbuf = builder.MakeNode(kSrcBufLabel, cpg::NodeKind::kMemoryObject);
  program.dstbuf = builder.MakeNode(kDstBufLabel, cpg::NodeKind::kMemoryObject);
  program.copy_length =
      builder.MakeNode(kCopyLengthLabel, cpg::NodeKind::kParameter);
  program.copy_length_memory = builder.Id(
      core::IdKind::kMemoryRef, Suffixed(kCopyLengthLabel, ":memory"));
  program.entry = builder.MakeNode(kEntryLabel, cpg::NodeKind::kFunction);
  program.vendor_validate =
      builder.MakeNode(kVendorValidateLabel, cpg::NodeKind::kFunction);
  return program;
}

// The demo's thin-CPG relation slice: the sink call, the vendor call whose
// summary is withheld, the value flow into the sink operand, the memory the
// sink's caller reads and writes, and the may-alias premise between them. Only
// relations the case can represent appear here; a relation the model cannot
// carry is never silently dropped.
std::vector<cpg::CpgEdge> MakeDemoEdges(
    const EvidenceScenarioBuilder& builder, const DemoProgram& program) {
  std::vector<cpg::CpgEdge> edges;
  edges.push_back(builder.MakeEdge("calls:decode:memcpy", cpg::EdgeKind::kCalls,
                                   program.decode.node_id,
                                   program.memcpy_site.node_id));
  edges.push_back(builder.MakeEdge(
      "calls:decode:vendor_validate", cpg::EdgeKind::kCalls,
      program.decode.node_id, program.vendor_validate.node_id));
  edges.push_back(builder.MakeEdge(
      "flows:srcbuf:copy_length", cpg::EdgeKind::kFlowsTo,
      program.srcbuf.node_id, program.copy_length.node_id));
  edges.push_back(builder.MakeEdge(
      "flows:copy_length:memcpy", cpg::EdgeKind::kFlowsTo,
      program.copy_length.node_id, program.memcpy_site.node_id));
  edges.push_back(builder.MakeEdge("writes:decode:dstbuf", cpg::EdgeKind::kWrites,
                                   program.decode.node_id,
                                   program.dstbuf.node_id));
  edges.push_back(builder.MakeEdge("reads:decode:srcbuf", cpg::EdgeKind::kReads,
                                   program.decode.node_id,
                                   program.srcbuf.node_id));
  cpg::CpgEdge alias =
      builder.MakeEdge("aliases:srcbuf:dstbuf", cpg::EdgeKind::kAliases,
                       program.srcbuf.node_id, program.dstbuf.node_id);
  alias.alias_state = cpg::AliasState::kMayAlias;
  edges.push_back(std::move(alias));
  return edges;
}

// The demo's facts, in the shapes the M10B queries match: the range of the
// copied length, the destination object's capacity, the may-alias premise, and
// the function-scoped unknowns. Each cell type mirrors the corresponding
// builder helper so the rows stay schema-valid.
facts::AnalysisFact MakeValueRangeFact(const DemoProgram& program) {
  facts::SemanticRow row;
  row.relation = facts::RelationId::kDirectRead;
  row.cells = {program.decode.node_id, program.copy_length_memory,
               sem::ByteRangeKind::kKnown, std::int64_t{kCopyLengthMin},
               static_cast<std::uint64_t>(kCopyLengthMax),
               sem::EpistemicState::kMust};
  return MakeFactOrAbort(std::move(row));
}

facts::AnalysisFact MakeObjectCapacityFact(const DemoProgram& program) {
  facts::SemanticRow row;
  row.relation = facts::RelationId::kDirectWrite;
  row.cells = {program.decode.node_id, program.dstbuf.node_id,
               sem::ByteRangeKind::kKnown, std::int64_t{0},
               static_cast<std::uint64_t>(kDstBufCapacity),
               sem::EpistemicState::kMust};
  return MakeFactOrAbort(std::move(row));
}

facts::AnalysisFact MakeMayAliasFact(const DemoProgram& program) {
  facts::SemanticRow row;
  row.relation = facts::RelationId::kAlias;
  row.cells = {program.srcbuf.node_id, program.dstbuf.node_id,
               sem::AliasKind::kMayAlias, sem::EpistemicState::kMay};
  return MakeFactOrAbort(std::move(row));
}

facts::AnalysisFact MakeFunctionUnknownFact(const DemoProgram& program,
                                            std::string_view reason) {
  facts::SemanticRow row;
  row.relation = facts::RelationId::kUnknownEffect;
  row.cells = {program.decode.node_id, std::string(kDecodeLabel),
               std::string(reason), sem::EpistemicState::kUnknown};
  return MakeFactOrAbort(std::move(row));
}

// The demo handoff plus the identity of the dominating-check certificate, which
// the case records as the fact dependency it consumed.
struct DemoInput {
  evidence::EvidenceBuildInput input;
  core::StableId check_completion_fact_id;
};

DemoInput MakeDemoInput(const EvidenceScenarioBuilder& builder,
                        bool truncated_dominating_checks,
                        ev::TruncationReason dominating_checks_reason) {
  const DemoProgram program = MakeDemoProgram(builder);

  DemoInput demo;
  evidence::EvidenceBuildInput& input = demo.input;

  input.claim_seed.finding_id =
      builder.Id(core::IdKind::kFact, "finding:overflow_001");
  input.claim_seed.kind = ev::ClaimKind::kBufferOverflow;
  input.claim_seed.severity = ev::Severity::kHigh;
  input.claim_seed.subject_ref = program.dstbuf.node_id;
  input.claim_seed.source_ref = program.copy_length.node_id;
  input.claim_seed.sink_ref = program.memcpy_site.node_id;

  const facts::AnalysisFact range_fact = MakeValueRangeFact(program);
  const facts::AnalysisFact capacity_fact = MakeObjectCapacityFact(program);
  const facts::AnalysisFact alias_fact = MakeMayAliasFact(program);
  const facts::AnalysisFact check_unknown =
      MakeFunctionUnknownFact(program, kTruncationReasonText);
  const facts::AnalysisFact vendor_unknown =
      MakeFunctionUnknownFact(program, kVendorUnknownReason);

  ev::FlowSlice& slice = input.flow_slice;
  slice.nodes = {program.decode,   program.memcpy_site, program.srcbuf,
                 program.dstbuf,   program.copy_length, program.entry,
                 program.vendor_validate};
  slice.edges = MakeDemoEdges(builder, program);
  slice.supporting_facts = {range_fact, alias_fact};
  slice.unknowns = {check_unknown, vendor_unknown};
  for (const auto& fact : slice.supporting_facts) {
    slice.provenance_refs.push_back(fact.fact_id);
  }
  for (const auto& fact : slice.unknowns) {
    slice.provenance_refs.push_back(fact.fact_id);
  }
  std::sort(slice.provenance_refs.begin(), slice.provenance_refs.end());

  QueryFixture flow_fixture =
      MakeFlowQueryFixture(builder, program.run_id,
                           {program.srcbuf.node_id, program.memcpy_site.node_id},
                           slice);
  slice.metadata = flow_fixture.set.metadata;

  QueryFixture range_fixture = MakeFactQueryFixture(
      builder, "range", program.run_id, {program.copy_length_memory},
      {range_fact}, ev::QueryCompleteness::kComplete, {}, 1);
  QueryFixture capacity_fixture = MakeFactQueryFixture(
      builder, "capacity", program.run_id, {input.claim_seed.subject_ref},
      {capacity_fact}, ev::QueryCompleteness::kComplete, {}, 1);
  QueryFixture alias_fixture = MakeFactQueryFixture(
      builder, "alias", program.run_id, {input.claim_seed.subject_ref},
      {alias_fact}, ev::QueryCompleteness::kComplete, {}, 1);
  QueryFixture unknown_fixture = MakeFactQueryFixture(
      builder, "unknown", program.run_id, {program.decode.node_id},
      {check_unknown, vendor_unknown}, ev::QueryCompleteness::kComplete, {}, 2);

  // The dominating-check query is closed-world: the demo's missing check is a
  // complete-empty result, and a budget cut turns it into a truncated-empty one
  // that records the reason instead of a negative fact.
  const ev::QueryCompleteness check_completeness =
      truncated_dominating_checks ? ev::QueryCompleteness::kTruncated
                                 : ev::QueryCompleteness::kComplete;
  const std::vector<ev::TruncationReason> check_reasons =
      truncated_dominating_checks
          ? std::vector<ev::TruncationReason>{dominating_checks_reason}
          : std::vector<ev::TruncationReason>{};
  QueryFixture check_fixture = MakeFactQueryFixture(
      builder, "dominating_check", program.run_id,
      {input.claim_seed.sink_ref}, {}, check_completeness, check_reasons, 0);
  demo.check_completion_fact_id = check_fixture.completion_fact.fact_id;

  input.ranges = range_fixture.set;
  input.capacities = capacity_fixture.set;
  input.aliases = alias_fixture.set;
  input.unknowns = unknown_fixture.set;
  input.dominating_checks = check_fixture.set;

  std::vector<QueryFixture> fixtures;
  fixtures.push_back(std::move(flow_fixture));
  fixtures.push_back(std::move(range_fixture));
  fixtures.push_back(std::move(capacity_fixture));
  fixtures.push_back(std::move(alias_fixture));
  fixtures.push_back(std::move(unknown_fixture));
  fixtures.push_back(std::move(check_fixture));
  std::sort(fixtures.begin(), fixtures.end(),
            [](const QueryFixture& a, const QueryFixture& b) {
              return a.completion_fact.fact_id < b.completion_fact.fact_id;
            });
  for (const QueryFixture& fixture : fixtures) {
    input.query_completion_facts.push_back(fixture.completion_fact);
    input.query_completion_bindings.push_back(fixture.binding);
  }

  // The handoff's provenance closure is rooted at the value-flow certificate,
  // as production roots it, and reports the truncated check at the handoff
  // level rather than hiding it.
  auto& provenance = input.provenance;
  provenance.set_run_id(core::ToString(program.run_id));
  for (const QueryFixture& fixture : fixtures) {
    if (fixture.descriptor.query_kind == "value_flow") {
      provenance.set_fact_id(core::ToString(fixture.completion_fact.fact_id));
      auto proto_fact = facts::ToProtoFact(fixture.completion_fact);
      if (proto_fact.ok()) {
        *provenance.mutable_fact() = std::move(*proto_fact);
      }
      FillProtoBinding(provenance.mutable_binding(), fixture.binding);
    }
    FillProtoWitness(provenance.add_nodes(), fixture.witness);
  }
  provenance.set_truncated(truncated_dominating_checks);
  if (truncated_dominating_checks) {
    provenance.set_truncation_reason(
        std::string(ev::ToString(dominating_checks_reason)));
  }
  return demo;
}

}  // namespace

evidence::QueryResultMetadata EvidenceScenarioBuilder::MakeMetadata(
    std::string_view run_name, std::string_view provenance_name,
    evidence::QueryCompleteness completeness,
    std::vector<evidence::TruncationReason> reasons,
    std::size_t examined_items) const {
  evidence::QueryResultMetadata metadata;
  metadata.completeness = completeness;
  metadata.truncation_reasons = std::move(reasons);
  metadata.examined_items = examined_items;
  metadata.analysis_run_id = Id(core::IdKind::kAnalysisRun, run_name);
  metadata.query_provenance_id = Id(core::IdKind::kFact, provenance_name);
  return metadata;
}

// --- M10C semantic-case factories -------------------------------------------

EvidenceScenarioBuilder& EvidenceScenarioBuilder::WithTruncatedDominatingChecks(
    evidence::TruncationReason reason) {
  truncated_dominating_checks_ = true;
  dominating_checks_reason_ = reason;
  return *this;
}

evidence::EvidenceBuildRequest EvidenceScenarioBuilder::BuildRequestFor(
    evidence::EvidenceLevel level, bool truncated_dominating_checks,
    evidence::TruncationReason reason) const {
  DemoInput demo = MakeDemoInput(*this, truncated_dominating_checks, reason);
  evidence::EvidenceBuildRequest request;
  request.context = MakeProgramContext();
  request.input = std::move(demo.input);
  request.level = level;
  return request;
}

evidence::EvidenceBuildRequest EvidenceScenarioBuilder::BuildRequest(
    evidence::EvidenceLevel level) const {
  return BuildRequestFor(level, truncated_dominating_checks_,
                         dominating_checks_reason_);
}

// The smallest valid initial L0 case, projected from the same typed handoff the
// L1 case uses: the claim and its subject, the primary supporting fact, the
// primary path, one expandable omission for the causal slice L0 withholds, and
// the initial verification state. L0 carries no proof obligation.
evidence::EvidenceCase EvidenceScenarioBuilder::MakeValidMinimalEvidenceCase()
    const {
  const core::StableId run_id = Id(core::IdKind::kAnalysisRun, kRunName);
  DemoInput demo = MakeDemoInput(*this, /*truncated_dominating_checks=*/false,
                                 evidence::TruncationReason::kUnspecified);

  // Only the claim's own subject and the primary path survive the L0
  // projection; the rest of the causal slice stays an expandable omission.
  const CaseGraph graph = ProjectGraph(
      demo.input,
      {std::string(kCopyLengthLabel), std::string(kMemcpyLabel),
       std::string(kDstBufLabel)},
      "", "");

  evidence::EvidenceCase value;
  value.schema_version = std::string(evidence::kEvidenceSchemaVersion);
  value.level = evidence::EvidenceLevel::kL0;
  value.program = BindProgram(MakeProgramContext(), run_id);

  value.primary_claim.id = std::string(kClaimId);
  value.primary_claim.kind = evidence::ClaimKind::kBufferOverflow;
  value.primary_claim.subject = EntityLocalId(kDstBufLabel);
  value.primary_claim.predicate =
      OverflowPredicate(EntityLocalId(kCopyLengthLabel),
                        EntityLocalId(kDstBufLabel));
  value.primary_claim.severity = evidence::Severity::kHigh;
  value.primary_claim.description =
      "the copied length may exceed the destination buffer capacity";

  value.entities = graph.entities;
  value.edges = graph.edges;
  value.paths.push_back(MakeEirPath(
      std::string(kPathValueFlowId), evidence::PathKind::kValueFlow,
      {EntityLocalId(kCopyLengthLabel), EntityLocalId(kMemcpyLabel)}, {},
      evidence::Feasibility::kSat, std::string(kProvenanceFlowId)));

  value.provenance = {
      MakeProvenanceRecord(std::string(kProvenanceRangeId), kRangeProducer,
                           "range.known.v1", "anchor:copy_length", run_id, {}),
      MakeProvenanceRecord(std::string(kProvenanceFlowId), kFlowProducer,
                           "value_flow.interprocedural.v1", "anchor:memcpy",
                           run_id, {}),
  };

  const facts::AnalysisFact& range_fact = SingleFactOrAbort(demo.input.ranges);
  value.facts.push_back(MakeEirFact(
      std::string(kFactRangeId), range_fact.fact_id,
      RangePredicate(EntityLocalId(kCopyLengthLabel), kCopyLengthMin,
                     kCopyLengthMax),
      evidence::EpistemicState::kMust, evidence::Confidence::kExact,
      std::string(kRangeProducer), std::string(kProvenanceRangeId),
      /*derived=*/false));

  value.omissions.push_back(MakeOmission(
      std::string(kOmissionLevelProjectionId), "level_projection",
      EntityLocalId(kMemcpyLabel), "the l1 causal slice is withheld at l0",
      /*expandable=*/true));
  value.verification_state = evidence::VerificationState::kPossibleDefect;
  return value;
}

// The fully populated DEM-001 case. It is always projected from the truncated
// dominating-check handoff, so the missing check stays visible as an unknown
// plus an expandable omission and is never turned into a negative fact.
evidence::EvidenceCase EvidenceScenarioBuilder::MakeOverflowEvidenceCase() const {
  const core::StableId run_id = Id(core::IdKind::kAnalysisRun, kRunName);
  const evidence::TruncationReason reason =
      dominating_checks_reason_ == evidence::TruncationReason::kUnspecified
          ? evidence::TruncationReason::kMaxPaths
          : dominating_checks_reason_;
  DemoInput demo = MakeDemoInput(*this, /*truncated_dominating_checks=*/true,
                                 reason);

  const core::StableId summary_id =
      Id(core::IdKind::kFunctionSummary, kVendorValidateLabel);
  const CaseGraph graph =
      ProjectGraph(demo.input, {}, kSummaryId, kVendorValidateLabel);

  evidence::EvidenceCase value;
  value.schema_version = std::string(evidence::kEvidenceSchemaVersion);
  value.level = evidence::EvidenceLevel::kL1;
  value.program = BindProgram(MakeProgramContext(), run_id);

  value.primary_claim.id = std::string(kClaimId);
  value.primary_claim.kind = evidence::ClaimKind::kBufferOverflow;
  value.primary_claim.subject = EntityLocalId(kDstBufLabel);
  value.primary_claim.predicate =
      OverflowPredicate(EntityLocalId(kCopyLengthLabel),
                        EntityLocalId(kDstBufLabel));
  value.primary_claim.severity = evidence::Severity::kHigh;
  value.primary_claim.description =
      "the copied length is not bounded by the destination capacity at the "
      "sink call";

  value.entities = graph.entities;
  value.edges = graph.edges;
  value.paths.push_back(MakeEirPath(
      std::string(kPathValueFlowId), evidence::PathKind::kValueFlow,
      {EntityLocalId(kSrcBufLabel), EntityLocalId(kCopyLengthLabel),
       EntityLocalId(kMemcpyLabel)},
      {AliasPredicate(EntityLocalId(kSrcBufLabel),
                      EntityLocalId(kDstBufLabel))},
      evidence::Feasibility::kSat, std::string(kProvenanceFlowId)));

  value.provenance = {
      MakeProvenanceRecord(std::string(kProvenanceRangeId), kRangeProducer,
                           "range.known.v1", "anchor:copy_length", run_id, {}),
      MakeProvenanceRecord(std::string(kProvenanceCapacityId),
                           kCapacityProducer, "capacity.object.v1",
                           "anchor:dstbuf", run_id, {}),
      MakeProvenanceRecord(std::string(kProvenanceAliasId), kAliasProducer,
                           "alias.may.v1", "anchor:dstbuf", run_id,
                           {std::string(kFactRangeId),
                            std::string(kFactCapacityId)}),
      MakeProvenanceRecord(std::string(kProvenanceFlowId), kFlowProducer,
                           "value_flow.interprocedural.v1", "anchor:memcpy",
                           run_id, {}),
      MakeProvenanceRecord(std::string(kProvenanceCallId), kCallProducer,
                           "call.direct.v1", "anchor:decode", run_id, {}),
      MakeProvenanceRecord(std::string(kProvenanceMemoryId), kMemoryProducer,
                           "memory.effect.v1", "anchor:decode", run_id, {}),
      MakeProvenanceRecord(std::string(kProvenanceQueryId),
                           evidence::kQueryCompletionProducerId,
                           evidence::kQueryCompletionRuleId,
                           "anchor:dominating_check", run_id, {}),
      MakeProvenanceRecord(std::string(kProvenanceSpecificationId),
                           kSpecificationProducer, "cwe.787.v1",
                           "anchor:protocol", run_id, {}),
  };

  const facts::AnalysisFact& range_fact = SingleFactOrAbort(demo.input.ranges);
  const facts::AnalysisFact& capacity_fact =
      SingleFactOrAbort(demo.input.capacities);
  const facts::AnalysisFact& alias_fact =
      SingleFactOrAbort(demo.input.aliases);
  value.facts.push_back(MakeEirFact(
      std::string(kFactRangeId), range_fact.fact_id,
      RangePredicate(EntityLocalId(kCopyLengthLabel), kCopyLengthMin,
                     kCopyLengthMax),
      evidence::EpistemicState::kMust, evidence::Confidence::kExact,
      std::string(kRangeProducer), std::string(kProvenanceRangeId),
      /*derived=*/false));
  value.facts.push_back(MakeEirFact(
      std::string(kFactCapacityId), capacity_fact.fact_id,
      CapacityPredicate(EntityLocalId(kDstBufLabel), kDstBufCapacity),
      evidence::EpistemicState::kMust, evidence::Confidence::kExact,
      std::string(kCapacityProducer), std::string(kProvenanceCapacityId),
      /*derived=*/false));
  value.facts.push_back(MakeEirFact(
      std::string(kFactAliasId), alias_fact.fact_id,
      AliasPredicate(EntityLocalId(kSrcBufLabel), EntityLocalId(kDstBufLabel)),
      evidence::EpistemicState::kMay, evidence::Confidence::kMedium,
      std::string(kAliasProducer), std::string(kProvenanceAliasId),
      /*derived=*/true));

  value.assumptions.push_back(MakeAssumption(
      std::string(kAssumptionId),
      Call("tainted", {Reference(EntityLocalId(kCopyLengthLabel))}),
      "threat_model.packet_length", "global"));

  value.hypotheses.push_back(MakeHypothesis(
      std::string(kHypothesisId),
      Call("postcondition",
           {Reference(EntityLocalId(kVendorValidateLabel)),
            WithinCapacity(EntityLocalId(kCopyLengthLabel),
                           EntityLocalId(kDstBufLabel))}),
      "review.agent.hypothesis.v1",
      "the vendor validator clamps the length before the sink",
      evidence::Confidence::kLow));

  // The truncated check is an open question, not a refutation: it records the
  // reason the query stopped and blocks nothing by asserting a negation.
  value.unknowns.push_back(MakeUnknown(
      std::string(kUnknownCheckId),
      Call("dominates", {Reference(EntityLocalId(kVendorValidateLabel)),
                         Reference(EntityLocalId(kMemcpyLabel))}),
      ev::UnknownReasonCode::kAnalysisTimeout,
      std::string(kTruncationReasonText), {std::string(kFactAliasId)},
      "expand_dominating_check_query(E_memcpy)"));
  value.unknowns.push_back(MakeUnknown(
      std::string(kUnknownVendorId),
      Call("postcondition", {Reference(EntityLocalId(kVendorValidateLabel))}),
      ev::UnknownReasonCode::kExternalFunction,
      std::string(kVendorUnknownReason), {std::string(kFactCapacityId)},
      "infer_contract(E_vendor_validate)"));

  value.constraints.push_back(MakeConstraint(
      std::string(kConstraintId),
      WithinCapacity(EntityLocalId(kCopyLengthLabel),
                     EntityLocalId(kDstBufLabel)),
      "path", evidence::EpistemicState::kMust,
      std::string(kProvenanceSpecificationId)));

  value.proof_obligations.push_back(MakeProofObligation(
      std::string(kObligationId),
      ForAll("path",
             Call("feasible_paths", {Reference(EntityLocalId(kEntryLabel)),
                                     Reference(EntityLocalId(kMemcpyLabel))}),
             WithinCapacity(EntityLocalId(kCopyLengthLabel),
                            EntityLocalId(kDstBufLabel))),
      {"range_analysis", "smt", "symbolic_execution"},
      Integer(kProofBudgetSeconds)));

  value.summaries.push_back(MakeSummaryReference(
      std::string(kSummaryId), EntityLocalId(kVendorValidateLabel), summary_id,
      {"postcondition", "call_effect"}));

  value.dependencies = {
      MakeDependency(std::string(kDependencySummaryId),
                     evidence::DependencyKind::kSummary, summary_id),
      MakeDependency(std::string(kDependencyFactId),
                     evidence::DependencyKind::kFact,
                     demo.check_completion_fact_id),
      MakeDependency(std::string(kDependencyTypeLayoutId),
                     evidence::DependencyKind::kTypeLayout,
                     Id(core::IdKind::kModel, kTypeLayoutId)),
      MakeDependency(std::string(kDependencyConfigurationId),
                     evidence::DependencyKind::kConfiguration,
                     Id(core::IdKind::kModel, kAnalysisConfigurationId)),
      MakeDependency(std::string(kDependencySpecificationId),
                     evidence::DependencyKind::kSpecification,
                     Id(core::IdKind::kModel, "cwe-787")),
  };

  value.omissions = {
      MakeOmission(std::string(kOmissionTruncatedId), "truncated_query",
                   std::string(kUnknownCheckId),
                   std::string(kTruncationReasonText), /*expandable=*/true),
      MakeOmission(std::string(kOmissionVendorId), "analyzer_expansion",
                   std::string(kUnknownVendorId),
                   "the external function has no summary",
                   /*expandable=*/false),
      MakeOmission(std::string(kOmissionSummaryId), "summary_expansion",
                   std::string(kSummaryId),
                   "the vendor summary's components are withheld at l1",
                   /*expandable=*/true),
  };

  value.verification_state = evidence::VerificationState::kPossibleDefect;
  return value;
}

evidence::EvidenceCase MakeValidMinimalEvidenceCase() {
  return EvidenceScenarioBuilder().MakeValidMinimalEvidenceCase();
}

evidence::EvidenceCase MakeOverflowEvidenceCase() {
  return EvidenceScenarioBuilder().MakeOverflowEvidenceCase();
}

}  // namespace veritas::testing
