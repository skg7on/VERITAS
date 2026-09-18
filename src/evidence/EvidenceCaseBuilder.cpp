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

#include "veritas/evidence/EvidenceCaseBuilder.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/cpg/CpgTypes.h"
#include "veritas/evidence/EvidenceCanonicalizer.h"
#include "veritas/evidence/EvidencePredicateMapper.h"
#include "veritas/evidence/EvidenceQueryService.h"
#include "veritas/evidence/EvidenceValidator.h"
#include "veritas/evidence/QueryCompletion.h"
#include "veritas/facts/RelationSchema.h"

namespace veritas::evidence {

namespace {

namespace sem = analysis::semantic;

// --- Local-ID vocabulary ----------------------------------------------------
//
// A case-local handle is an EIR-T `Identifier`: `[A-Za-z][A-Za-z0-9_]*`. Every
// handle below is a pure function of the request's stable IDs, so two builds of
// the same request agree byte for byte and reversing the insertion order of any
// input vector changes nothing. Nothing here reads a filename, a line number, a
// clock, or a pointer.
constexpr std::string_view kClaimLocalId = "C1";
constexpr std::string_view kObligationLocalId = "O1";
constexpr std::string_view kPathLocalId = "P_value_flow";
constexpr std::string_view kConstraintLocalId = "K_path_safety";
constexpr std::string_view kDependencyFactId = "DEP_fact";
constexpr std::string_view kDependencyTypeLayoutId = "DEP_type_layout";
constexpr std::string_view kDependencyConfigurationId = "DEP_configuration";
constexpr std::string_view kAbsenceProvenanceId = "PR_closed_world_absence";
constexpr std::string_view kAbsencePredicate = "dominates_bounds_check";
constexpr std::string_view kCoverageProducerId = "analysis.soundness_coverage";

// --- Text and identifier helpers --------------------------------------------

bool IsIdentifierChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

// Maps arbitrary text onto the EIR-T `Identifier` alphabet. Every character
// outside the alphabet becomes `_`, and a non-letter start is prefixed, so the
// result is always lexable.
std::string Sanitize(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 1);
  for (char c : text) {
    out.push_back(IsIdentifierChar(c) ? c : '_');
  }
  if (out.empty()) {
    return "unnamed";
  }
  const char front = out.front();
  if (!((front >= 'a' && front <= 'z') || (front >= 'A' && front <= 'Z'))) {
    out.insert(out.begin(), '_');
  }
  return out;
}

std::string DigestPrefix(const core::StableId& id) {
  return id.digest_hex.substr(
      0, std::min<std::size_t>(8, id.digest_hex.size()));
}

std::string StableText(const core::StableId& id) { return core::ToString(id); }

// A case-local `core::StableId` for a purely textual identity. The program
// bindings `build::ProgramContext` cannot carry are content-addressed here so
// the case's `Dependency` members still name a canonical identity.
core::StableId ModelId(std::string_view text) {
  return core::MakeStableId(
      core::IdKind::kModel,
      std::as_bytes(std::span<const char>(text.data(), text.size())));
}

// Assigns collision-free handles in call order. The caller drives the order
// from a sorted stable-ID sequence, so the assignment is deterministic.
class IdAllocator {
 public:
  std::string Allocate(std::string base) {
    if (used_.insert(base).second) {
      return base;
    }
    for (std::size_t suffix = 2;; ++suffix) {
      std::string candidate = base + "_" + std::to_string(suffix);
      if (used_.insert(candidate).second) {
        return candidate;
      }
    }
  }

 private:
  std::set<std::string, std::less<>> used_;
};

// --- Expression builders ----------------------------------------------------

Expression Reference(const std::string& local_id) {
  Expression expression;
  expression.kind = Expression::Kind::kReference;
  expression.text = local_id;
  return expression;
}

Expression StringLiteral(std::string text) {
  Expression expression;
  expression.kind = Expression::Kind::kString;
  expression.text = std::move(text);
  return expression;
}

Expression Call(std::string callee, std::vector<Expression> arguments) {
  Expression expression;
  expression.kind = Expression::Kind::kCall;
  expression.text = std::move(callee);
  expression.operands = std::move(arguments);
  return expression;
}

Expression Compare(std::string op, Expression left, Expression right) {
  Expression expression;
  expression.kind = Expression::Kind::kCompare;
  expression.text = std::move(op);
  expression.operands.push_back(std::move(left));
  expression.operands.push_back(std::move(right));
  return expression;
}

Expression ForAll(std::string variable, Expression domain, Expression body) {
  Expression expression;
  expression.kind = Expression::Kind::kForAll;
  expression.text = std::move(variable);
  expression.operands.push_back(std::move(domain));
  expression.operands.push_back(std::move(body));
  return expression;
}

Expression ValueOf(const std::string& local_id) {
  return Call("value", {Reference(local_id)});
}

Expression CapacityOf(const std::string& local_id) {
  return Call("capacity", {Reference(local_id)});
}

Expression WithinCapacity(const std::string& value_id,
                          const std::string& memory_id) {
  return Compare("<=", ValueOf(value_id), CapacityOf(memory_id));
}

// Collects every case-local handle an expression references.
void CollectReferences(const Expression& expression,
                       std::set<std::string>* out) {
  if (expression.kind == Expression::Kind::kReference) {
    out->insert(expression.text);
  }
  for (const Expression& operand : expression.operands) {
    CollectReferences(operand, out);
  }
}

// True when `expression` names `local_id` anywhere inside it.
bool Mentions(const Expression& expression, const std::string& local_id) {
  if (expression.kind == Expression::Kind::kReference &&
      expression.text == local_id) {
    return true;
  }
  for (const Expression& operand : expression.operands) {
    if (Mentions(operand, local_id)) {
      return true;
    }
  }
  return false;
}

// --- Text helpers -----------------------------------------------------------

std::vector<std::string> SplitComma(std::string_view text) {
  std::vector<std::string> parts;
  if (text.empty()) {
    return parts;
  }
  std::size_t start = 0;
  for (std::size_t i = 0; i <= text.size(); ++i) {
    if (i == text.size() || text[i] == ',') {
      parts.emplace_back(text.substr(start, i - start));
      start = i + 1;
    }
  }
  return parts;
}

std::string Join(const std::vector<std::string>& parts, std::string_view sep) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) {
      out.append(sep);
    }
    out.append(parts[i]);
  }
  return out;
}

// --- Cell accessors ---------------------------------------------------------

const std::string* AsString(const facts::SemanticCellValue& cell) {
  return std::get_if<std::string>(&cell);
}

const std::uint64_t* AsUint64(const facts::SemanticCellValue& cell) {
  return std::get_if<std::uint64_t>(&cell);
}

const sem::EpistemicState* AsEpistemic(const facts::SemanticCellValue& cell) {
  return std::get_if<sem::EpistemicState>(&cell);
}

// --- Enum projections -------------------------------------------------------

std::optional<EntityKind> EntityKindFor(cpg::NodeKind kind) {
  switch (kind) {
    case cpg::NodeKind::kFunction:
      return EntityKind::kFunction;
    case cpg::NodeKind::kParameter:
      return EntityKind::kValue;
    case cpg::NodeKind::kGlobal:
    case cpg::NodeKind::kMemoryObject:
      return EntityKind::kMemoryObject;
    case cpg::NodeKind::kCallSite:
      return EntityKind::kCallSite;
    case cpg::NodeKind::kBasicBlockSummary:
      return EntityKind::kBasicBlock;
    case cpg::NodeKind::kSummary:
    case cpg::NodeKind::kUnknown:
      // A summary node and an unknown node are not evidence entities; a caller
      // that wants one declared must supply a node the model can carry.
      return std::nullopt;
  }
  return std::nullopt;
}

