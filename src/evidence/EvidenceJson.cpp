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

// EvidenceJson.cpp — `ToEvidenceJson`, the full-fidelity JSON writer.
//
// The document is built as an `llvm::json::Value` tree and serialized once by
// an `llvm::json::OStream`, rather than streamed member by member. Two things
// need that shape and neither is available to a streaming writer:
//
//   * an object's members are emitted in the order `OStream` fixes for a
//     `json::Object` — lexicographically by key — which is the same order
//     `core::CanonicalEncode` emits a `core::CanonicalObject` in;
//   * a collection the model treats as unordered is sorted before it is
//     written, and the sort needs each member's value, not its text.
//
// Every record-emitter below is named `Emit<Record>` rather than `<Record>`.
// That is not decoration: a member function named `Entity` would hide the type
// `Entity` inside every member function of this class, so the lambdas that name
// a record type would not compile.
//
// Exhaustiveness over the semantic model is maintained by hand, exactly as
// `EvidenceProto.cpp` maintains it: C++20 has no reflection, so nothing here
// fails to compile when a field is added to `EvidenceCase.h`. The guard is the
// test suite's structural oracle, which names every field of every record and
// fails when one is dropped. Adding a field to a record in `EvidenceCase.h`
// therefore requires a matching change here plus an oracle entry; without
// both, the field leaves the document silently and no test would say so.
//
// A refused value never truncates the document. The first failure is kept and
// every later emit is skipped, so a caller cannot receive a document that
// looks complete while a member of it is missing.

#include "veritas/evidence/EvidenceJson.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/evidence/EvidenceCanonicalizer.h"
#include "veritas/evidence/EvidenceCase.h"
#include "veritas/evidence/EvidenceValidator.h"

namespace veritas::evidence {
namespace {

// The stable spelling of an expression kind: the operator and literal names
// the grammar uses. This is the third private copy in `src/evidence` — the
// canonicalizer and the validator each carry one — and it is kept local for
// the same reason they do: it is a spelling table for one file's own output,
// not a model API. The three must agree, and the JSON suite pins the spellings
// this file emits.
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

// `kAnd` and `kOr` are the only commutative kinds: the canonicalizer orders
// their operands by encoding, and so does the writer below. Everything else —
// a comparison's {left, right}, an implication's antecedent and consequent, a
// quantifier's domain and body, and a call's arguments — keeps the order it
// declares, because that order is what the node means.
bool IsCommutative(Expression::Kind kind) {
  return kind == Expression::Kind::kAnd || kind == Expression::Kind::kOr;
}

// One element of a canonically ordered collection, and the key that orders it.
// The three components are the canonicalizer's, in its order: the record's
// declared kind family spelled as the payload spells it, the record's canonical
// stable-ID string when it carries one, and its case-local handle.
//
// A key tie is not broken here, and that is a property of the case rather than
// a gap in this comparison: `ValidateEvidenceCase` refuses a case whose members
// share a local identifier, and the local handle is the last component of every
// key below, so the key is unique within every collection this writer sees. The
// writer validates before it emits, so a tie would mean the local-ID gate had
// been loosened — and the order would then be the input order.
struct SortKey {
  std::string kind;
  std::string stable_id;
  std::string local_id;
};

bool SortKeyLess(const SortKey& left, const SortKey& right) {
  if (left.kind != right.kind) {
    return left.kind < right.kind;
  }
  if (left.stable_id != right.stable_id) {
    return left.stable_id < right.stable_id;
  }
  return left.local_id < right.local_id;
}

std::string StableIdOrEmpty(const std::optional<core::StableId>& stable_id) {
  if (!stable_id.has_value()) {
    return std::string();
  }
  return core::ToString(*stable_id);
}

// The compact JSON text of a value, used to order the three collections whose
// members carry no distinguishing key of their own: `Path::conditions`, the
// operands of a commutative node, and `ProgramBinding::analyzer_versions`.
// This mirrors the canonicalizer's tie-break on the element's own encoding, and
// it is total for the same reason that one is — two elements of such a
// collection that render identically are the same element.
std::string JsonText(const llvm::json::Value& value) {
  std::string text;
  llvm::raw_string_ostream os(text);
  llvm::json::OStream(os, /*IndentSize=*/0).value(value);
  os.flush();
  return text;
}

bool JsonTextLess(const llvm::json::Value& left,
                  const llvm::json::Value& right) {
  return JsonText(left) < JsonText(right);
}

// --- The emitter ------------------------------------------------------------

class JsonEmitter {
 public:
  JsonEmitter() = default;

