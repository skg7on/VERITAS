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

#include "veritas/evidence/EvidencePredicateMapper.h"

#include <cstdint>
#include <string>
#include <utility>

namespace veritas::evidence {
namespace {

namespace sem = analysis::semantic;

// Reads a cell as its declared domain, or reports the mismatch. The relation
// registry already declared each column's domain, so a variant holding another
// alternative is a caller defect rather than a mapping decision.
const core::StableId* AsStable(const facts::SemanticCellValue& cell) {
  return std::get_if<core::StableId>(&cell);
}

const std::int64_t* AsInt64(const facts::SemanticCellValue& cell) {
  return std::get_if<std::int64_t>(&cell);
}

const std::uint64_t* AsUint64(const facts::SemanticCellValue& cell) {
  return std::get_if<std::uint64_t>(&cell);
}

const sem::EpistemicState* AsEpistemic(const facts::SemanticCellValue& cell) {
  return std::get_if<sem::EpistemicState>(&cell);
}

std::string CellMismatch(facts::RelationId relation, std::size_t index,
                         std::string_view expected) {
  return "fact row for relation '" +
         facts::RelationsV2().Get(relation).name + "' cell " +
         std::to_string(index) + " is not a " + std::string(expected);
}

// The EIR reference to a case-local handle. `text` carries the handle with no
// `@`: the sigil belongs to EIR-T's spelling, not to the model.
Expression Reference(const std::string& local_id) {
  Expression expression;
  expression.kind = Expression::Kind::kReference;
  expression.text = local_id;
  return expression;
}

Expression Integer(std::int64_t value) {
  Expression expression;
  expression.kind = Expression::Kind::kInteger;
  expression.integer = value;
  return expression;
}

Expression Call(std::string callee, std::vector<Expression> arguments) {
  Expression expression;
  expression.kind = Expression::Kind::kCall;
  expression.text = std::move(callee);
  expression.operands = std::move(arguments);
  return expression;
}

// Resolves one stable cell to the case-local handle that declares it.
StatusOr<std::string> ResolveCell(const facts::AnalysisFact& fact,
                                  std::size_t index,
                                  const StableIdResolver& resolver) {
  const core::StableId* stable = AsStable(fact.row.cells[index]);
  if (stable == nullptr) {
    return Status::InvalidArgument(
        CellMismatch(fact.row.relation, index, "stable ID"));
  }
  std::optional<std::string> local = resolver.Resolve(*stable);
  if (!local.has_value()) {
    return Status::InvalidArgument(
        "the case declares no member for the stable ID '" +
        core::ToString(*stable) + "' referenced by a fact row for relation '" +
        facts::RelationsV2().Get(fact.row.relation).name + "'");
  }
  return std::move(*local);
}

}  // namespace

void LocalIdTable::Bind(const core::StableId& stable_id, std::string local_id) {
  by_text_[core::ToString(stable_id)] = std::move(local_id);
}

std::optional<std::string> LocalIdTable::Resolve(
    const core::StableId& stable_id) const {
  auto it = by_text_.find(core::ToString(stable_id));
  if (it == by_text_.end()) {
    return std::nullopt;
  }
  return it->second;
}

EpistemicState CopyEpistemicState(sem::EpistemicState state) {
  switch (state) {
    case sem::EpistemicState::kMust:
      return EpistemicState::kMust;
    case sem::EpistemicState::kMay:
      return EpistemicState::kMay;
    case sem::EpistemicState::kMustNot:
      return EpistemicState::kMustNot;
    case sem::EpistemicState::kInferred:
      return EpistemicState::kInferred;
    case sem::EpistemicState::kAssumed:
      return EpistemicState::kAssumed;
    case sem::EpistemicState::kUnknown:
      return EpistemicState::kUnknown;
  }
  // Unreachable: the M9 enum is closed and every value is handled above. The
  // fallthrough is the weakest state rather than an abort, so a future enum
  // addition degrades to "unknown" instead of terminating a build.
  return EpistemicState::kUnknown;
}

Confidence ConfidenceForState(EpistemicState state) {
  switch (state) {
    case EpistemicState::kMust:
    case EpistemicState::kMustNot:
      return Confidence::kExact;
    case EpistemicState::kMay:
      return Confidence::kMedium;
    case EpistemicState::kInferred:
    case EpistemicState::kAssumed:
      return Confidence::kLow;
    case EpistemicState::kUnknown:
    case EpistemicState::kUnspecified:
      return Confidence::kUnknown;
  }
  return Confidence::kUnknown;
}

std::optional<std::string_view> EvidencePredicateMapper::PredicateName(
    facts::RelationId relation) {
  switch (relation) {
    case facts::RelationId::kDirectRead:
      return "range";
    case facts::RelationId::kDirectWrite:
      return "capacity";
    case facts::RelationId::kReachableCall:
      return "reachable";
    case facts::RelationId::kAlias:
      return "alias";
    case facts::RelationId::kMayRead:
      return "reads";
    case facts::RelationId::kMayWrite:
      return "writes";
    case facts::RelationId::kGlobalFlow:
      return "reachable";
    default:
      return std::nullopt;
  }
}

std::optional<std::string_view> EvidencePredicateMapper::ProducerName(
    facts::RelationId relation) {
  switch (relation) {
    case facts::RelationId::kDirectRead:
      return "analysis.value_range";
    case facts::RelationId::kDirectWrite:
      return "analysis.memory_object";
    case facts::RelationId::kReachableCall:
      return "analysis.call_graph";
    case facts::RelationId::kAlias:
      return "analysis.alias";
    case facts::RelationId::kMayRead:
    case facts::RelationId::kMayWrite:
      return "analysis.memory_effect";
    case facts::RelationId::kGlobalFlow:
      return "analysis.value_flow";
    default:
      return std::nullopt;
  }
}

StatusOr<Fact> EvidencePredicateMapper::MapFact(
    const facts::AnalysisFact& fact, const StableIdResolver& resolver,
    const FactMappingHandle& handle) const {
  // The registry is the authority on the row's shape: cell count, per-cell
  // domain, and allowed epistemic states. Nothing below re-derives any of it.
  Status shape = facts::ValidateSemanticRow(fact.row);
  if (!shape.ok()) {
    return shape;
  }

  switch (fact.row.relation) {
    case facts::RelationId::kDirectRead:
      return MapRange(fact, resolver, handle);
    case facts::RelationId::kDirectWrite:
      return MapCapacity(fact, resolver, handle);
    case facts::RelationId::kReachableCall:
    case facts::RelationId::kGlobalFlow:
      return MapReachability(fact, resolver, handle);
    case facts::RelationId::kAlias:
      return MapAlias(fact, resolver, handle);
    case facts::RelationId::kMayRead:
    case facts::RelationId::kMayWrite:
      return MapMemoryEffect(fact, resolver, handle);
    default:
      // Refused, never dropped: an unsupported relation is a typed failure the
      // caller must resolve, because a silently omitted fact would leave the
      // case claiming a completeness it does not have.
      return Status::InvalidArgument(
          "the relation '" + facts::RelationsV2().Get(fact.row.relation).name +
          "' has no built-in EIR predicate mapping");
  }
}

StatusOr<Fact> EvidencePredicateMapper::MapRange(
    const facts::AnalysisFact& fact, const StableIdResolver& resolver,
    const FactMappingHandle& handle) const {
  const auto& cells = fact.row.cells;
  const std::int64_t* offset = AsInt64(cells[3]);
  const std::uint64_t* size = AsUint64(cells[4]);
  if (offset == nullptr) {
    return Status::InvalidArgument(
        CellMismatch(fact.row.relation, 3, "signed offset"));
  }
  if (size == nullptr) {
    return Status::InvalidArgument(CellMismatch(fact.row.relation, 4, "size"));
  }
  auto value = ResolveCell(fact, 1, resolver);
  if (!value.ok()) {
    return value.status();
  }
  // `range(@value, min, max)`: the read's byte window is the closed interval
  // the analysis established, stated over the memory the read names.
  return Assemble(fact, handle,
                  Call("range", {Reference(value.value()), Integer(*offset),
                                 Integer(static_cast<std::int64_t>(*size))}));
}

StatusOr<Fact> EvidencePredicateMapper::MapCapacity(
    const facts::AnalysisFact& fact, const StableIdResolver& resolver,
    const FactMappingHandle& handle) const {
  const auto& cells = fact.row.cells;
  const std::uint64_t* size = AsUint64(cells[4]);
  if (size == nullptr) {
    return Status::InvalidArgument(CellMismatch(fact.row.relation, 4, "size"));
  }
  auto memory = ResolveCell(fact, 1, resolver);
  if (!memory.ok()) {
    return memory.status();
  }
  // `capacity(@memory, bytes)`: the size of the object the write targets.
  return Assemble(fact, handle,
                  Call("capacity", {Reference(memory.value()),
                                    Integer(static_cast<std::int64_t>(*size))}));
}

StatusOr<Fact> EvidencePredicateMapper::MapReachability(
    const facts::AnalysisFact& fact, const StableIdResolver& resolver,
    const FactMappingHandle& handle) const {
  auto from = ResolveCell(fact, 0, resolver);
  if (!from.ok()) {
    return from.status();
  }
  auto to = ResolveCell(fact, 1, resolver);
  if (!to.ok()) {
    return to.status();
  }
  // `reachable(@from, @to)`: both endpoints of the edge are resolved, which is
  // why the resolver is a table and not one local-ID string.
  return Assemble(fact, handle,
                  Call("reachable", {Reference(from.value()),
                                     Reference(to.value())}));
}

StatusOr<Fact> EvidencePredicateMapper::MapAlias(
    const facts::AnalysisFact& fact, const StableIdResolver& resolver,
    const FactMappingHandle& handle) const {
  auto left = ResolveCell(fact, 0, resolver);
  if (!left.ok()) {
    return left.status();
  }
  auto right = ResolveCell(fact, 1, resolver);
  if (!right.ok()) {
    return right.status();
  }
  // `alias(@left, @right)`: both references are resolved to their own handles.
  return Assemble(fact, handle, Call("alias", {Reference(left.value()),
                                               Reference(right.value())}));
}

StatusOr<Fact> EvidencePredicateMapper::MapMemoryEffect(
    const facts::AnalysisFact& fact, const StableIdResolver& resolver,
    const FactMappingHandle& handle) const {
  auto function = ResolveCell(fact, 0, resolver);
  if (!function.ok()) {
    return function.status();
  }
  auto memory = ResolveCell(fact, 1, resolver);
  if (!memory.ok()) {
    return memory.status();
  }
  // The direction is the registry's, not a heuristic: `MayRead` reads and
  // `MayWrite` writes. Both references are resolved.
  const std::string_view predicate =
      *PredicateName(fact.row.relation);
  return Assemble(fact, handle,
                  Call(std::string(predicate),
                       {Reference(function.value()), Reference(memory.value())}));
}

StatusOr<Fact> EvidencePredicateMapper::Assemble(
    const facts::AnalysisFact& fact, const FactMappingHandle& handle,
    Expression predicate) const {
  const auto& cells = fact.row.cells;
  const sem::EpistemicState* state = AsEpistemic(cells.back());
  if (state == nullptr) {
    return Status::InvalidArgument(
        CellMismatch(fact.row.relation, cells.size() - 1, "epistemic state"));
  }

  const std::optional<std::string_view> producer =
      ProducerName(fact.row.relation);
  if (!producer.has_value()) {
    return Status::InvalidArgument(
        "the relation '" + facts::RelationsV2().Get(fact.row.relation).name +
        "' has no built-in producer identity");
  }

  Fact mapped;
  mapped.id = handle.local_id;
  // The M9 fact ID is content-addressed over the semantic row alone, so it
  // survives lowering as the case's cross-run identity for this fact.
  mapped.stable_id = fact.fact_id;
  mapped.predicate = std::move(predicate);
  // Copied, never promoted: the mapper's authority stops at transcription.
  mapped.epistemic = CopyEpistemicState(*state);
  mapped.confidence = ConfidenceForState(mapped.epistemic);
  mapped.producer = std::string(*producer);
  mapped.provenance_id = handle.provenance_id;
  // A fact the analysis stated as MUST is an observation of the analysis, not a
  // derivation this case performed; anything weaker is derived and therefore
  // owes the provenance the validator will look for.
  mapped.derived = *state != sem::EpistemicState::kMust;
  return mapped;
}

}  // namespace veritas::evidence