// The entity kind a bare fact reference implies when the flow slice supplied no
// node for it. The stable-ID kind is the only authority consulted.
EntityKind EntityKindForRef(core::IdKind kind) {
  switch (kind) {
    case core::IdKind::kFunctionVariant:
    case core::IdKind::kFunctionSymbol:
    case core::IdKind::kFunctionBody:
    case core::IdKind::kFunctionSummary:
      return EntityKind::kFunction;
    case core::IdKind::kCallSite:
      return EntityKind::kCallSite;
    case core::IdKind::kMemoryRef:
      return EntityKind::kMemoryObject;
    case core::IdKind::kBasicBlockSummary:
      return EntityKind::kBasicBlock;
    default:
      return EntityKind::kValue;
  }
}

std::string_view EntityKindSlug(EntityKind kind) {
  switch (kind) {
    case EntityKind::kFunction:
      return "function";
    case EntityKind::kCallSite:
      return "call_site";
    case EntityKind::kValue:
      return "value";
    case EntityKind::kMemoryObject:
      return "memory_object";
    case EntityKind::kBasicBlock:
      return "basic_block";
    case EntityKind::kUnspecified:
      return "unspecified";
  }
  return "unspecified";
}

struct EdgeProjection {
  RelationKind kind;
  EpistemicState epistemic;
};

std::optional<EdgeProjection> ProjectEdge(const cpg::CpgEdge& edge) {
  switch (edge.kind) {
    case cpg::EdgeKind::kCalls:
      return EdgeProjection{RelationKind::kCalls, EpistemicState::kMust};
    case cpg::EdgeKind::kMayCall:
      return EdgeProjection{RelationKind::kCalls, EpistemicState::kMay};
    case cpg::EdgeKind::kReads:
      return EdgeProjection{RelationKind::kReads, EpistemicState::kMust};
    case cpg::EdgeKind::kWrites:
      return EdgeProjection{RelationKind::kWrites, EpistemicState::kMust};
    case cpg::EdgeKind::kFlowsTo:
      return EdgeProjection{RelationKind::kFlowsTo, EpistemicState::kMust};
    case cpg::EdgeKind::kAliases:
      switch (edge.alias_state.value_or(cpg::AliasState::kUnknownAlias)) {
        case cpg::AliasState::kMustAlias:
          return EdgeProjection{RelationKind::kMayAlias, EpistemicState::kMust};
        case cpg::AliasState::kMayAlias:
          return EdgeProjection{RelationKind::kMayAlias, EpistemicState::kMay};
        case cpg::AliasState::kNoAlias:
          return EdgeProjection{RelationKind::kMayAlias,
                                EpistemicState::kMustNot};
        case cpg::AliasState::kUnknownAlias:
          return EdgeProjection{RelationKind::kMayAlias,
                                EpistemicState::kUnknown};
      }
      return std::nullopt;
    case cpg::EdgeKind::kContains:
    case cpg::EdgeKind::kDeclares:
    case cpg::EdgeKind::kDominatesSummary:
    case cpg::EdgeKind::kSummarizedBy:
    case cpg::EdgeKind::kUnknownAt:
      // The model carries no relation kind for these. The projection refuses
      // rather than dropping the edge and leaving the graph silently thinner.
      return std::nullopt;
  }
  return std::nullopt;
}

// The closed-code classification of an M9 unknown-effect reason sentence. The
// sentence is M9 free text; the M10B analyzer catalog spells it in upper snake
// case. A sentence the catalog does not name stays an unresolved call, which is
// the weakest claim the case can make about it — never a stronger one.
UnknownReasonCode ReasonCodeForText(std::string_view text) {
  if (text == "EXTERNAL_FUNCTION") return UnknownReasonCode::kExternalFunction;
  if (text == "UNRESOLVED_CALL") return UnknownReasonCode::kUnresolvedCall;
  if (text == "UNKNOWN_ALIAS") return UnknownReasonCode::kUnknownAlias;
  if (text == "MISSING_SPECIFICATION") {
    return UnknownReasonCode::kMissingSpecification;
  }
  if (text == "UNSUPPORTED_LANGUAGE_FEATURE") {
    return UnknownReasonCode::kUnsupportedLanguageFeature;
  }
  if (text == "INLINE_ASSEMBLY") return UnknownReasonCode::kInlineAssembly;
  if (text == "DYNAMIC_LOADING") return UnknownReasonCode::kDynamicLoading;
  if (text == "UNKNOWN_BUILD_CONFIGURATION") {
    return UnknownReasonCode::kUnknownBuildConfiguration;
  }
  return UnknownReasonCode::kUnresolvedCall;
}

// --- Certificate reading ----------------------------------------------------

// The nine canonical cells of evidence.query_completion.v1, read back into the
// descriptor they were written from. Re-deriving the fact from this descriptor
// is how the builder proves the certificate it was handed was not patched after
// publication — a task no M10B accessor performs.
StatusOr<QueryCompletionDescriptor> ReadCompletionDescriptor(
    const facts::AnalysisFact& fact) {
  if (fact.row.relation != facts::RelationId::kQueryCompletion) {
    return Status::InvalidArgument(
        "a query completion certificate must be an "
        "evidence.query_completion.v1 row");
  }
  const auto& cells = fact.row.cells;
  if (cells.size() != 9) {
    return Status::InvalidArgument(
        "an evidence.query_completion.v1 row carries nine cells");
  }
  const std::string* query_kind = AsString(cells[0]);
  const std::string* scope_text = AsString(cells[1]);
  const std::string* budget_text = AsString(cells[2]);
  const std::string* version = AsString(cells[3]);
  const std::string* fingerprint = AsString(cells[4]);
  const std::string* completeness_text = AsString(cells[5]);
  const std::string* reasons_text = AsString(cells[6]);
  const std::uint64_t* examined = AsUint64(cells[7]);
  const std::string* digest = AsString(cells[8]);
  if (query_kind == nullptr || scope_text == nullptr ||
      budget_text == nullptr || version == nullptr || fingerprint == nullptr ||
      completeness_text == nullptr || reasons_text == nullptr ||
      examined == nullptr || digest == nullptr) {
    return Status::InvalidArgument(
        "an evidence.query_completion.v1 row carries the wrong cell types");
  }

  QueryCompletionDescriptor descriptor;
  descriptor.query_kind = *query_kind;
  for (const std::string& text : SplitComma(*scope_text)) {
    auto ref = core::ParseStableId(text);
    if (!ref.ok()) {
      return ref.status();
    }
    descriptor.ordered_scope_refs.push_back(std::move(ref).value());
  }
  const std::vector<std::string> budget_parts = SplitComma(*budget_text);
  if (budget_parts.size() != 5) {
    return Status::InvalidArgument(
        "an evidence.query_completion.v1 budget cell carries five limits");
  }
  std::size_t* limits[5] = {&descriptor.budget.max_depth,
                            &descriptor.budget.max_nodes,
                            &descriptor.budget.max_paths,
                            &descriptor.budget.max_facts_per_query,
                            &descriptor.budget.max_provenance_depth};
  for (std::size_t i = 0; i < 5; ++i) {
    if (budget_parts[i].empty()) {
      return Status::InvalidArgument(
          "an evidence.query_completion.v1 budget limit is empty");
    }
    std::size_t value = 0;
    for (char c : budget_parts[i]) {
      if (c < '0' || c > '9') {
        return Status::InvalidArgument(
            "an evidence.query_completion.v1 budget limit is not a number");
      }
      value = value * 10 + static_cast<std::size_t>(c - '0');
    }
    *limits[i] = value;
  }
  descriptor.query_implementation_version = *version;
  descriptor.input_snapshot_fingerprint = *fingerprint;
  auto completeness = ParseQueryCompleteness(*completeness_text);
  if (!completeness.ok()) {
    return completeness.status();
  }
  descriptor.completeness = completeness.value();
  for (const std::string& text : SplitComma(*reasons_text)) {
    auto reason = ParseTruncationReason(text);
    if (!reason.ok()) {
      return reason.status();
    }
    descriptor.ordered_truncation_reasons.push_back(std::move(reason).value());
  }
  descriptor.examined_items = static_cast<std::size_t>(*examined);
  descriptor.returned_member_digest = *digest;
  return descriptor;
}

// --- The build --------------------------------------------------------------