  bool ok() const { return failed_.ok(); }
  const Status& status() const { return failed_; }

  llvm::json::Value Case(const EvidenceCase& value);

 private:
  // --- The one decision -----------------------------------------------------
  //
  // Whether `text` has a spelling in JSON text, which is UTF-8 by definition
  // (RFC 8259 §8.1). Every string this boundary writes — a value, an object
  // key, a member of one of the sorted reference lists below — is put through
  // this predicate first, and no other route to the LLVM JSON library skips it.
  //
  // The predicate has to be ours because the LLVM JSON library does not refuse
  // for us. A `json::Value` or `json::ObjectKey` built from a bad string
  // asserts when asserts are on and is replaced by U+FFFD when they are off,
  // and `json::OStream` re-fixes a bad object *key* at output time — its
  // `attributeBegin` checks the key and substitutes — while never touching a
  // bad string *value*. A rewritten key is the worse of the two outcomes:
  // `evidence_id` is the content address of the *original* bytes, so a document
  // that spells a different string no longer describes the case it names.
  static bool IsSpellable(std::string_view text) {
    return llvm::json::isUTF8(text);
  }

  // Latches the refusal. The first failure is kept and every later emit is
  // skipped, so a caller cannot receive a document that looks complete while a
  // member of it is missing.
  void Refuse() {
    if (failed_.ok()) {
      failed_ = Status::InvalidArgument(
          "the case holds a string that is not valid UTF-8, and JSON text is "
          "UTF-8 by definition, so the value has no JSON spelling");
    }
  }

  // --- The two spellings of a string ----------------------------------------

  llvm::json::Value Text(std::string_view text) {
    if (!ok()) {
      return llvm::json::Value(nullptr);
    }
    if (!IsSpellable(text)) {
      Refuse();
      return llvm::json::Value(nullptr);
    }
    return llvm::json::Value(std::string(text));
  }

  // A property key. Keys take the same decision as values, for the reason
  // above. A refused key yields the empty string, which is spellable; the
  // object it would have belonged to is discarded along with the latched
  // status, so the placeholder is never observable.
  llvm::json::ObjectKey Key(std::string_view text) {
    if (!IsSpellable(text)) {
      Refuse();
      return llvm::json::ObjectKey("");
    }
    return llvm::json::ObjectKey(std::string(text));
  }

  // Sorts a reference list lexicographically by value, duplicates preserved:
  // §19.1's order for `blocking_ids`, `input_fact_ids`, `components`, and
  // `verifier_kinds`. Each member goes out through `Text`, so a list is refused
  // on the same terms as any other string rather than reaching `llvm::json`
  // unguarded.
  llvm::json::Array SortedStrings(const std::vector<std::string>& values) {
    std::vector<std::string> sorted(values);
    std::sort(sorted.begin(), sorted.end());
    llvm::json::Array array;
    for (const std::string& value : sorted) {
      array.push_back(Text(value));
    }
    return array;
  }

  llvm::json::Value OptionalText(const std::optional<core::StableId>& id) {
    if (!ok() || !id.has_value()) {
      return llvm::json::Value(nullptr);
    }
    return Text(core::ToString(*id));
  }

  // Renders `emit` over a sorted copy of `records`. `key` names the canonical
  // sort key of one record; the caller supplies it rather than this template
  // guessing, because which components a record carries is a property of the
  // record and not of the collection.
  template <typename Record, typename KeyFn, typename EmitFn>
  llvm::json::Value Collection(const std::vector<Record>& records, KeyFn key,
                               EmitFn emit) {
    struct Entry {
      SortKey sort_key;
      llvm::json::Value value;
    };
    std::vector<Entry> entries;
    entries.reserve(records.size());
    for (const Record& record : records) {
      entries.push_back(Entry{key(record), emit(record)});
    }
    std::stable_sort(entries.begin(), entries.end(),
                     [](const Entry& left, const Entry& right) {
                       return SortKeyLess(left.sort_key, right.sort_key);
                     });
    llvm::json::Array array;
    for (Entry& entry : entries) {
      array.push_back(std::move(entry.value));
    }
    return llvm::json::Value(std::move(array));
  }

