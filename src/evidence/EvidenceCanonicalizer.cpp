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

// EvidenceCanonicalizer.cpp — the canonical byte encoding of an `eir.v1`
// case.
//
// The implementation is one deterministic value tree built with
// `core::CanonicalValue`, whose root object covers every model field (the
// object is a `std::map`, so the emplace order below is cosmetic), plus one
// sort discipline for every collection the header declares unordered. Every
// enum
// enters the bytes through its stable textual spelling and every engaged stable
// ID through `core::ToString`, so identity never depends on an enumerator's
// numeric value or on a struct's memory layout.

#include "veritas/evidence/EvidenceCanonicalizer.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "veritas/core/CanonicalValue.h"
#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/evidence/EvidenceCase.h"
#include "veritas/evidence/EvidenceValidator.h"

namespace veritas::evidence {
namespace {

// The stable spelling of an expression kind: the operator and literal names
// the grammar uses, which the validator's diagnostics already carry, so
// identity and diagnostics name a kind the same way.
std::string_view ExpressionKindName(Expression::Kind kind) {
  switch (kind) {
    case Expression::Kind::kUnspecified:
      return "unspecified";
    case Expression::Kind::kBool:
      return "bool";
    case Expression::Kind::kInteger:
      return "integer";
    case Expression::Kind::kString:
      return "string";
    case Expression::Kind::kSymbol:
      return "symbol";
    case Expression::Kind::kReference:
      return "reference";
    case Expression::Kind::kCall:
      return "call";
    case Expression::Kind::kNot:
      return "not";
    case Expression::Kind::kCompare:
      return "compare";
    case Expression::Kind::kAnd:
      return "and";
    case Expression::Kind::kOr:
      return "or";
    case Expression::Kind::kImplies:
      return "implies";
    case Expression::Kind::kForAll:
      return "forall";
    case Expression::Kind::kExists:
      return "exists";
  }
  return "expression";
}

// The sort key of one element of a canonically ordered collection. `kind` is
// the record's declared kind family **spelled as the payload spells it**, not
// its enumerator value: an enumerator inserted into the middle of a family
// renumbers its successors, and an order keyed on that number would change the
// bytes — and so the identity — of a case whose meaning never changed. The
// component is empty for a record with no kind family. The stable-ID component
// is empty when the record carries no engaged identity.
struct SortKey {
  std::string kind;
  std::string stable_id;
  std::string local_id;
};

// One element together with the encoding that orders it. The bytes are
// computed once and serve both as the total tie-break and as the element's
// content, so the order and the payload can never disagree.
struct KeyedElement {
  SortKey key;
  std::vector<std::byte> bytes;
  core::CanonicalValue value;
};

bool KeyedLess(const KeyedElement& left, const KeyedElement& right) {
  if (left.key.kind != right.key.kind) {
    return left.key.kind < right.key.kind;
  }
  if (left.key.stable_id != right.key.stable_id) {
    return left.key.stable_id < right.key.stable_id;
  }
  if (left.key.local_id != right.key.local_id) {
    return left.key.local_id < right.key.local_id;
  }
  return std::lexicographical_compare(left.bytes.begin(), left.bytes.end(),
                                      right.bytes.begin(), right.bytes.end());
}

StatusOr<KeyedElement> MakeKeyedElement(SortKey key,
                                        core::CanonicalValue value) {
  auto bytes = core::CanonicalEncode(value);
  if (!bytes.ok()) {
    return bytes.status();
  }
  return KeyedElement{std::move(key), std::move(*bytes), std::move(value)};
}

// A collection sorted by the canonical order: kind spelling, stable identity,
// local handle, then the element's own encoding.
core::CanonicalValue SortedArray(std::vector<KeyedElement> elements) {
  std::sort(elements.begin(), elements.end(), KeyedLess);
  core::CanonicalArray array;
  array.reserve(elements.size());
  for (KeyedElement& element : elements) {
    array.push_back(std::move(element.value));
  }
  return core::Array(std::move(array));
}

// A collection whose sequence is semantic: emitted exactly as declared.
core::CanonicalValue OrderedArray(std::vector<core::CanonicalValue> values) {
  return core::Array(std::move(values));
}

// Orders values that carry no identity of their own by their canonical
// encodings alone. Deterministic and total for distinct values; equal
// encodings are interchangeable by construction.
//
// A value that cannot be encoded is a hard failure, never a value appended
// after the ordered ones: an element that skipped the sort would put the
// collection back in input order and reintroduce the construction-order
// dependence `VID-007` forbids. Today `core::CanonicalEncode` has no failure
// path at all — it encodes every kind, a `TaggedPath` included — so this
// status never fires; it exists so that it cannot start failing silently.
StatusOr<std::vector<core::CanonicalValue>> SortValuesByEncoding(
    std::vector<core::CanonicalValue> values) {
  std::vector<KeyedElement> elements;
  elements.reserve(values.size());
  for (core::CanonicalValue& value : values) {
    auto element = MakeKeyedElement(SortKey{}, std::move(value));
    if (!element.ok()) {
      return element.status();
    }
    elements.push_back(std::move(*element));
  }
  std::sort(elements.begin(), elements.end(), KeyedLess);

  std::vector<core::CanonicalValue> sorted;
  sorted.reserve(elements.size());
  for (KeyedElement& element : elements) {
    sorted.push_back(std::move(element.value));
  }
  return sorted;
}

// --- Scalars, references, and lists -----------------------------------------

// The stable-ID component of a sort key: the canonical spelling when engaged,
// the empty string when not, so an absent identity sorts before every present
// one and never collides with a local handle.
std::string StableIdComponent(const std::optional<core::StableId>& stable_id) {
  if (!stable_id.has_value()) {
    return std::string();
  }
  return core::ToString(*stable_id);
}

core::CanonicalValue EncodeOptionalStableId(
    const std::optional<core::StableId>& stable_id) {
  if (!stable_id.has_value()) {
    return core::Null();
  }
  return core::String(core::ToString(*stable_id));
}

core::CanonicalValue EncodeRequiredStableId(const core::StableId& stable_id) {
  return core::String(core::ToString(stable_id));
}

// A reference list: an unordered set of names, sorted by value with duplicates
// preserved, so multiplicity stays part of the case.
core::CanonicalValue EncodeSortedStrings(
    const std::vector<std::string>& values) {
  std::vector<std::string> sorted = values;
  std::sort(sorted.begin(), sorted.end());
  std::vector<core::CanonicalValue> array;
  array.reserve(sorted.size());
  for (std::string& value : sorted) {
    array.push_back(core::String(std::move(value)));
  }
  return OrderedArray(std::move(array));
}

// --- Expressions ------------------------------------------------------------

StatusOr<core::CanonicalValue> EncodeExpression(const Expression& expression) {
  std::vector<core::CanonicalValue> operands;
  operands.reserve(expression.operands.size());
  for (const Expression& operand : expression.operands) {
    auto encoded = EncodeExpression(operand);
    if (!encoded.ok()) {
      return encoded.status();
    }
    operands.push_back(std::move(*encoded));
  }

  // `and` and `or` are commutative: their operands are ordered by their own
  // canonical encodings. Every other kind keeps its operand sequence — a
  // comparison stays left-to-right, an implication keeps its antecedent and
  // consequent, and call arguments keep their positions.
  if (expression.kind == Expression::Kind::kAnd ||
      expression.kind == Expression::Kind::kOr) {
    auto sorted = SortValuesByEncoding(std::move(operands));
    if (!sorted.ok()) {
      return sorted.status();
    }
    operands = std::move(*sorted);
  }

  core::CanonicalObject object;
  object.emplace("boolean", core::Bool(expression.boolean));
  object.emplace("integer", core::Int(expression.integer));
  object.emplace(
      "kind", core::String(std::string(ExpressionKindName(expression.kind))));
  object.emplace("operands", OrderedArray(std::move(operands)));
  object.emplace("text", core::String(expression.text));
  return core::Object(std::move(object));
}

// --- Records ----------------------------------------------------------------

core::CanonicalValue EncodeAnalyzerVersion(const AnalyzerVersion& version) {
  core::CanonicalObject object;
  object.emplace("configuration", core::String(version.configuration));
  object.emplace("producer", core::String(version.producer));
  object.emplace("version", core::String(version.version));
  return core::Object(std::move(object));
}

StatusOr<core::CanonicalValue> EncodeProgramBinding(
    const ProgramBinding& program) {
  std::vector<KeyedElement> versions;
  versions.reserve(program.analyzer_versions.size());
  for (const AnalyzerVersion& version : program.analyzer_versions) {
    auto element = MakeKeyedElement(SortKey{}, EncodeAnalyzerVersion(version));
    if (!element.ok()) {
      return element.status();
    }
    versions.push_back(std::move(*element));
  }

  core::CanonicalObject object;
  object.emplace("analysis_configuration_id",
                 core::String(program.analysis_configuration_id));
  object.emplace("analysis_run_id",
                 EncodeOptionalStableId(program.analysis_run_id));
  object.emplace("analyzer_versions", SortedArray(std::move(versions)));
  object.emplace("build_variant_id", core::String(program.build_variant_id));
  object.emplace("repository_id", core::String(program.repository_id));
  object.emplace("revision_id", core::String(program.revision_id));
  object.emplace("target_triple", core::String(program.target_triple));
  object.emplace("type_layout_id", core::String(program.type_layout_id));
  return core::Object(std::move(object));
}

StatusOr<core::CanonicalValue> EncodeEntity(const Entity& entity) {
  // The property bag is key-sorted structurally: `CanonicalObject` is a
  // `std::map`, so its order never depends on how the bag was filled.
  core::CanonicalObject properties;
  for (const auto& property : entity.properties) {
    auto value = EncodeExpression(property.second);
    if (!value.ok()) {
      return value.status();
    }
    properties.emplace(property.first, std::move(*value));
  }

  core::CanonicalObject object;
  object.emplace("id", core::String(entity.id));
  object.emplace("kind", core::String(std::string(ToString(entity.kind))));
  object.emplace("properties", core::Object(std::move(properties)));
  object.emplace("stable_id", EncodeOptionalStableId(entity.stable_id));
  return core::Object(std::move(object));
}

core::CanonicalValue EncodeEdge(const Edge& edge) {
  core::CanonicalObject object;
  object.emplace("epistemic",
                 core::String(std::string(ToString(edge.epistemic))));
  object.emplace("expandable", core::Bool(edge.expandable));
  object.emplace("from", core::String(edge.from));
  object.emplace("id", core::String(edge.id));
  object.emplace("kind", core::String(std::string(ToString(edge.kind))));
  object.emplace("provenance_id", core::String(edge.provenance_id));
  object.emplace("summarized_by", core::String(edge.summarized_by));
  object.emplace("to", core::String(edge.to));
  return core::Object(std::move(object));
}

StatusOr<core::CanonicalValue> EncodePath(const Path& path) {
  // A path's conditions constrain it conjunctively, so the set is ordered like
  // an `and` over the same predicates. Its segments are not: the sequence is
  // the path.
  std::vector<core::CanonicalValue> conditions;
  conditions.reserve(path.conditions.size());
  for (const Expression& condition : path.conditions) {
    auto encoded = EncodeExpression(condition);
    if (!encoded.ok()) {
      return encoded.status();
    }
    conditions.push_back(std::move(*encoded));
  }
  auto sorted_conditions = SortValuesByEncoding(std::move(conditions));
  if (!sorted_conditions.ok()) {
    return sorted_conditions.status();
  }

  std::vector<core::CanonicalValue> segments;
  segments.reserve(path.entity_ids.size());
  for (const std::string& entity_id : path.entity_ids) {
    segments.push_back(core::String(entity_id));
  }

  core::CanonicalObject object;
  object.emplace("conditions", OrderedArray(std::move(*sorted_conditions)));
  object.emplace("entity_ids", OrderedArray(std::move(segments)));
  object.emplace("feasibility",
                 core::String(std::string(ToString(path.feasibility))));
  object.emplace("id", core::String(path.id));
  object.emplace("kind", core::String(std::string(ToString(path.kind))));
  object.emplace("provenance_id", core::String(path.provenance_id));
  return core::Object(std::move(object));
}

StatusOr<core::CanonicalValue> EncodeClaim(const Claim& claim) {
  auto predicate = EncodeExpression(claim.predicate);
  if (!predicate.ok()) {
    return predicate.status();
  }

  core::CanonicalObject object;
  object.emplace("description", core::String(claim.description));
  object.emplace("id", core::String(claim.id));
  object.emplace("kind", core::String(std::string(ToString(claim.kind))));
  object.emplace("predicate", std::move(*predicate));
  object.emplace("severity",
                 core::String(std::string(ToString(claim.severity))));
  object.emplace("subject", core::String(claim.subject));
  return core::Object(std::move(object));
}

StatusOr<core::CanonicalValue> EncodeFact(const Fact& fact) {
  auto predicate = EncodeExpression(fact.predicate);
  if (!predicate.ok()) {
    return predicate.status();
  }

  core::CanonicalObject object;
  object.emplace("confidence",
                 core::String(std::string(ToString(fact.confidence))));
  object.emplace("derived", core::Bool(fact.derived));
  object.emplace("epistemic",
                 core::String(std::string(ToString(fact.epistemic))));
  object.emplace("id", core::String(fact.id));
  object.emplace("predicate", std::move(*predicate));
  object.emplace("producer", core::String(fact.producer));
  object.emplace("provenance_id", core::String(fact.provenance_id));
  object.emplace("stable_id", EncodeOptionalStableId(fact.stable_id));
  return core::Object(std::move(object));
}

StatusOr<core::CanonicalValue> EncodeAssumption(const Assumption& assumption) {
  auto predicate = EncodeExpression(assumption.predicate);
  if (!predicate.ok()) {
    return predicate.status();
  }

  core::CanonicalObject object;
  object.emplace("id", core::String(assumption.id));
  object.emplace("predicate", std::move(*predicate));
  object.emplace("scope", core::String(assumption.scope));
  object.emplace("source", core::String(assumption.source));
  return core::Object(std::move(object));
}

StatusOr<core::CanonicalValue> EncodeHypothesis(const Hypothesis& hypothesis) {
  auto predicate = EncodeExpression(hypothesis.predicate);
  if (!predicate.ok()) {
    return predicate.status();
  }

  core::CanonicalObject object;
  object.emplace("confidence",
                 core::String(std::string(ToString(hypothesis.confidence))));
  object.emplace("id", core::String(hypothesis.id));
  object.emplace("predicate", std::move(*predicate));
  object.emplace("producer", core::String(hypothesis.producer));
  object.emplace("reason", core::String(hypothesis.reason));
  return core::Object(std::move(object));
}

StatusOr<core::CanonicalValue> EncodeUnknown(const Unknown& unknown) {
  auto property = EncodeExpression(unknown.property);
  if (!property.ok()) {
    return property.status();
  }

  core::CanonicalObject object;
  object.emplace("blocking_ids", EncodeSortedStrings(unknown.blocking_ids));
  object.emplace("id", core::String(unknown.id));
  object.emplace("property", std::move(*property));
  object.emplace("reason", core::String(unknown.reason));
  object.emplace("reason_code",
                 core::String(std::string(ToString(unknown.reason_code))));
  object.emplace("suggested_resolution",
                 core::String(unknown.suggested_resolution));
  return core::Object(std::move(object));
}

StatusOr<core::CanonicalValue> EncodeConstraint(const Constraint& constraint) {
  auto expression = EncodeExpression(constraint.expression);
  if (!expression.ok()) {
    return expression.status();
  }

  core::CanonicalObject object;
  object.emplace("epistemic",
                 core::String(std::string(ToString(constraint.epistemic))));
  object.emplace("expression", std::move(*expression));
  object.emplace("id", core::String(constraint.id));
  object.emplace("provenance_id", core::String(constraint.provenance_id));
  object.emplace("scope", core::String(constraint.scope));
  return core::Object(std::move(object));
}

core::CanonicalValue EncodeProvenance(const Provenance& record) {
  core::CanonicalObject object;
  object.emplace("analysis_run_id",
                 EncodeOptionalStableId(record.analysis_run_id));
  object.emplace("configuration", core::String(record.configuration));
  object.emplace("id", core::String(record.id));
  object.emplace("input_fact_ids", EncodeSortedStrings(record.input_fact_ids));
  object.emplace("producer", core::String(record.producer));
  object.emplace("rule", core::String(record.rule));
  object.emplace("source_anchor_id", core::String(record.source_anchor_id));
  object.emplace("version", core::String(record.version));
  return core::Object(std::move(object));
}

StatusOr<core::CanonicalValue> EncodeProofObligation(
    const ProofObligation& obligation) {
  auto budget = EncodeExpression(obligation.budget);
  if (!budget.ok()) {
    return budget.status();
  }
  auto predicate = EncodeExpression(obligation.predicate);
  if (!predicate.ok()) {
    return predicate.status();
  }

  core::CanonicalObject object;
  object.emplace("budget", std::move(*budget));
  object.emplace("goal_kind",
                 core::String(std::string(ToString(obligation.goal_kind))));
  object.emplace("id", core::String(obligation.id));
  object.emplace("predicate", std::move(*predicate));
  object.emplace("result_id", core::String(obligation.result_id));
  object.emplace("status",
                 core::String(std::string(ToString(obligation.status))));
  object.emplace("verification_producer",
                 core::String(obligation.verification_producer));
  object.emplace("verifier_kinds",
                 EncodeSortedStrings(obligation.verifier_kinds));
  return core::Object(std::move(object));
}

core::CanonicalValue EncodeSummaryReference(const SummaryReference& summary) {
  core::CanonicalObject object;
  object.emplace("components", EncodeSortedStrings(summary.components));
  object.emplace("function_id", core::String(summary.function_id));
  object.emplace("id", core::String(summary.id));
  object.emplace("summary_id", EncodeRequiredStableId(summary.summary_id));
  return core::Object(std::move(object));
}

core::CanonicalValue EncodeDependency(const Dependency& dependency) {
  core::CanonicalObject object;
  object.emplace("id", core::String(dependency.id));
  object.emplace("kind", core::String(std::string(ToString(dependency.kind))));
  object.emplace("stable_id", EncodeRequiredStableId(dependency.stable_id));
  return core::Object(std::move(object));
}

core::CanonicalValue EncodeOmission(const Omission& omission) {
  core::CanonicalObject object;
  object.emplace("expandable", core::Bool(omission.expandable));
  object.emplace("id", core::String(omission.id));
  object.emplace("kind", core::String(omission.kind));
  object.emplace("reason", core::String(omission.reason));
  object.emplace("subject", core::String(omission.subject));
  return core::Object(std::move(object));
}

// --- Member collections -----------------------------------------------------

StatusOr<core::CanonicalValue> EncodeEntities(
    const std::vector<Entity>& entities) {
  std::vector<KeyedElement> elements;
  elements.reserve(entities.size());
  for (const Entity& entity : entities) {
    auto encoded = EncodeEntity(entity);
    if (!encoded.ok()) {
      return encoded.status();
    }
    auto element = MakeKeyedElement(
        SortKey{std::string(ToString(entity.kind)),
                StableIdComponent(entity.stable_id), entity.id},
        std::move(*encoded));
    if (!element.ok()) {
      return element.status();
    }
    elements.push_back(std::move(*element));
  }
  return SortedArray(std::move(elements));
}

StatusOr<core::CanonicalValue> EncodeEdges(const std::vector<Edge>& edges) {
  std::vector<KeyedElement> elements;
  elements.reserve(edges.size());
  for (const Edge& edge : edges) {
    auto element = MakeKeyedElement(
        SortKey{std::string(ToString(edge.kind)), std::string(), edge.id},
        EncodeEdge(edge));
    if (!element.ok()) {
      return element.status();
    }
    elements.push_back(std::move(*element));
  }
  return SortedArray(std::move(elements));
}

StatusOr<core::CanonicalValue> EncodePaths(const std::vector<Path>& paths) {
  std::vector<KeyedElement> elements;
  elements.reserve(paths.size());
  for (const Path& path : paths) {
    auto encoded = EncodePath(path);
    if (!encoded.ok()) {
      return encoded.status();
    }
    auto element = MakeKeyedElement(
        SortKey{std::string(ToString(path.kind)), std::string(), path.id},
        std::move(*encoded));
    if (!element.ok()) {
      return element.status();
    }
    elements.push_back(std::move(*element));
  }
  return SortedArray(std::move(elements));
}

StatusOr<core::CanonicalValue> EncodeFacts(const std::vector<Fact>& facts) {
  std::vector<KeyedElement> elements;
  elements.reserve(facts.size());
  for (const Fact& fact : facts) {
    auto encoded = EncodeFact(fact);
    if (!encoded.ok()) {
      return encoded.status();
    }
    auto element = MakeKeyedElement(
        SortKey{std::string(), StableIdComponent(fact.stable_id), fact.id},
        std::move(*encoded));
    if (!element.ok()) {
      return element.status();
    }
    elements.push_back(std::move(*element));
  }
  return SortedArray(std::move(elements));
}

StatusOr<core::CanonicalValue> EncodeAssumptions(
    const std::vector<Assumption>& assumptions) {
  std::vector<KeyedElement> elements;
  elements.reserve(assumptions.size());
  for (const Assumption& assumption : assumptions) {
    auto encoded = EncodeAssumption(assumption);
    if (!encoded.ok()) {
      return encoded.status();
    }
    auto element = MakeKeyedElement(
        SortKey{std::string(), std::string(), assumption.id},
        std::move(*encoded));
    if (!element.ok()) {
      return element.status();
    }
    elements.push_back(std::move(*element));
  }
  return SortedArray(std::move(elements));
}

StatusOr<core::CanonicalValue> EncodeHypotheses(
    const std::vector<Hypothesis>& hypotheses) {
  std::vector<KeyedElement> elements;
  elements.reserve(hypotheses.size());
  for (const Hypothesis& hypothesis : hypotheses) {
    auto encoded = EncodeHypothesis(hypothesis);
    if (!encoded.ok()) {
      return encoded.status();
    }
    auto element = MakeKeyedElement(
        SortKey{std::string(), std::string(), hypothesis.id},
        std::move(*encoded));
    if (!element.ok()) {
      return element.status();
    }
    elements.push_back(std::move(*element));
  }
  return SortedArray(std::move(elements));
}

StatusOr<core::CanonicalValue> EncodeUnknowns(
    const std::vector<Unknown>& unknowns) {
  std::vector<KeyedElement> elements;
  elements.reserve(unknowns.size());
  for (const Unknown& unknown : unknowns) {
    auto encoded = EncodeUnknown(unknown);
    if (!encoded.ok()) {
      return encoded.status();
    }
    auto element =
        MakeKeyedElement(SortKey{std::string(), std::string(), unknown.id},
                         std::move(*encoded));
    if (!element.ok()) {
      return element.status();
    }
    elements.push_back(std::move(*element));
  }
  return SortedArray(std::move(elements));
}

StatusOr<core::CanonicalValue> EncodeConstraints(
    const std::vector<Constraint>& constraints) {
  std::vector<KeyedElement> elements;
  elements.reserve(constraints.size());
  for (const Constraint& constraint : constraints) {
    auto encoded = EncodeConstraint(constraint);
    if (!encoded.ok()) {
      return encoded.status();
    }
    auto element = MakeKeyedElement(
        SortKey{std::string(), std::string(), constraint.id},
        std::move(*encoded));
    if (!element.ok()) {
      return element.status();
    }
    elements.push_back(std::move(*element));
  }
  return SortedArray(std::move(elements));
}

StatusOr<core::CanonicalValue> EncodeProvenanceRecords(
    const std::vector<Provenance>& records) {
  std::vector<KeyedElement> elements;
  elements.reserve(records.size());
  for (const Provenance& record : records) {
    auto element =
        MakeKeyedElement(SortKey{std::string(), std::string(), record.id},
                         EncodeProvenance(record));
    if (!element.ok()) {
      return element.status();
    }
    elements.push_back(std::move(*element));
  }
  return SortedArray(std::move(elements));
}

StatusOr<core::CanonicalValue> EncodeProofObligations(
    const std::vector<ProofObligation>& obligations) {
  std::vector<KeyedElement> elements;
  elements.reserve(obligations.size());
  for (const ProofObligation& obligation : obligations) {
    auto encoded = EncodeProofObligation(obligation);
    if (!encoded.ok()) {
      return encoded.status();
    }
    auto element = MakeKeyedElement(
        SortKey{std::string(ToString(obligation.goal_kind)), std::string(),
                obligation.id},
        std::move(*encoded));
    if (!element.ok()) {
      return element.status();
    }
    elements.push_back(std::move(*element));
  }
  return SortedArray(std::move(elements));
}

StatusOr<core::CanonicalValue> EncodeSummaries(
    const std::vector<SummaryReference>& summaries) {
  std::vector<KeyedElement> elements;
  elements.reserve(summaries.size());
  for (const SummaryReference& summary : summaries) {
    auto element =
        MakeKeyedElement(SortKey{std::string(), std::string(), summary.id},
                         EncodeSummaryReference(summary));
    if (!element.ok()) {
      return element.status();
    }
    elements.push_back(std::move(*element));
  }
  return SortedArray(std::move(elements));
}

StatusOr<core::CanonicalValue> EncodeDependencies(
    const std::vector<Dependency>& dependencies) {
  std::vector<KeyedElement> elements;
  elements.reserve(dependencies.size());
  for (const Dependency& dependency : dependencies) {
    auto element = MakeKeyedElement(
        SortKey{std::string(ToString(dependency.kind)), std::string(),
                dependency.id},
        EncodeDependency(dependency));
    if (!element.ok()) {
      return element.status();
    }
    elements.push_back(std::move(*element));
  }
  return SortedArray(std::move(elements));
}

StatusOr<core::CanonicalValue> EncodeOmissions(
    const std::vector<Omission>& omissions) {
  std::vector<KeyedElement> elements;
  elements.reserve(omissions.size());
  for (const Omission& omission : omissions) {
    auto element = MakeKeyedElement(
        SortKey{std::string(), std::string(), omission.id},
        EncodeOmission(omission));
    if (!element.ok()) {
      return element.status();
    }
    elements.push_back(std::move(*element));
  }
  return SortedArray(std::move(elements));
}

// --- The case ---------------------------------------------------------------

// The value tree `core::CanonicalEncode` hashes. `evidence_id` is never read:
// the identity is this function's output, so it cannot be its own input.
StatusOr<core::CanonicalValue> CanonicalEvidenceValue(
    const EvidenceCase& value) {
  core::CanonicalObject root;
  root.emplace("schema", core::String(value.schema_version));
  root.emplace("level", core::String(std::string(ToString(value.level))));
  root.emplace("state",
               core::String(std::string(ToString(value.verification_state))));

  auto program = EncodeProgramBinding(value.program);
  if (!program.ok()) {
    return program.status();
  }
  root.emplace("program", std::move(*program));
  auto claim = EncodeClaim(value.primary_claim);
  if (!claim.ok()) {
    return claim.status();
  }
  root.emplace("claim", std::move(*claim));

  auto entities = EncodeEntities(value.entities);
  if (!entities.ok()) {
    return entities.status();
  }
  root.emplace("entities", std::move(*entities));

  auto edges = EncodeEdges(value.edges);
  if (!edges.ok()) {
    return edges.status();
  }
  root.emplace("edges", std::move(*edges));

  auto paths = EncodePaths(value.paths);
  if (!paths.ok()) {
    return paths.status();
  }
  root.emplace("paths", std::move(*paths));

  auto facts = EncodeFacts(value.facts);
  if (!facts.ok()) {
    return facts.status();
  }
  root.emplace("facts", std::move(*facts));

  auto assumptions = EncodeAssumptions(value.assumptions);
  if (!assumptions.ok()) {
    return assumptions.status();
  }
  root.emplace("assumptions", std::move(*assumptions));

  auto hypotheses = EncodeHypotheses(value.hypotheses);
  if (!hypotheses.ok()) {
    return hypotheses.status();
  }
  root.emplace("hypotheses", std::move(*hypotheses));

  auto unknowns = EncodeUnknowns(value.unknowns);
  if (!unknowns.ok()) {
    return unknowns.status();
  }
  root.emplace("unknowns", std::move(*unknowns));

  auto constraints = EncodeConstraints(value.constraints);
  if (!constraints.ok()) {
    return constraints.status();
  }
  root.emplace("constraints", std::move(*constraints));

  auto provenance = EncodeProvenanceRecords(value.provenance);
  if (!provenance.ok()) {
    return provenance.status();
  }
  root.emplace("provenance", std::move(*provenance));

  auto obligations = EncodeProofObligations(value.proof_obligations);
  if (!obligations.ok()) {
    return obligations.status();
  }
  root.emplace("proof_obligations", std::move(*obligations));

  auto summaries = EncodeSummaries(value.summaries);
  if (!summaries.ok()) {
    return summaries.status();
  }
  root.emplace("summaries", std::move(*summaries));

  auto dependencies = EncodeDependencies(value.dependencies);
  if (!dependencies.ok()) {
    return dependencies.status();
  }
  root.emplace("dependencies", std::move(*dependencies));

  auto omissions = EncodeOmissions(value.omissions);
  if (!omissions.ok()) {
    return omissions.status();
  }
  root.emplace("omissions", std::move(*omissions));

  return core::Object(std::move(root));
}

}  // namespace

StatusOr<std::vector<std::byte>> CanonicalEvidenceBytes(
    const EvidenceCase& value) {
  auto canonical = CanonicalEvidenceValue(value);
  if (!canonical.ok()) {
    return canonical.status();
  }
  return core::CanonicalEncode(*canonical);
}

StatusOr<core::StableId> ComputeEvidenceId(const EvidenceCase& value) {
  auto bytes = CanonicalEvidenceBytes(value);
  if (!bytes.ok()) {
    return bytes.status();
  }
  return core::MakeStableId(core::IdKind::kEvidence, *bytes);
}

Status FinalizeEvidenceIdentity(EvidenceCase* value) {
  if (value == nullptr) {
    return Status::InvalidArgument(
        "FinalizeEvidenceIdentity requires a case to finalize");
  }

  // Validate first: an ill-formed case has no authority to be identified.
  const Status valid = RequireValidEvidenceCase(*value);
  if (!valid.ok()) {
    return valid;
  }

  // Then compute, and assign only once every earlier step has succeeded, so a
  // failed call leaves the case (and any identity it carried) untouched.
  auto computed = ComputeEvidenceId(*value);
  if (!computed.ok()) {
    return computed.status();
  }
  value->evidence_id = *computed;
  return Status::Ok();
}

}  // namespace veritas::evidence