// One selected witness, read from the handoff's proto closure. The typed
// `facts::FactWitness` never crosses the M10B boundary; the proto form does.
struct WitnessView {
  std::string producer_id;
  std::string rule_id;
  std::string rule_version;
  std::string source_anchor_id;
};

// The whole build. Every member is derived from the request; nothing is carried
// over from a previous build, so two builds cannot influence one another.
class Builder {
 public:
  explicit Builder(const EvidenceBuildRequest& request) : request_(request) {}

  StatusOr<EvidenceCase> Run() {
    Status status = CheckRequest();
    if (!status.ok()) return status;
    status = ResolveRun();
    if (!status.ok()) return status;
    status = AssignEntities();
    if (!status.ok()) return status;
    status = BuildProvenance();
    if (!status.ok()) return status;
    status = RouteFacts();
    if (!status.ok()) return status;
    status = BuildGraph();
    if (!status.ok()) return status;
    status = BuildDominatingCheck();
    if (!status.ok()) return status;
    status = BuildClaimSupport();
    if (!status.ok()) return status;
    status = BuildSummaryExpansions();
    if (!status.ok()) return status;

    EvidenceCase value;
    value.schema_version = std::string(kEvidenceSchemaVersion);
    value.level = request_.level;
    value.program = BuildProgramBinding();
    value.primary_claim = claim_;
    value.entities = entities_;
    value.edges = edges_;
    value.paths = paths_;
    value.facts = facts_;
    value.unknowns = unknowns_;
    value.constraints = constraints_;
    value.provenance = provenance_;
    value.proof_obligations = obligations_;
    value.dependencies = dependencies_;
    value.omissions = omissions_;
    value.verification_state = VerificationState::kPossibleDefect;

    status = Project(&value);
    if (!status.ok()) return status;
    status = RetargetBlocking(&value);
    if (!status.ok()) return status;

    SortCase(&value);
    Status finalized = FinalizeEvidenceIdentity(&value);
    if (!finalized.ok()) {
      return finalized;
    }
    return value;
  }

 private:
  // --- Request-level checks ------------------------------------------------

  Status CheckRequest() const {
    if (request_.level == EvidenceLevel::kUnspecified) {
      return Status::InvalidArgument(
          "the build request declares no evidence level");
    }
    if (request_.input.claim_seed.kind != ClaimKind::kBufferOverflow) {
      return Status::InvalidArgument(
          "M10C constructs only the buffer-overflow claim; the input declares "
          "another claim kind");
    }
    const build::ProgramContext& context = request_.context;
    const std::pair<std::string_view, const std::string*> required[] = {
        {"repository_id", &context.repository_id},
        {"revision_id", &context.revision_id},
        {"build_variant_id", &context.build_variant_id},
        {"target_triple", &context.target_triple},
        {"type_layout_hash", &context.type_layout_hash},
        {"analysis_configuration_id", &request_.analysis_configuration_id},
    };
    for (const auto& [name, value] : required) {
      if (value->empty()) {
        return Status::InvalidArgument(
            "the build request carries an empty " + std::string(name) +
            "; a case cannot be bound to a program identity it does not have");
      }
    }
    return Status::Ok();
  }

  // The run is carried four times over — the flow slice's metadata, every fact
  // set's metadata, the provenance graph, and each completion binding — so
  // agreement is checkable rather than assumed. A mixed input is rejected,
  // never rebased (spec 4.4, BLD-010).
  Status ResolveRun() {
    const EvidenceBuildInput& input = request_.input;
    std::optional<core::StableId> run;

    auto note = [&run](const core::StableId& candidate,
                       std::string_view carrier) -> Status {
      if (candidate.digest_hex.empty()) {
        return Status::Ok();
      }
      if (!run.has_value()) {
        run = candidate;
        return Status::Ok();
      }
      if (*run != candidate) {
        return Status::InvalidArgument(
            "the evidence input mixes analysis runs: " + std::string(carrier) +
            " belongs to " + StableText(candidate) +
            " but the case is bound to " + StableText(*run));
      }
      return Status::Ok();
    };

    Status status =
        note(input.flow_slice.metadata.analysis_run_id, "the flow slice");
    if (!status.ok()) return status;
    const std::pair<const char*, const EvidenceFactSet*> sets[] = {
        {"the range result", &input.ranges},
        {"the capacity result", &input.capacities},
        {"the alias result", &input.aliases},
        {"the dominating-check result", &input.dominating_checks},
        {"the unknown result", &input.unknowns},
    };
    for (const auto& [name, set] : sets) {
      status = note(set->metadata.analysis_run_id, name);
      if (!status.ok()) return status;
    }
    if (!input.provenance.run_id().empty()) {
      auto parsed = core::ParseStableId(input.provenance.run_id());
      if (!parsed.ok()) return parsed.status();
      status = note(parsed.value(), "the provenance graph");
      if (!status.ok()) return status;
    }
    for (const facts::RunFactBinding& binding :
         input.query_completion_bindings) {
      status = note(binding.run_id, "a query-completion binding");
      if (!status.ok()) return status;
    }
    if (!run.has_value()) {
      return Status::InvalidArgument(
          "the evidence input names no analysis run anywhere, so no case can "
          "be bound to one");
    }
    run_id_ = *run;
    return Status::Ok();
  }

  // --- Entities ------------------------------------------------------------

  Status NoteStable(const core::StableId& id, std::optional<EntityKind> kind,
                    std::string_view label) {
    if (id.digest_hex.empty()) {
      return Status::InvalidArgument(
          "the evidence input references an empty stable ID");
    }
    const std::string text = StableText(id);
    auto it = plans_.find(text);
    if (it == plans_.end()) {
      EntityPlan plan;
      plan.kind = kind.has_value() ? *kind : EntityKindForRef(id.kind);
      plan.label = std::string(label);
      plans_.emplace(text, std::move(plan));
      return Status::Ok();
    }
    // A flow-slice node is the authority on its own kind and label; a bare fact
    // reference only fills a gap.
    if (kind.has_value() && !label.empty()) {
      it->second.kind = *kind;
      it->second.label = std::string(label);
    }
    return Status::Ok();
  }

  Status NoteFactRefs(const facts::AnalysisFact& fact) {
    for (const facts::SemanticCellValue& cell : fact.row.cells) {
      const core::StableId* stable = std::get_if<core::StableId>(&cell);
      if (stable == nullptr) {
        continue;
      }
      Status status = NoteStable(*stable, std::nullopt, {});
      if (!status.ok()) return status;
    }
    return Status::Ok();
  }

  // Assigns every handle in canonical stable-ID order. A stable ID the flow
  // slice supplied a node for gets `E_<label>` — the label is the semantic name
  // the analysis gave the node, never a source filename or a line number. A
  // stable ID only a fact references has no label, so its handle is its kind
  // plus the first eight digest characters. Either way the assignment is a pure
  // function of the sorted reference set, which is what makes the build
  // insertion-order independent.
  Status AssignEntities() {
    const EvidenceBuildInput& input = request_.input;

    for (const cpg::CpgNode& node : input.flow_slice.nodes) {
      std::optional<EntityKind> kind = EntityKindFor(node.kind);
      if (!kind.has_value()) {
        return Status::InvalidArgument(
            "the flow slice carries a node whose kind has no EIR entity kind");
      }
      Status status = NoteStable(node.node_id, kind, node.label);
      if (!status.ok()) return status;
    }

    const core::StableId* claim_refs[] = {&input.claim_seed.subject_ref,
                                          &input.claim_seed.source_ref,
                                          &input.claim_seed.sink_ref};
    for (const core::StableId* ref : claim_refs) {
      Status status = NoteStable(*ref, std::nullopt, {});
      if (!status.ok()) return status;
    }

    const std::vector<const EvidenceFactSet*> sets = {
        &input.ranges, &input.capacities, &input.aliases, &input.unknowns,
        &input.dominating_checks};
    for (const EvidenceFactSet* set : sets) {
      for (const facts::AnalysisFact& fact : set->facts) {
        Status status = NoteFactRefs(fact);
        if (!status.ok()) return status;
      }
    }
    for (const facts::AnalysisFact& fact : input.flow_slice.supporting_facts) {
      Status status = NoteFactRefs(fact);
      if (!status.ok()) return status;
    }
    for (const facts::AnalysisFact& fact : input.flow_slice.contradicting_facts) {
      Status status = NoteFactRefs(fact);
      if (!status.ok()) return status;
    }
    for (const facts::AnalysisFact& fact : input.flow_slice.unknowns) {
      Status status = NoteFactRefs(fact);
      if (!status.ok()) return status;
    }

    IdAllocator allocator;
    for (const auto& [text, plan] : plans_) {
      auto stable = core::ParseStableId(text);
      if (!stable.ok()) return stable.status();
      std::string base;
      if (!plan.label.empty()) {
        base = "E_" + Sanitize(plan.label);
      } else {
        base = "E_" + std::string(EntityKindSlug(plan.kind)) + "_" +
               DigestPrefix(stable.value());
      }
      const std::string local = allocator.Allocate(std::move(base));
      Entity entity;
      entity.id = local;
      entity.kind = plan.kind;
      entity.stable_id = stable.value();
      local_by_stable_.emplace(text, local);
      resolver_.Bind(stable.value(), local);
      entities_.push_back(std::move(entity));
    }
    return Status::Ok();
  }