  // Emits an expression tree. Every field of every node is written, leaves
  // included, so a consumer reads one shape; the sentinel kind is spelled like
  // every other sentinel the model renders.
  llvm::json::Value EmitExpression(const Expression& value);

  llvm::json::Value EmitCommutativeOperands(
      const std::vector<Expression>& values);

  llvm::json::Value EmitProgram(const ProgramBinding& value);
  llvm::json::Value EmitClaim(const Claim& value);
  llvm::json::Value EmitEntity(const Entity& value);
  llvm::json::Value EmitEdge(const Edge& value);
  llvm::json::Value EmitPath(const Path& value);
  llvm::json::Value EmitFact(const Fact& value);
  llvm::json::Value EmitAssumption(const Assumption& value);
  llvm::json::Value EmitHypothesis(const Hypothesis& value);
  llvm::json::Value EmitUnknown(const Unknown& value);
  llvm::json::Value EmitConstraint(const Constraint& value);
  llvm::json::Value EmitProvenance(const Provenance& value);
  llvm::json::Value EmitProofObligation(const ProofObligation& value);
  llvm::json::Value EmitSummaryReference(const SummaryReference& value);
  llvm::json::Value EmitDependency(const Dependency& value);
  llvm::json::Value EmitOmission(const Omission& value);

  llvm::json::Value EmitAnalyzerVersions(
      const std::vector<AnalyzerVersion>& value);