  const std::string* LocalFor(const core::StableId& id) const {
    auto it = local_by_stable_.find(StableText(id));
    if (it == local_by_stable_.end()) {
      return nullptr;
    }
    return &it->second;
  }

  StatusOr<std::string> LocalForOrFail(const core::StableId& id,
                                       std::string_view role) const {
    const std::string* local = LocalFor(id);
    if (local == nullptr) {
      return Status::InvalidArgument(
          std::string(role) + " names the stable ID '" + StableText(id) +
          "', which the case declares no entity for");
    }
    return *local;
  }

  // --- Provenance ----------------------------------------------------------

  // One record per completion certificate. Everything the record states comes
  // from the witness, the binding, or the request: the rule is the witness's,
  // the anchor is the witness's, and the producer is the witness's producer
  // translated into EIR's vocabulary (ruling L49) — never a literal, and never
  // a copy of an identifier EIR-T cannot spell.
  Status BuildProvenance() {
    const EvidenceBuildInput& input = request_.input;

    for (const facts::RunFactBinding& binding :
         input.query_completion_bindings) {
      binding_by_fact_.emplace(StableText(binding.fact_id), &binding);
    }
    for (const fact::v1::FactWitness& proto : input.provenance.nodes()) {
      if (!proto.selected()) {
        continue;
      }
      WitnessView view;
      view.producer_id = proto.producer_id();
      view.rule_id = proto.rule_id();
      view.rule_version = proto.rule_version();
      view.source_anchor_id = proto.source_anchor_id();
      witness_by_fact_.emplace(proto.output_fact_id(), std::move(view));
    }

    std::vector<const facts::AnalysisFact*> certificates;
    certificates.reserve(input.query_completion_facts.size());
    for (const facts::AnalysisFact& fact : input.query_completion_facts) {
      certificates.push_back(&fact);
    }
    std::sort(certificates.begin(), certificates.end(),
              [](const facts::AnalysisFact* a, const facts::AnalysisFact* b) {
                return a->fact_id < b->fact_id;
              });

    IdAllocator allocator;
    for (const facts::AnalysisFact* fact : certificates) {
      auto descriptor = ReadCompletionDescriptor(*fact);
      if (!descriptor.ok()) return descriptor.status();
      // The certificate must re-derive from its own cells: a row whose budget,
      // completeness, or returned-member digest was edited after publication
      // does not, and a case may not rest on a certificate that fails its own
      // identity check.
      auto rederived = MakeQueryCompletionFact(descriptor.value());
      if (!rederived.ok()) return rederived.status();
      if (rederived.value().fact_id != fact->fact_id) {
        return Status::InvalidArgument(
            "the completion certificate '" + StableText(fact->fact_id) +
            "' does not re-derive from its own cells");
      }

      const std::string text = StableText(fact->fact_id);
      Provenance record;
      record.id =
          allocator.Allocate("PR_" + Sanitize(descriptor.value().query_kind));
      // The EIR spelling of the M9 query producer. A witness that names a
      // producer overrides it; the default is the translation of the M10B
      // constant, so the case never carries the hyphenated M9 string.
      record.producer = TranslateProducer(kQueryCompletionProducerId);
      record.rule = std::string(kQueryCompletionRuleId);
      record.analysis_run_id = run_id_;
      record.version = std::string(kQueryImplementationVersion);
      record.configuration = request_.analysis_configuration_id;
      const auto witness = witness_by_fact_.find(text);
      if (witness != witness_by_fact_.end()) {
        // The translation that makes the case writable as EIR-T at all.
        record.producer = TranslateProducer(witness->second.producer_id);
        if (!witness->second.rule_id.empty()) {
          record.rule = witness->second.rule_id;
        }
        if (!witness->second.rule_version.empty()) {
          record.version = witness->second.rule_version;
        }
        record.source_anchor_id = witness->second.source_anchor_id;
      }
      provenance_by_fact_.emplace(text, record.id);
      provenance_.push_back(std::move(record));
    }
    return Status::Ok();
  }

  const std::string* ProvenanceForFact(const core::StableId& fact_id) const {
    auto it = provenance_by_fact_.find(StableText(fact_id));
    if (it == provenance_by_fact_.end()) {
      return nullptr;
    }
    return &it->second;
  }

  // --- Fact routing --------------------------------------------------------

  // The bucket order that decides which certificate certifies a fact appearing
  // in more than one bucket. It is fixed, not incidental: a fact's own query is
  // the certificate that returned it, so the domain fact sets come before the
  // flow slice's aggregate views.
  Status RouteFacts() {
    const EvidenceBuildInput& input = request_.input;
    const std::pair<const std::vector<facts::AnalysisFact>*,
                    const core::StableId*>
        buckets[] = {
            {&input.ranges.facts, &input.ranges.metadata.query_provenance_id},
            {&input.capacities.facts,
             &input.capacities.metadata.query_provenance_id},
            {&input.aliases.facts, &input.aliases.metadata.query_provenance_id},
            {&input.dominating_checks.facts,
             &input.dominating_checks.metadata.query_provenance_id},
            {&input.unknowns.facts,
             &input.unknowns.metadata.query_provenance_id},
            {&input.flow_slice.supporting_facts,
             &input.flow_slice.metadata.query_provenance_id},
            {&input.flow_slice.contradicting_facts,
             &input.flow_slice.metadata.query_provenance_id},
            {&input.flow_slice.unknowns,
             &input.flow_slice.metadata.query_provenance_id},
        };

    std::map<std::string, const facts::AnalysisFact*> routed;
    std::map<std::string, core::StableId> certificate;
    for (const auto& [facts, provenance_of_set] : buckets) {
      for (const facts::AnalysisFact& fact : *facts) {
        const std::string text = StableText(fact.fact_id);
        if (!routed.emplace(text, &fact).second) {
          continue;
        }
        certificate.emplace(text, *provenance_of_set);
      }
    }

    EvidencePredicateMapper mapper;
    for (const auto& [text, fact] : routed) {
      auto certificate_it = certificate.find(text);
      const std::string* provenance_id =
          certificate_it != certificate.end()
              ? ProvenanceForFact(certificate_it->second)
              : nullptr;
      const std::string provenance_local =
          provenance_id != nullptr ? *provenance_id : std::string();

      if (fact->row.relation == facts::RelationId::kUnknownEffect) {
        Status status = AddUnknown(*fact);
        if (!status.ok()) return status;
        continue;
      }
      if (fact->row.relation == facts::RelationId::kSoundnessCoverage) {
        coverage_facts_.push_back(fact);
        continue;
      }

      // A relation with no built-in mapping has no spelling to name a handle
      // after, and the mapper is the authority that refuses it. The handle is
      // left unnamed so the refusal comes from `MapFact` — the same typed
      // failure every unmappable relation produces, and never a dereference of
      // an empty mapping (BLD-010).
      const std::optional<std::string_view> predicate =
          EvidencePredicateMapper::PredicateName(fact->row.relation);
      FactMappingHandle handle;
      if (predicate.has_value()) {
        handle.local_id = fact_ids_.Allocate("F_" + Sanitize(*predicate));
      }
      handle.provenance_id = provenance_local;
      auto mapped = mapper.MapFact(*fact, resolver_, handle);
      if (!mapped.ok()) {
        return mapped.status();
      }
      Fact record = std::move(mapped).value();
      if (record.predicate.kind == Expression::Kind::kCall &&
          record.predicate.text == "alias" &&
          record.predicate.operands.size() == 2 &&
          record.predicate.operands[0].kind == Expression::Kind::kReference &&
          record.predicate.operands[1].kind == Expression::Kind::kReference) {
        alias_links_.push_back(AliasLink{record.predicate.operands[0].text,
                                         record.predicate.operands[1].text,
                                         record.epistemic});
      }
      facts_.push_back(std::move(record));
    }
    return Status::Ok();
  }

  // --- Unknown members -----------------------------------------------------

  Status AddUnknown(const facts::AnalysisFact& fact) {
    const auto& cells = fact.row.cells;
    if (cells.size() != 4) {
      return Status::InvalidArgument("an UnknownEffect row carries four cells");
    }
    const core::StableId* function_id = std::get_if<core::StableId>(&cells[0]);
    const std::string* subject = AsString(cells[1]);
    const std::string* reason = AsString(cells[2]);
    if (function_id == nullptr || subject == nullptr || reason == nullptr) {
      return Status::InvalidArgument("an UnknownEffect row has the wrong cells");
    }
    auto function_local = LocalForOrFail(*function_id, "an unknown-effect row");
    if (!function_local.ok()) return function_local.status();

    const UnknownReasonCode code = ReasonCodeForText(*reason);
    Unknown unknown;
    unknown.id = unknown_ids_.Allocate("U_" + Sanitize(*subject) + "_" +
                                       Sanitize(ToString(code)));
    unknown.property = Call("effect_of", {Reference(function_local.value())});
    unknown.reason_code = code;
    unknown.reason = *reason;
    unknown.suggested_resolution = "infer_effect(" + function_local.value() + ")";
    unknowns_.push_back(std::move(unknown));

    // The open question and the marker that says a higher level can close it
    // are both present, so absence is never the only representation.
    Omission omission;
    omission.id =
        omission_ids_.Allocate("OM_analyzer_expansion_" + Sanitize(*subject));
    omission.kind = "analyzer_expansion";
    omission.subject = unknowns_.back().id;
    omission.reason = "the analysis did not model this function's effect";
    omission.expandable = false;
    omissions_.push_back(std::move(omission));
    return Status::Ok();
  }

  // --- Graph ---------------------------------------------------------------

  Status BuildGraph() {
    Status status = BuildEdges();
    if (!status.ok()) return status;
    return BuildPaths();
  }

  Status BuildEdges() {
    const EvidenceBuildInput& input = request_.input;
    const std::string* flow_provenance =
        ProvenanceForFact(input.flow_slice.metadata.query_provenance_id);

    std::vector<const cpg::CpgEdge*> edges;
    edges.reserve(input.flow_slice.edges.size());
    for (const cpg::CpgEdge& edge : input.flow_slice.edges) {
      edges.push_back(&edge);
    }
    std::sort(edges.begin(), edges.end(),
              [](const cpg::CpgEdge* a, const cpg::CpgEdge* b) {
                return a->edge_id < b->edge_id;
              });

    for (const cpg::CpgEdge* edge : edges) {
      std::optional<EdgeProjection> projection = ProjectEdge(*edge);
      if (!projection.has_value()) {
        return Status::InvalidArgument(
            "the flow slice carries an edge kind the case cannot represent");
      }
      // An expandable edge is a summary edge: the validator requires it to name
      // the summary reference whose expansion was withheld, and a case that
      // names one it was never given would be asserting a derivation it cannot
      // show. The handoff carries no summary identity, so the edge is refused
      // rather than silently closed.
      if (edge->expandable) {
        return Status::InvalidArgument(
            "the flow slice carries an expandable summary edge, but the "
            "handoff carries no summary reference to name the expansion that "
            "was withheld");
      }
      auto from = LocalForOrFail(edge->source_node_id, "a flow edge");
      if (!from.ok()) return from.status();
      auto to = LocalForOrFail(edge->target_node_id, "a flow edge");
      if (!to.ok()) return to.status();

      Edge value;
      value.id = edge_ids_.Allocate("ED_" + from.value().substr(2) + "_" +
                                    to.value().substr(2));
      value.from = from.value();
      value.to = to.value();
      value.kind = projection->kind;
      value.epistemic = projection->epistemic;
      value.provenance_id =
          flow_provenance != nullptr ? *flow_provenance : std::string();
      value.expandable = false;
      edges_.push_back(std::move(value));
    }
    return Status::Ok();
  }

  // The primary path is the ordered `kFlowsTo` chain the flow slice returned.
  // The chain's order is semantic and is preserved verbatim: no canonicalizer
  // reorders `entity_ids`.
  Status BuildPaths() {
    const EvidenceBuildInput& input = request_.input;
    std::map<std::string, std::string> successor;
    std::map<std::string, std::size_t> indegree;
    for (const Edge& edge : edges_) {
      if (edge.kind != RelationKind::kFlowsTo) {
        continue;
      }
      successor.emplace(edge.from, edge.to);
      indegree.emplace(edge.from, 0);
      indegree[edge.to] += 1;
    }
    if (successor.empty()) {
      return Status::Ok();
    }

    std::vector<std::string> chain;
    std::size_t starts = 0;
    for (const auto& [node, degree] : indegree) {
      if (degree != 0) {
        continue;
      }
      ++starts;
      if (!chain.empty()) {
        continue;
      }
      for (std::string cursor = node;;) {
        chain.push_back(cursor);
        auto next = successor.find(cursor);
        if (next == successor.end()) {
          break;
        }
        cursor = next->second;
      }
    }
    // A path is a connected chain: the validator requires every consecutive
    // pair to be joined by an edge of the path's relation kind, so a
    // one-segment or disconnected chain is not a path and is not emitted.
    if (chain.size() < 2) {
      return Status::Ok();
    }

    Path path;
    path.id = std::string(kPathLocalId);
    path.kind = PathKind::kValueFlow;
    path.entity_ids = chain;
    path.feasibility = Feasibility::kSat;
    const std::string* flow_provenance =
        ProvenanceForFact(input.flow_slice.metadata.query_provenance_id);
    path.provenance_id =
        flow_provenance != nullptr ? *flow_provenance : std::string();

    // BLD-005: an alias the analysis could not establish as MUST is carried as
    // a *condition* on the path, never promoted to a supporting premise. The
    // case records the uncertainty where it belongs — on the path that depends
    // on it — instead of strengthening it.
    for (const AliasLink& link : alias_links_) {
      if (link.epistemic == EpistemicState::kMust) {
        continue;
      }
      path.conditions.push_back(
          Call("alias", {Reference(link.left), Reference(link.right)}));
    }
    path_first_ = chain.front();
    paths_.push_back(std::move(path));

    if (starts > 1) {
      // More than one chain reached the slice. The case states the one it
      // followed and marks the rest, so a sibling path never becomes a
      // universal claim.
      Omission omission;
      omission.id =
          omission_ids_.Allocate("OM_mixed_paths_" + Sanitize(chain.back()));
      omission.kind = "mixed_paths";
      omission.subject = paths_.back().id;
      omission.reason =
          "the flow slice returned more than one distinct value-flow chain, so "
          "the path states one of them and not all";
      omission.expandable = true;
      omissions_.push_back(std::move(omission));
    }
    return Status::Ok();
  }

  // --- The dominating-check query ------------------------------------------