  Status failed_ = Status::Ok();
};

llvm::json::Value JsonEmitter::EmitExpression(const Expression& value) {
  llvm::json::Object object;
  object["kind"] = Text(ExpressionKindName(value.kind));
  object["text"] = Text(value.text);
  object["integer"] = llvm::json::Value(value.integer);
  object["boolean"] = llvm::json::Value(value.boolean);
  if (IsCommutative(value.kind)) {
    object["operands"] = EmitCommutativeOperands(value.operands);
  } else {
    llvm::json::Array operands;
    for (const Expression& operand : value.operands) {
      operands.push_back(EmitExpression(operand));
    }
    object["operands"] = llvm::json::Value(std::move(operands));
  }
  return llvm::json::Value(std::move(object));
}

llvm::json::Value JsonEmitter::EmitCommutativeOperands(
    const std::vector<Expression>& values) {
  std::vector<llvm::json::Value> operands;
  operands.reserve(values.size());
  for (const Expression& value : values) {
    operands.push_back(EmitExpression(value));
  }
  std::stable_sort(operands.begin(), operands.end(), JsonTextLess);
  llvm::json::Array array;
  for (llvm::json::Value& operand : operands) {
    array.push_back(std::move(operand));
  }
  return llvm::json::Value(std::move(array));
}

llvm::json::Value JsonEmitter::EmitAnalyzerVersions(
    const std::vector<AnalyzerVersion>& value) {
  std::vector<const AnalyzerVersion*> ordered;
  ordered.reserve(value.size());
  for (const AnalyzerVersion& version : value) {
    ordered.push_back(&version);
  }
  std::stable_sort(
      ordered.begin(), ordered.end(),
      [](const AnalyzerVersion* left, const AnalyzerVersion* right) {
        if (left->producer != right->producer) {
          return left->producer < right->producer;
        }
        if (left->version != right->version) {
          return left->version < right->version;
        }
        return left->configuration < right->configuration;
      });
  llvm::json::Array array;
  for (const AnalyzerVersion* version : ordered) {
    llvm::json::Object object;
    object["producer"] = Text(version->producer);
    object["version"] = Text(version->version);
    object["configuration"] = Text(version->configuration);
    array.push_back(llvm::json::Value(std::move(object)));
  }
  return llvm::json::Value(std::move(array));
}

llvm::json::Value JsonEmitter::EmitProgram(const ProgramBinding& value) {
  llvm::json::Object object;
  object["repository_id"] = Text(value.repository_id);
  object["revision_id"] = Text(value.revision_id);
  object["build_variant_id"] = Text(value.build_variant_id);
  object["target_triple"] = Text(value.target_triple);
  object["analysis_configuration_id"] = Text(value.analysis_configuration_id);
  object["type_layout_id"] = Text(value.type_layout_id);
  object["analysis_run_id"] = OptionalText(value.analysis_run_id);
  object["analyzer_versions"] = EmitAnalyzerVersions(value.analyzer_versions);
  return llvm::json::Value(std::move(object));
}

llvm::json::Value JsonEmitter::EmitClaim(const Claim& value) {
  llvm::json::Object object;
  object["id"] = Text(value.id);
  object["kind"] = Text(ToString(value.kind));
  object["subject"] = Text(value.subject);
  object["predicate"] = EmitExpression(value.predicate);
  object["severity"] = Text(ToString(value.severity));
  object["description"] = Text(value.description);
  return llvm::json::Value(std::move(object));
}

llvm::json::Value JsonEmitter::EmitEntity(const Entity& value) {
  llvm::json::Object properties;
  for (const auto& entry : value.properties) {
    properties[Key(entry.first)] = EmitExpression(entry.second);
  }
  llvm::json::Object object;
  object["id"] = Text(value.id);
  object["kind"] = Text(ToString(value.kind));
  object["stable_id"] = OptionalText(value.stable_id);
  object["properties"] = llvm::json::Value(std::move(properties));
  return llvm::json::Value(std::move(object));
}

llvm::json::Value JsonEmitter::EmitEdge(const Edge& value) {
  llvm::json::Object object;
  object["id"] = Text(value.id);
  object["from"] = Text(value.from);
  object["to"] = Text(value.to);
  object["kind"] = Text(ToString(value.kind));
  object["epistemic"] = Text(ToString(value.epistemic));
  object["provenance_id"] = Text(value.provenance_id);
  object["summarized_by"] = Text(value.summarized_by);
  object["expandable"] = llvm::json::Value(value.expandable);
  return llvm::json::Value(std::move(object));
}

llvm::json::Value JsonEmitter::EmitPath(const Path& value) {
  llvm::json::Array segments;
  for (const std::string& segment : value.entity_ids) {
    segments.push_back(Text(segment));
  }
  llvm::json::Object object;
  object["id"] = Text(value.id);
  object["kind"] = Text(ToString(value.kind));
  object["entity_ids"] = llvm::json::Value(std::move(segments));
  object["conditions"] = EmitCommutativeOperands(value.conditions);
  object["feasibility"] = Text(ToString(value.feasibility));
  object["provenance_id"] = Text(value.provenance_id);
  return llvm::json::Value(std::move(object));
}

llvm::json::Value JsonEmitter::EmitFact(const Fact& value) {
  llvm::json::Object object;
  object["id"] = Text(value.id);
  object["stable_id"] = OptionalText(value.stable_id);
  object["predicate"] = EmitExpression(value.predicate);
  object["epistemic"] = Text(ToString(value.epistemic));
  object["confidence"] = Text(ToString(value.confidence));
  object["producer"] = Text(value.producer);
  object["provenance_id"] = Text(value.provenance_id);
  object["derived"] = llvm::json::Value(value.derived);
  return llvm::json::Value(std::move(object));
}

llvm::json::Value JsonEmitter::EmitAssumption(const Assumption& value) {
  llvm::json::Object object;
  object["id"] = Text(value.id);
  object["predicate"] = EmitExpression(value.predicate);
  object["source"] = Text(value.source);
  object["scope"] = Text(value.scope);
  return llvm::json::Value(std::move(object));
}

llvm::json::Value JsonEmitter::EmitHypothesis(const Hypothesis& value) {
  llvm::json::Object object;
  object["id"] = Text(value.id);
  object["predicate"] = EmitExpression(value.predicate);
  object["producer"] = Text(value.producer);
  object["reason"] = Text(value.reason);
  object["confidence"] = Text(ToString(value.confidence));
  return llvm::json::Value(std::move(object));
}

llvm::json::Value JsonEmitter::EmitUnknown(const Unknown& value) {
  llvm::json::Object object;
  object["id"] = Text(value.id);
  object["property"] = EmitExpression(value.property);
  object["reason_code"] = Text(ToString(value.reason_code));
  object["reason"] = Text(value.reason);
  object["blocking_ids"] = llvm::json::Value(SortedStrings(value.blocking_ids));
  object["suggested_resolution"] = Text(value.suggested_resolution);
  return llvm::json::Value(std::move(object));
}

llvm::json::Value JsonEmitter::EmitConstraint(const Constraint& value) {
  llvm::json::Object object;
  object["id"] = Text(value.id);
  object["expression"] = EmitExpression(value.expression);
  object["scope"] = Text(value.scope);
  object["epistemic"] = Text(ToString(value.epistemic));
  object["provenance_id"] = Text(value.provenance_id);
  return llvm::json::Value(std::move(object));
}

llvm::json::Value JsonEmitter::EmitProvenance(const Provenance& value) {
  llvm::json::Object object;
  object["id"] = Text(value.id);
  object["producer"] = Text(value.producer);
  object["rule"] = Text(value.rule);
  object["input_fact_ids"] =
      llvm::json::Value(SortedStrings(value.input_fact_ids));
  object["source_anchor_id"] = Text(value.source_anchor_id);
  object["analysis_run_id"] = OptionalText(value.analysis_run_id);
  object["version"] = Text(value.version);
  object["configuration"] = Text(value.configuration);
  return llvm::json::Value(std::move(object));
}

llvm::json::Value JsonEmitter::EmitProofObligation(
    const ProofObligation& value) {
  llvm::json::Object object;
  object["id"] = Text(value.id);
  object["goal_kind"] = Text(ToString(value.goal_kind));
  object["predicate"] = EmitExpression(value.predicate);
  object["verifier_kinds"] =
      llvm::json::Value(SortedStrings(value.verifier_kinds));
  object["budget"] = EmitExpression(value.budget);
  object["status"] = Text(ToString(value.status));
  object["result_id"] = Text(value.result_id);
  object["verification_producer"] = Text(value.verification_producer);
  return llvm::json::Value(std::move(object));
}

llvm::json::Value JsonEmitter::EmitSummaryReference(
    const SummaryReference& value) {
  llvm::json::Object object;
  object["id"] = Text(value.id);
  object["function_id"] = Text(value.function_id);
  object["summary_id"] = Text(core::ToString(value.summary_id));
  object["components"] = llvm::json::Value(SortedStrings(value.components));
  return llvm::json::Value(std::move(object));
}

llvm::json::Value JsonEmitter::EmitDependency(const Dependency& value) {
  llvm::json::Object object;
  object["id"] = Text(value.id);
  object["kind"] = Text(ToString(value.kind));
  object["stable_id"] = Text(core::ToString(value.stable_id));
  return llvm::json::Value(std::move(object));
}

llvm::json::Value JsonEmitter::EmitOmission(const Omission& value) {
  llvm::json::Object object;
  object["id"] = Text(value.id);
  object["kind"] = Text(value.kind);
  object["subject"] = Text(value.subject);
  object["reason"] = Text(value.reason);
  object["expandable"] = llvm::json::Value(value.expandable);
  return llvm::json::Value(std::move(object));
}

llvm::json::Value JsonEmitter::Case(const EvidenceCase& value) {
  llvm::json::Object root;

  // `evidence_id` is the one member `core::CanonicalEncode` never reads — it is
  // the output of the identity computation — and it is the one member a
  // consumer of this document must be able to read, because the document's
  // whole claim to be *the* case is that it carries the same content address
  // the other two boundaries carry. The writer refuses a case that has not been
  // finalized, so it is always a string here and never a null.
  root["evidence_id"] = OptionalText(value.evidence_id);
  root["schema_version"] = Text(value.schema_version);
  root["level"] = Text(ToString(value.level));
  root["verification_state"] = Text(ToString(value.verification_state));

  root["program"] = EmitProgram(value.program);
  root["primary_claim"] = EmitClaim(value.primary_claim);

  root["entities"] = Collection(
      value.entities,
      [](const Entity& entity) {
        return SortKey{std::string(ToString(entity.kind)),
                       StableIdOrEmpty(entity.stable_id), entity.id};
      },
      [this](const Entity& entity) { return EmitEntity(entity); });

  root["edges"] = Collection(
      value.edges,
      [](const Edge& edge) {
        return SortKey{std::string(ToString(edge.kind)), std::string(),
                       edge.id};
      },
      [this](const Edge& edge) { return EmitEdge(edge); });

  root["paths"] = Collection(
      value.paths,
      [](const Path& path) {
        return SortKey{std::string(ToString(path.kind)), std::string(),
                       path.id};
      },
      [this](const Path& path) { return EmitPath(path); });

  root["facts"] = Collection(
      value.facts,
      [](const Fact& fact) {
        return SortKey{std::string(), StableIdOrEmpty(fact.stable_id), fact.id};
      },
      [this](const Fact& fact) { return EmitFact(fact); });

  root["assumptions"] = Collection(
      value.assumptions,
      [](const Assumption& assumption) {
        return SortKey{std::string(), std::string(), assumption.id};
      },
      [this](const Assumption& assumption) {
        return EmitAssumption(assumption);
      });

  root["hypotheses"] = Collection(
      value.hypotheses,
      [](const Hypothesis& hypothesis) {
        return SortKey{std::string(), std::string(), hypothesis.id};
      },
      [this](const Hypothesis& hypothesis) {
        return EmitHypothesis(hypothesis);
      });

  root["unknowns"] = Collection(
      value.unknowns,
      [](const Unknown& unknown) {
        return SortKey{std::string(), std::string(), unknown.id};
      },
      [this](const Unknown& unknown) { return EmitUnknown(unknown); });

  root["constraints"] = Collection(
      value.constraints,
      [](const Constraint& constraint) {
        return SortKey{std::string(), std::string(), constraint.id};
      },
      [this](const Constraint& constraint) {
        return EmitConstraint(constraint);
      });

  root["provenance"] = Collection(
      value.provenance,
      [](const Provenance& record) {
        return SortKey{std::string(), std::string(), record.id};
      },
      [this](const Provenance& record) { return EmitProvenance(record); });

  root["proof_obligations"] = Collection(
      value.proof_obligations,
      [](const ProofObligation& obligation) {
        return SortKey{std::string(ToString(obligation.goal_kind)),
                       std::string(), obligation.id};
      },
      [this](const ProofObligation& obligation) {
        return EmitProofObligation(obligation);
      });

  root["summaries"] = Collection(
      value.summaries,
      [](const SummaryReference& summary) {
        return SortKey{std::string(), std::string(), summary.id};
      },
      [this](const SummaryReference& summary) {
        return EmitSummaryReference(summary);
      });

  root["dependencies"] = Collection(
      value.dependencies,
      [](const Dependency& dependency) {
        return SortKey{std::string(ToString(dependency.kind)), std::string(),
                       dependency.id};
      },
      [this](const Dependency& dependency) {
        return EmitDependency(dependency);
      });

  root["omissions"] = Collection(
      value.omissions,
      [](const Omission& omission) {
        return SortKey{std::string(), std::string(), omission.id};
      },
      [this](const Omission& omission) { return EmitOmission(omission); });

  return llvm::json::Value(std::move(root));
}

}  // namespace

StatusOr<std::string> ToEvidenceJson(const EvidenceCase& value) {
  // The identity gate first, in `WriteEirText`'s order and with its semantics.
  // A case that has not been finalized carries no `evidence_id`, and one whose
  // identity is not its recomputed content address is a case whose declared
  // identity disagrees with its semantics; both are refused rather than
  // written, so this boundary and the EIR-T boundary agree about which cases
  // are serializable at all. `FinalizeEvidenceIdentity` runs the
  // well-formedness gate and recomputes rather than trusting the ID, so the
  // same two calls cover both conditions here, and the well-formedness gate is
  // reached through `RequireValidEvidenceCase` exactly as it is there.
  if (!value.evidence_id.has_value()) {
    return Status::InvalidArgument(
        "the case carries no evidence_id: the JSON writer requires a finalized "
        "case, so call FinalizeEvidenceIdentity first");
  }
  const Status valid = RequireValidEvidenceCase(value);
  if (!valid.ok()) {
    return valid;
  }
  StatusOr<core::StableId> computed = ComputeEvidenceId(value);
  if (!computed.ok()) {
    return computed.status();
  }
  if (*computed != *value.evidence_id) {
    return Status::InvalidArgument(
        "the case's evidence_id is not the content address of its own "
        "semantics, so the JSON writer will not stamp it onto a document that "
        "disagrees with it");
  }

  JsonEmitter emitter;
  const llvm::json::Value document = emitter.Case(value);
  if (!emitter.ok()) {
    return emitter.status();
  }

  std::string out;
  llvm::raw_string_ostream os(out);
  llvm::json::OStream(os, /*IndentSize=*/2).value(document);
  os.flush();
  // Exactly one trailing newline, as `build::ToDiagnosticJson` produces: the
  // document ends at the closing brace and this is the only byte after it.
  out.push_back('\n');
  return out;
}

}  // namespace veritas::evidence