  // BLD-002/003/006/007. A closed-world absence is derivable only from a
  // certificate that re-derives, a completeness of `kComplete`, an empty
  // result, a scope that pins the claim's own sink, the case's run, and a
  // selected witness. Everything weaker leaves the question open.
  Status BuildDominatingCheck() {
    const EvidenceBuildInput& input = request_.input;
    const core::StableId& sink_ref = input.claim_seed.sink_ref;
    auto sink_local = LocalForOrFail(sink_ref, "the claim's sink");
    if (!sink_local.ok()) return sink_local.status();
    observation_sink_local_ = sink_local.value();

    const core::StableId certificate =
        input.dominating_checks.metadata.query_provenance_id;
    const facts::AnalysisFact* fact = nullptr;
    for (const facts::AnalysisFact& candidate : input.query_completion_facts) {
      if (candidate.fact_id == certificate) {
        fact = &candidate;
        break;
      }
    }
    if (fact == nullptr) {
      return OpenCheck(
          "the dominating-check result names a completion certificate the "
          "handoff does not carry",
          UnknownReasonCode::kMissingSpecification, sink_local.value(),
          "OM_dominating_check_certificate");
    }

    // The certificate already re-derived: `BuildProvenance` re-derives every
    // certificate the handoff carries and refuses the whole handoff when one
    // does not, so no certificate reaching this point was patched after
    // publication. The re-derivation is validated exactly once, there, and this
    // method relies on that rather than repeating it.
    auto descriptor = ReadCompletionDescriptor(*fact);
    if (!descriptor.ok()) return descriptor.status();

    // The query's own scope must pin the sink the absence is claimed about. A
    // result returned for a sibling scope says nothing about this sink, so it
    // is never promoted to a universal check over it (BLD-003).
    const bool scope_pins_sink =
        std::find(descriptor.value().ordered_scope_refs.begin(),
                  descriptor.value().ordered_scope_refs.end(),
                  sink_ref) != descriptor.value().ordered_scope_refs.end();
    if (!scope_pins_sink) {
      return OpenCheck(
          "the dominating-check query was scoped to another member, so its "
          "result says nothing about the sink",
          UnknownReasonCode::kUnresolvedCall, sink_local.value(),
          "OM_sibling_check_scope");
    }

    const std::string certificate_text = StableText(fact->fact_id);
    auto binding = binding_by_fact_.find(certificate_text);
    if (binding == binding_by_fact_.end() ||
        binding->second->run_id != run_id_) {
      return OpenCheck(
          "the dominating-check certificate belongs to another analysis run",
          UnknownReasonCode::kMissingSpecification, sink_local.value(),
          "OM_dominating_check_run");
    }
    if (witness_by_fact_.find(certificate_text) == witness_by_fact_.end()) {
      return OpenCheck(
          "the dominating-check certificate carries no selected witness in the "
          "handoff's provenance closure",
          UnknownReasonCode::kMissingSpecification, sink_local.value(),
          "OM_dominating_check_witness");
    }

    const std::string* provenance_local =
        ProvenanceForFact(input.dominating_checks.metadata.query_provenance_id);

    if (descriptor.value().completeness == QueryCompleteness::kTruncated) {
      // BLD-006: a truncated empty result is a withheld answer, never negative
      // evidence. It becomes an explicit unknown naming the budget that cut it
      // short, plus the omission that can recover it.
      std::vector<std::string> reasons;
      for (TruncationReason reason :
           descriptor.value().ordered_truncation_reasons) {
        reasons.emplace_back(ToString(reason));
      }
      const std::string reason_text =
          "dominating-check query truncated: " + Join(reasons, ",");
      Unknown unknown;
      unknown.id = unknown_ids_.Allocate(
          "U_" + Sanitize(sink_local.value().substr(2)) +
          "_missing_specification");
      unknown.property =
          Call(std::string(kAbsencePredicate),
               {Reference(sink_local.value()), Reference(sink_local.value())});
      unknown.reason_code = UnknownReasonCode::kMissingSpecification;
      unknown.reason = reason_text;
      unknown.suggested_resolution =
          "expand_dominating_check_query(" + sink_local.value() + ")";
      unknowns_.push_back(std::move(unknown));

      Omission omission;
      omission.id = omission_ids_.Allocate(
          "OM_truncated_query_" + Sanitize(sink_local.value().substr(2)));
      omission.kind = "truncated_query";
      omission.subject = unknowns_.back().id;
      omission.reason = reason_text;
      omission.expandable = true;
      omissions_.push_back(std::move(omission));
      return Status::Ok();
    }

    if (!input.dominating_checks.facts.empty()) {
      // BLD-002: a check was found. It is counterevidence — the case records it
      // and stops there. It does not conclude the sink is safe, and it does not
      // conclude the check dominates: no verdict follows from presence alone.
      for (const facts::AnalysisFact* row : coverage_facts_) {
        const sem::EpistemicState* state = AsEpistemic(row->row.cells.back());
        const sem::EpistemicState raw =
            state != nullptr ? *state : sem::EpistemicState::kUnknown;
        Fact evidence;
        evidence.id = fact_ids_.Allocate("F_" + std::string(kAbsencePredicate));
        evidence.stable_id = row->fact_id;
        evidence.predicate =
            Call(std::string(kAbsencePredicate),
                 {Reference(sink_local.value()), Reference(sink_local.value())});
        evidence.epistemic = CopyEpistemicState(raw);
        evidence.confidence = ConfidenceForState(evidence.epistemic);
        evidence.producer = std::string(kCoverageProducerId);
        evidence.provenance_id =
            provenance_local != nullptr ? *provenance_local : std::string();
        evidence.derived = raw != sem::EpistemicState::kMust;
        facts_.push_back(std::move(evidence));
      }
      return Status::Ok();
    }

    // BLD-007: the one derivation the builder performs. Complete, empty,
    // correctly scoped, in this run, with a selected witness — so the absence is
    // certified, and the certificate is its provenance input.
    const std::string certificate_local = fact_ids_.Allocate("F_query_completion");
    Fact certificate_fact;
    certificate_fact.id = certificate_local;
    certificate_fact.stable_id = fact->fact_id;
    certificate_fact.predicate = Call(
        "query_completion",
        {StringLiteral(descriptor.value().query_kind),
         StringLiteral(
             std::string(ToString(descriptor.value().completeness)))});
    certificate_fact.epistemic = EpistemicState::kMust;
    certificate_fact.confidence = Confidence::kExact;
    certificate_fact.producer = std::string(kEvidenceQueryProducerId);
    certificate_fact.provenance_id =
        provenance_local != nullptr ? *provenance_local : std::string();
    certificate_fact.derived = false;
    facts_.push_back(certificate_fact);

    Provenance record;
    record.id = std::string(kAbsenceProvenanceId);
    record.producer = std::string(kClosedWorldProducerId);
    record.rule = std::string(kClosedWorldAbsenceRuleId);
    record.input_fact_ids = {certificate_local};
    record.analysis_run_id = run_id_;
    record.version = std::string(kQueryImplementationVersion);
    record.configuration = request_.analysis_configuration_id;
    provenance_.push_back(std::move(record));

    Fact absence;
    absence.id = fact_ids_.Allocate("F_" + std::string(kAbsencePredicate));
    absence.stable_id = fact->fact_id;
    absence.predicate =
        Call(std::string(kAbsencePredicate),
             {Reference(sink_local.value()), Reference(sink_local.value())});
    absence.epistemic = EpistemicState::kMustNot;
    absence.confidence = Confidence::kExact;
    absence.producer = std::string(kClosedWorldProducerId);
    absence.provenance_id = std::string(kAbsenceProvenanceId);
    absence.derived = true;
    facts_.push_back(std::move(absence));
    return Status::Ok();
  }

  // Records an open dominating-check question: an explicit unknown over the
  // sink plus the omission that says a higher level can close it. Never a
  // negative fact, and never an interrupted-query reason code — the case has no
  // query-completion provenance under the M9 producer to pair with one.
  Status OpenCheck(std::string reason, UnknownReasonCode code,
                   const std::string& sink_local, std::string omission_base) {
    Unknown unknown;
    unknown.id = unknown_ids_.Allocate("U_" + Sanitize(sink_local.substr(2)) +
                                       "_" + Sanitize(ToString(code)));
    unknown.property =
        Call(std::string(kAbsencePredicate),
             {Reference(sink_local), Reference(sink_local)});
    unknown.reason_code = code;
    unknown.reason = std::move(reason);
    unknown.suggested_resolution =
        "expand_dominating_check_query(" + sink_local + ")";
    unknowns_.push_back(std::move(unknown));

    Omission omission;
    omission.id = omission_ids_.Allocate(std::move(omission_base));
    omission.kind = "dominating_check";
    omission.subject = unknowns_.back().id;
    omission.reason = unknowns_.back().reason;
    omission.expandable = true;
    omissions_.push_back(std::move(omission));
    return Status::Ok();
  }

  // --- Claim, constraint, obligation, dependencies -------------------------

  Status BuildClaimSupport() {
    const EvidenceBuildInput& input = request_.input;
    auto subject =
        LocalForOrFail(input.claim_seed.subject_ref, "the claim's subject");
    if (!subject.ok()) return subject.status();
    auto source =
        LocalForOrFail(input.claim_seed.source_ref, "the claim's source");
    if (!source.ok()) return source.status();
    auto sink = LocalForOrFail(input.claim_seed.sink_ref, "the claim's sink");
    if (!sink.ok()) return sink.status();

    claim_.id = std::string(kClaimLocalId);
    claim_.kind = input.claim_seed.kind;
    claim_.severity = input.claim_seed.severity;
    claim_.subject = subject.value();
    claim_.predicate =
        Compare(">", ValueOf(source.value()), CapacityOf(subject.value()));
    claim_.description =
        "the copied length is not bounded by the destination capacity at the "
        "sink call";
    claim_source_local_ = source.value();

    Constraint constraint;
    constraint.id = std::string(kConstraintLocalId);
    constraint.expression = WithinCapacity(source.value(), subject.value());
    constraint.scope = "path";
    // The constraint states the *requirement*, not an established fact: it is
    // exactly the predicate the obligation asks a verifier to prove, so it
    // cannot be a MUST.
    constraint.epistemic = EpistemicState::kMay;
    const std::string* flow_provenance =
        ProvenanceForFact(input.flow_slice.metadata.query_provenance_id);
    constraint.provenance_id =
        flow_provenance != nullptr ? *flow_provenance : std::string();
    constraints_.push_back(std::move(constraint));

    ProofObligation obligation;
    obligation.id = std::string(kObligationLocalId);
    obligation.goal_kind = ProofGoalKind::kProve;
    obligation.predicate = ForAll(
        "path",
        Call("feasible_paths", {Reference(path_first_.empty()
                                              ? source.value()
                                              : path_first_),
                                Reference(sink.value())}),
        WithinCapacity(source.value(), subject.value()));
    obligation.verifier_kinds = {"range_analysis", "smt", "symbolic_execution"};
    obligation.status = ProofStatus::kPending;
    obligations_.push_back(std::move(obligation));

    Dependency fact_dependency;
    fact_dependency.id = std::string(kDependencyFactId);
    fact_dependency.kind = DependencyKind::kFact;
    fact_dependency.stable_id =
        input.dominating_checks.metadata.query_provenance_id;
    dependencies_.push_back(std::move(fact_dependency));

    Dependency layout_dependency;
    layout_dependency.id = std::string(kDependencyTypeLayoutId);
    layout_dependency.kind = DependencyKind::kTypeLayout;
    layout_dependency.stable_id = ModelId(request_.context.type_layout_hash);
    dependencies_.push_back(std::move(layout_dependency));

    Dependency configuration_dependency;
    configuration_dependency.id = std::string(kDependencyConfigurationId);
    configuration_dependency.kind = DependencyKind::kConfiguration;
    configuration_dependency.stable_id =
        ModelId(request_.analysis_configuration_id);
    dependencies_.push_back(std::move(configuration_dependency));
    return Status::Ok();
  }

  // --- Summary expansion markers -------------------------------------------

  // BLD-009: a function the case did not expand is reported as an expansion the
  // case could not perform, not left as a silent gap. The marker is a property
  // of the detail level, so it appears only where the detail is claimed.
  Status BuildSummaryExpansions() {
    if (request_.level != EvidenceLevel::kL2) {
      return Status::Ok();
    }
    std::vector<std::string> functions;
    for (const Entity& entity : entities_) {
      if (entity.kind == EntityKind::kFunction) {
        functions.push_back(entity.id);
      }
    }
    std::sort(functions.begin(), functions.end());
    for (const std::string& function : functions) {
      Omission omission;
      omission.id =
          omission_ids_.Allocate("OM_summary_expansion_" + Sanitize(function));
      omission.kind = "summary_expansion";
      omission.subject = function;
      omission.reason =
          "the handoff carries no function summary for this function, so its "
          "components are not expanded at l2";
      omission.expandable = true;
      omissions_.push_back(std::move(omission));
    }
    return Status::Ok();
  }

  // --- Program binding -----------------------------------------------------

  ProgramBinding BuildProgramBinding() const {
    ProgramBinding binding;
    binding.repository_id = request_.context.repository_id;
    binding.revision_id = request_.context.revision_id;
    binding.build_variant_id = request_.context.build_variant_id;
    binding.target_triple = request_.context.target_triple;
    // The two fields `build::ProgramContext` cannot carry (ruling L51). They
    // travel on the request because the caller that opened the snapshot is the
    // only party that knows them; the builder never invents them.
    binding.analysis_configuration_id = request_.analysis_configuration_id;
    binding.type_layout_id = request_.context.type_layout_hash;
    binding.analysis_run_id = run_id_;
    binding.analyzer_versions = request_.analyzer_versions;
    return binding;
  }

  // --- Level projection ----------------------------------------------------

  // Builds one complete case, then projects it down. L0 states only what its
  // own question needs and declares an omission for every member it removed, so
  // a withheld slice is never represented by its absence alone (BLD-008). L1
  // retains the causal slice. L2 retains the detail and adds an explicit
  // expandable marker for every expansion it did not perform (BLD-009).
  Status Project(EvidenceCase* value) {
    if (value->level != EvidenceLevel::kL0) {
      return Status::Ok();
    }

    std::set<std::string> retained_entities;
    retained_entities.insert(claim_.subject);
    retained_entities.insert(claim_source_local_);
    // The obligation L0 keeps states a goal over the sink the claim is about.
    const std::string* sink = LocalFor(request_.input.claim_seed.sink_ref);
    if (sink != nullptr) {
      retained_entities.insert(*sink);
    }
    if (!paths_.empty()) {
      for (const std::string& entity : paths_.front().entity_ids) {
        retained_entities.insert(entity);
      }
    }

    // A fact is primary when it states a bound the claim's own predicate reads:
    // the source's range and the subject's capacity. Everything else is the
    // surrounding slice, which L0 withholds.
    std::set<std::string> retained_facts;
    for (const Fact& fact : facts_) {
      if (Mentions(fact.predicate, claim_.subject) ||
          Mentions(fact.predicate, claim_source_local_)) {
        retained_facts.insert(fact.id);
      }
    }

    std::set<std::string> retained_edge_ids;
    if (!paths_.empty()) {
      const std::vector<std::string>& chain = paths_.front().entity_ids;
      for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
        for (const Edge& edge : edges_) {
          if (edge.from == chain[i] && edge.to == chain[i + 1]) {
            retained_edge_ids.insert(edge.id);
          }
        }
      }
    }

    // A retained unknown and a retained fact still name entities, so those
    // entities stay: dropping one would leave a reference the projection cannot
    // resolve.
    if (!unknowns_.empty()) {
      CollectReferences(unknowns_.front().property, &retained_entities);
    }
    for (const Fact& fact : facts_) {
      if (retained_facts.count(fact.id) != 0) {
        CollectReferences(fact.predicate, &retained_entities);
      }
    }

    // Every member L0 drops is named by an omission. The omission's `subject`
    // must name a member the projected case still declares, so the withheld
    // member is named in the reason and the surviving claim carries the
    // omission: a subject naming a member the projection removed would be a
    // dangling reference.
    std::vector<std::string> withheld;
    for (const Entity& entity : entities_) {
      if (retained_entities.count(entity.id) == 0) withheld.push_back(entity.id);
    }
    for (const Edge& edge : edges_) {
      if (retained_edge_ids.count(edge.id) == 0) withheld.push_back(edge.id);
    }
    if (paths_.size() > 1) {
      for (std::size_t i = 1; i < paths_.size(); ++i) {
        withheld.push_back(paths_[i].id);
      }
    }
    for (const Fact& fact : facts_) {
      if (retained_facts.count(fact.id) == 0) withheld.push_back(fact.id);
    }
    if (unknowns_.size() > 1) {
      for (std::size_t i = 1; i < unknowns_.size(); ++i) {
        withheld.push_back(unknowns_[i].id);
      }
    }
    for (const Constraint& constraint : constraints_) {
      withheld.push_back(constraint.id);
    }

    value->entities.erase(
        std::remove_if(value->entities.begin(), value->entities.end(),
                       [&retained_entities](const Entity& entity) {
                         return retained_entities.count(entity.id) == 0;
                       }),
        value->entities.end());
    value->edges.erase(
        std::remove_if(value->edges.begin(), value->edges.end(),
                       [&retained_edge_ids](const Edge& edge) {
                         return retained_edge_ids.count(edge.id) == 0;
                       }),
        value->edges.end());
    if (!value->paths.empty()) {
      value->paths.resize(1);
    }
    value->facts.erase(
        std::remove_if(value->facts.begin(), value->facts.end(),
                       [&retained_facts](const Fact& fact) {
                         return retained_facts.count(fact.id) == 0;
                       }),
        value->facts.end());
    if (!value->unknowns.empty()) {
      value->unknowns.resize(1);
    }
    value->constraints.clear();

    // A provenance record is retained when a surviving member still names it.
    std::set<std::string> referenced_provenance;
    for (const Fact& fact : value->facts) {
      referenced_provenance.insert(fact.provenance_id);
    }
    for (const Path& path : value->paths) {
      referenced_provenance.insert(path.provenance_id);
    }
    for (const Edge& edge : value->edges) {
      referenced_provenance.insert(edge.provenance_id);
    }
    for (const Provenance& record : value->provenance) {
      if (referenced_provenance.count(record.id) == 0) {
        withheld.push_back(record.id);
      }
    }
    value->provenance.erase(
        std::remove_if(value->provenance.begin(), value->provenance.end(),
                       [&referenced_provenance](const Provenance& record) {
                         return referenced_provenance.count(record.id) == 0;
                       }),
        value->provenance.end());

    // An omission whose subject the projection removed would dangle, so it goes
    // too — and it is a removed member like any other, so the loop below names
    // it. A dropped marker is never allowed to disappear silently: that is the
    // whole of BLD-008.
    std::set<std::string> surviving;
    for (const Entity& entity : value->entities) surviving.insert(entity.id);
    for (const Edge& edge : value->edges) surviving.insert(edge.id);
    for (const Path& path : value->paths) surviving.insert(path.id);
    for (const Fact& fact : value->facts) surviving.insert(fact.id);
    for (const Unknown& unknown : value->unknowns) surviving.insert(unknown.id);
    for (const Constraint& constraint : value->constraints) {
      surviving.insert(constraint.id);
    }
    for (const Provenance& record : value->provenance) {
      surviving.insert(record.id);
    }
    for (const Omission& omission : value->omissions) {
      if (surviving.count(omission.subject) == 0) {
        withheld.push_back(omission.id);
      }
    }
    value->omissions.erase(
        std::remove_if(value->omissions.begin(), value->omissions.end(),
                       [&surviving](const Omission& omission) {
                         return surviving.count(omission.subject) == 0;
                       }),
        value->omissions.end());

    std::sort(withheld.begin(), withheld.end());
    for (const std::string& member : withheld) {
      Omission omission;
      omission.id =
          omission_ids_.Allocate("OM_level_projection_" + Sanitize(member));
      omission.kind = "level_projection";
      omission.subject = claim_.id;
      omission.reason = "the member " + member +
                        " is withheld at l0; a higher level can recover it";
      omission.expandable = true;
      value->omissions.push_back(std::move(omission));
    }
    return Status::Ok();
  }

  // An unknown blocks the claim and every derived fact the *projected* case
  // still reports, so the blocking set never names a member the projection
  // removed.
  Status RetargetBlocking(EvidenceCase* value) {
    std::vector<std::string> blocking = {value->primary_claim.id};
    for (const Fact& fact : value->facts) {
      if (fact.derived) {
        blocking.push_back(fact.id);
      }
    }
    std::sort(blocking.begin(), blocking.end());
    blocking.erase(std::unique(blocking.begin(), blocking.end()),
                   blocking.end());
    for (Unknown& unknown : value->unknowns) {
      unknown.blocking_ids = blocking;
    }
    return Status::Ok();
  }

  void SortCase(EvidenceCase* value) const {
    auto by_id = [](const auto& a, const auto& b) { return a.id < b.id; };
    std::sort(value->entities.begin(), value->entities.end(), by_id);
    std::sort(value->edges.begin(), value->edges.end(), by_id);
    std::sort(value->paths.begin(), value->paths.end(), by_id);
    std::sort(value->facts.begin(), value->facts.end(), by_id);
    std::sort(value->unknowns.begin(), value->unknowns.end(), by_id);
    std::sort(value->constraints.begin(), value->constraints.end(), by_id);
    std::sort(value->provenance.begin(), value->provenance.end(), by_id);
    std::sort(value->proof_obligations.begin(), value->proof_obligations.end(),
              by_id);
    std::sort(value->summaries.begin(), value->summaries.end(), by_id);
    std::sort(value->dependencies.begin(), value->dependencies.end(), by_id);
    std::sort(value->omissions.begin(), value->omissions.end(), by_id);
  }

  struct EntityPlan {
    EntityKind kind = EntityKind::kUnspecified;
    std::string label;
  };
  struct AliasLink {
    std::string left;
    std::string right;
    EpistemicState epistemic = EpistemicState::kUnspecified;
  };

  const EvidenceBuildRequest& request_;
  core::StableId run_id_;

  std::map<std::string, EntityPlan> plans_;
  std::map<std::string, std::string> local_by_stable_;
  LocalIdTable resolver_;

  std::vector<Provenance> provenance_;
  std::map<std::string, std::string> provenance_by_fact_;
  std::map<std::string, const facts::RunFactBinding*> binding_by_fact_;
  std::map<std::string, WitnessView> witness_by_fact_;

  std::vector<Entity> entities_;
  std::vector<Edge> edges_;
  std::vector<Path> paths_;
  std::vector<Fact> facts_;
  std::vector<Unknown> unknowns_;
  std::vector<Constraint> constraints_;
  std::vector<ProofObligation> obligations_;
  std::vector<Dependency> dependencies_;
  std::vector<Omission> omissions_;
  std::vector<const facts::AnalysisFact*> coverage_facts_;
  std::vector<AliasLink> alias_links_;

  IdAllocator fact_ids_;
  IdAllocator unknown_ids_;
  IdAllocator omission_ids_;
  IdAllocator edge_ids_;

  Claim claim_;
  std::string claim_source_local_;
  std::string path_first_;
  std::string observation_sink_local_;
};

}  // namespace

std::string TranslateProducer(std::string_view m9_producer_id) {
  if (m9_producer_id == "evidence-query") {
    return std::string(kEvidenceQueryProducerId);
  }
  return std::string(m9_producer_id);
}

StatusOr<EvidenceCase> EvidenceCaseBuilder::Build(
    const EvidenceBuildRequest& request) const {
  Builder builder(request);
  return builder.Run();
}

}  // namespace veritas::evidence
