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

// EvidenceJsonTest.cpp — `ToEvidenceJson`, the full-EIR JSON boundary, and
// `REP-004`.
//
// `REP-004` names its own failure mode: "M10B slice JSON reused or field
// omitted". Two consequences follow, and they shape this suite:
//
//   * A substring check is not evidence of completeness. `EXPECT_THAT(json,
//     HasSubstr("\"omissions\""))` passes for a document that omits every other
//     member, and it passes for one that emits the member with the wrong
//     contents. So the completeness test below is a **structural oracle**: it
//     parses the document and walks it against the model, asserting for every
//     record the exact set of member names the document carries and the value
//     of every one of them. A field dropped from the writer changes the key
//     set, and the test goes red.
//   * Correctness comes from an oracle the writer does not own. The expected
//     member names are written out here, in this file, from the model in
//     `EvidenceCase.h`; the expected enum spellings are written out from the
//     grammar. Nothing in this file asks the writer what it emits.
//
// What this suite does **not** do: there is no JSON *reader* in M10C, so the
// document is not decoded back into a case and compared. `REP-004` does not ask
// for that, and the `EvidenceID` in the document is checked by construction —
// the writer refuses a case whose identity is not its recomputed content
// address, so a document that exists is a document whose `evidence_id` is
// correct. Semantic agreement among the three representations is Task 11's
// `DEM-003`, which has the CLI and a real reader to do it with.
//
// The suite covers the four obligations `REP-004` lists — reverse insertion
// order, escape strings, every semantic field, one final newline — plus the
// refusal contract that makes this boundary agree with the EIR-T one.

#include "veritas/evidence/EvidenceJson.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "evidence/EvidenceScenario.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/evidence/EirText.h"
#include "veritas/evidence/EvidenceCanonicalizer.h"
#include "veritas/evidence/EvidenceCase.h"
#include "veritas/evidence/EvidenceQueryService.h"
#include "veritas/evidence/EvidenceValidator.h"
#include "veritas/evidence/SliceTypes.h"

namespace veritas::evidence {
namespace {

using testing::EvidenceScenarioBuilder;
using testing::MakeOverflowEvidenceCase;
using testing::MakeValidMinimalEvidenceCase;

using llvm::json::Array;
using llvm::json::Object;
using llvm::json::Value;

// --- Harness ----------------------------------------------------------------

// Validates and assigns the identity, so the case is in the state the writer
// requires. A fixture that is not well-formed is a broken test rather than a
// failing behaviour, so the failure is recorded and the unfinalized case
// returned.
EvidenceCase Finalized(EvidenceCase value) {
  const Status status = FinalizeEvidenceIdentity(&value);
  if (!status.ok()) {
    ADD_FAILURE() << "FinalizeEvidenceIdentity refused: " << status.message();
  }
  return value;
}

std::string MustJson(const EvidenceCase& value) {
  StatusOr<std::string> json = ToEvidenceJson(value);
  if (!json.ok()) {
    ADD_FAILURE() << "ToEvidenceJson refused: " << json.status().message();
    return {};
  }
  return std::move(json).value();
}

std::string JsonText(const Value& value) {
  std::string text;
  llvm::raw_string_ostream os(text);
  llvm::json::OStream(os, /*IndentSize=*/0).value(value);
  os.flush();
  return text;
}

Value MustParse(llvm::StringRef text) {
  llvm::Expected<Value> parsed = llvm::json::parse(text);
  if (!parsed) {
    ADD_FAILURE() << "the document is not JSON: "
                  << llvm::toString(parsed.takeError());
    return Value(nullptr);
  }
  return std::move(*parsed);
}

// --- Structural accessors ---------------------------------------------------
//
// Each one records a failure and returns a benign default rather than
// aborting, so one malformed member reports once instead of hiding the rest of
// the oracle's findings behind a fatal return.

const Value& At(const Value& object, llvm::StringRef key) {
  static const Value missing(nullptr);
  if (const Object* members = object.getAsObject()) {
    if (const Value* found = members->get(key)) {
      return *found;
    }
  }
  EXPECT_TRUE(false) << "the document carries no \"" << key.str()
                     << "\" member at " << JsonText(object);
  return missing;
}

std::vector<std::string> Keys(const Value& value) {
  std::vector<std::string> keys;
  if (const Object* members = value.getAsObject()) {
    for (const auto& entry : *members) {
      keys.push_back(entry.first.str());
    }
  } else {
    EXPECT_TRUE(false) << "expected a JSON object, got " << JsonText(value);
  }
  std::sort(keys.begin(), keys.end());
  return keys;
}

void ExpectKeys(const Value& value, std::vector<std::string> expected) {
  std::sort(expected.begin(), expected.end());
  EXPECT_EQ(Keys(value), expected) << "at " << JsonText(value);
}

std::string StringOf(const Value& value) {
  if (std::optional<llvm::StringRef> text = value.getAsString()) {
    return text->str();
  }
  EXPECT_TRUE(false) << "expected a JSON string, got " << JsonText(value);
  return {};
}

int64_t IntOf(const Value& value) {
  if (std::optional<int64_t> number = value.getAsInteger()) {
    return *number;
  }
  EXPECT_TRUE(false) << "expected a JSON integer, got " << JsonText(value);
  return 0;
}

bool BoolOf(const Value& value) {
  if (std::optional<bool> flag = value.getAsBoolean()) {
    return *flag;
  }
  EXPECT_TRUE(false) << "expected a JSON boolean, got " << JsonText(value);
  return false;
}

const Array& ArrayOf(const Value& value) {
  static const Array empty;
  if (const Array* items = value.getAsArray()) {
    return *items;
  }
  EXPECT_TRUE(false) << "expected a JSON array, got " << JsonText(value);
  return empty;
}

// Records are located by their case-local handle rather than by position
// wherever a test names a specific one, so the assertion does not silently
// depend on the canonical ordering rule.
const Value& FindById(const Value& array, llvm::StringRef id) {
  static const Value missing(nullptr);
  for (const Value& record : ArrayOf(array)) {
    if (StringOf(At(record, "id")) == id) {
      return record;
    }
  }
  EXPECT_TRUE(false) << "no record " << id.str() << " in " << JsonText(array);
  return missing;
}

bool IsNull(const Value& value) { return value.getAsNull().has_value(); }

bool IsObject(const Value& value) { return value.getAsObject() != nullptr; }

// `null` is how the document spells "the model carries none". The canonical
// order key spells that same absence as the empty string, which is what
// `StableIdComponent` returns for a disengaged optional.
std::string OptionalStringOf(const Value& value) {
  return IsNull(value) ? std::string() : StringOf(value);
}

std::vector<std::string> StringsOf(const Value& value) {
  std::vector<std::string> out;
  for (const Value& item : ArrayOf(value)) {
    out.push_back(StringOf(item));
  }
  return out;
}

// The four reference lists the model declares sorted — `blocking_ids`,
// `input_fact_ids`, `components`, `verifier_kinds` — are compared as the sets
// they are. The fixture happens to store two of them out of order, which is
// exactly the case that would otherwise read as a writer bug: it is not, and
// the model's own contract says these lists are sets.
void ExpectSameStrings(const Value& got, std::vector<std::string> want) {
  std::vector<std::string> actual = StringsOf(got);
  std::sort(actual.begin(), actual.end());
  std::sort(want.begin(), want.end());
  EXPECT_EQ(actual, want);
}

// --- The independent spelling oracle ----------------------------------------
//
// Written from the grammar and from `EvidenceCase.h`, not from the writer. The
// third copy of these spellings lives in `EvidenceJson.cpp`; these two must
// agree, and if one is edited the other must be.

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

bool IsCommutative(Expression::Kind kind) {
  return kind == Expression::Kind::kAnd || kind == Expression::Kind::kOr;
}

// --- The model oracle -------------------------------------------------------

void CheckExpression(const Value& got, const Expression& want);

// An unordered expression list — the operands of a commutative `kAnd`/`kOr`,
// and `Path::conditions` (the conjunction the path asserts, which
// §19.1 orders by each conjunct's own encoding). Pairs each document operand
// with the model operand that carries the same `(kind, text)` and then checks
// that pair **recursively**, so a corrupted sub-operand is caught rather than
// hidden behind a top-level label. Two operands of the same node sharing both a
// kind and an operator/callee string would be indistinguishable to this
// matcher; the count assertion still holds, and no node in either fixture does
// that.
void CheckUnorderedExpressions(const Value& got,
                               const std::vector<Expression>& want) {
  const Array& operands = ArrayOf(got);
  ASSERT_EQ(operands.size(), want.size());
  std::vector<bool> used(want.size(), false);
  for (const Value& operand : operands) {
    const std::string kind = StringOf(At(operand, "kind"));
    const std::string text = StringOf(At(operand, "text"));
    bool matched = false;
    for (std::size_t i = 0; i < want.size() && !matched; ++i) {
      if (used[i] || ExpressionKindName(want[i].kind) != kind ||
          want[i].text != text) {
        continue;
      }
      used[i] = true;
      matched = true;
      CheckExpression(operand, want[i]);
    }
    EXPECT_TRUE(matched)
        << "the document carries an operand the model has no match for: "
        << JsonText(operand);
  }
}

void CheckExpression(const Value& got, const Expression& want) {
  ExpectKeys(got, {"boolean", "integer", "kind", "operands", "text"});
  EXPECT_EQ(StringOf(At(got, "kind")), ExpressionKindName(want.kind));
  EXPECT_EQ(StringOf(At(got, "text")), want.text);
  EXPECT_EQ(IntOf(At(got, "integer")), want.integer);
  EXPECT_EQ(BoolOf(At(got, "boolean")), want.boolean);
  if (IsCommutative(want.kind)) {
    CheckUnorderedExpressions(At(got, "operands"), want.operands);
    return;
  }
  const Array& operands = ArrayOf(At(got, "operands"));
  ASSERT_EQ(operands.size(), want.operands.size());
  for (std::size_t i = 0; i < want.operands.size(); ++i) {
    CheckExpression(operands[i], want.operands[i]);
  }
}

// The optional stable identity of a record: `null` when the model carries
// none, the canonical `kind:sha256:<digest>` string when it does. A field that
// disappeared would fail `ExpectKeys`; a field that degraded to the empty
// string fails here.
void CheckOptionalStableId(const Value& got,
                           const std::optional<core::StableId>& want) {
  if (!want.has_value()) {
    EXPECT_TRUE(IsNull(got)) << "expected null, got " << JsonText(got);
    return;
  }
  EXPECT_EQ(StringOf(got), core::ToString(*want));
}

void CheckStableId(const Value& got, const core::StableId& want) {
  EXPECT_EQ(StringOf(got), core::ToString(want));
}

void CheckProgram(const Value& got, const ProgramBinding& want) {
  ExpectKeys(got, {"analysis_configuration_id", "analysis_run_id",
                   "analyzer_versions", "build_variant_id", "repository_id",
                   "revision_id", "target_triple", "type_layout_id"});
  EXPECT_EQ(StringOf(At(got, "repository_id")), want.repository_id);
  EXPECT_EQ(StringOf(At(got, "revision_id")), want.revision_id);
  EXPECT_EQ(StringOf(At(got, "build_variant_id")), want.build_variant_id);
  EXPECT_EQ(StringOf(At(got, "target_triple")), want.target_triple);
  EXPECT_EQ(StringOf(At(got, "analysis_configuration_id")),
            want.analysis_configuration_id);
  EXPECT_EQ(StringOf(At(got, "type_layout_id")), want.type_layout_id);
  CheckOptionalStableId(At(got, "analysis_run_id"), want.analysis_run_id);

  const Array& versions = ArrayOf(At(got, "analyzer_versions"));
  ASSERT_EQ(versions.size(), want.analyzer_versions.size());
  for (const Value& version : versions) {
    ExpectKeys(version, {"configuration", "producer", "version"});
    const std::string producer = StringOf(At(version, "producer"));
    const std::string text = StringOf(At(version, "version"));
    const std::string configuration = StringOf(At(version, "configuration"));
    const bool found = std::any_of(
        want.analyzer_versions.begin(), want.analyzer_versions.end(),
        [&](const AnalyzerVersion& candidate) {
          return candidate.producer == producer && candidate.version == text &&
                 candidate.configuration == configuration;
        });
    EXPECT_TRUE(found) << "unexpected analyzer version " << JsonText(version);
  }
}

void CheckClaim(const Value& got, const Claim& want) {
  ExpectKeys(
      got, {"description", "id", "kind", "predicate", "severity", "subject"});
  EXPECT_EQ(StringOf(At(got, "id")), want.id);
  EXPECT_EQ(StringOf(At(got, "kind")), ToString(want.kind));
  EXPECT_EQ(StringOf(At(got, "subject")), want.subject);
  EXPECT_EQ(StringOf(At(got, "severity")), ToString(want.severity));
  EXPECT_EQ(StringOf(At(got, "description")), want.description);
  CheckExpression(At(got, "predicate"), want.predicate);
}

void CheckEntity(const Value& got, const Entity& want) {
  ExpectKeys(got, {"id", "kind", "properties", "stable_id"});
  EXPECT_EQ(StringOf(At(got, "id")), want.id);
  EXPECT_EQ(StringOf(At(got, "kind")), ToString(want.kind));
  CheckOptionalStableId(At(got, "stable_id"), want.stable_id);
  const Value& properties = At(got, "properties");
  ExpectKeys(properties, [&] {
    std::vector<std::string> names;
    for (const auto& entry : want.properties) {
      names.push_back(entry.first);
    }
    return names;
  }());
  for (const auto& entry : want.properties) {
    CheckExpression(At(properties, entry.first), entry.second);
  }
}

void CheckEdge(const Value& got, const Edge& want) {
  ExpectKeys(got, {"epistemic", "expandable", "from", "id", "kind",
                   "provenance_id", "summarized_by", "to"});
  EXPECT_EQ(StringOf(At(got, "id")), want.id);
  EXPECT_EQ(StringOf(At(got, "from")), want.from);
  EXPECT_EQ(StringOf(At(got, "to")), want.to);
  EXPECT_EQ(StringOf(At(got, "kind")), ToString(want.kind));
  EXPECT_EQ(StringOf(At(got, "epistemic")), ToString(want.epistemic));
  EXPECT_EQ(StringOf(At(got, "provenance_id")), want.provenance_id);
  EXPECT_EQ(StringOf(At(got, "summarized_by")), want.summarized_by);
  EXPECT_EQ(BoolOf(At(got, "expandable")), want.expandable);
}

void CheckPath(const Value& got, const Path& want) {
  ExpectKeys(got, {"conditions", "entity_ids", "feasibility", "id", "kind",
                   "provenance_id"});
  EXPECT_EQ(StringOf(At(got, "id")), want.id);
  EXPECT_EQ(StringOf(At(got, "kind")), ToString(want.kind));
  EXPECT_EQ(StringOf(At(got, "feasibility")), ToString(want.feasibility));
  EXPECT_EQ(StringOf(At(got, "provenance_id")), want.provenance_id);
  // The segment sequence *is* the path, so this is order-sensitive on purpose.
  EXPECT_EQ(StringsOf(At(got, "entity_ids")), want.entity_ids);
  CheckUnorderedExpressions(At(got, "conditions"), want.conditions);
}

void CheckFact(const Value& got, const Fact& want) {
  ExpectKeys(got, {"confidence", "derived", "epistemic", "id", "predicate",
                   "producer", "provenance_id", "stable_id"});
  EXPECT_EQ(StringOf(At(got, "id")), want.id);
  CheckOptionalStableId(At(got, "stable_id"), want.stable_id);
  EXPECT_EQ(StringOf(At(got, "epistemic")), ToString(want.epistemic));
  EXPECT_EQ(StringOf(At(got, "confidence")), ToString(want.confidence));
  EXPECT_EQ(StringOf(At(got, "producer")), want.producer);
  EXPECT_EQ(StringOf(At(got, "provenance_id")), want.provenance_id);
  EXPECT_EQ(BoolOf(At(got, "derived")), want.derived);
  CheckExpression(At(got, "predicate"), want.predicate);
}

void CheckAssumption(const Value& got, const Assumption& want) {
  ExpectKeys(got, {"id", "predicate", "scope", "source"});
  EXPECT_EQ(StringOf(At(got, "id")), want.id);
  EXPECT_EQ(StringOf(At(got, "source")), want.source);
  EXPECT_EQ(StringOf(At(got, "scope")), want.scope);
  CheckExpression(At(got, "predicate"), want.predicate);
}

void CheckHypothesis(const Value& got, const Hypothesis& want) {
  ExpectKeys(got, {"confidence", "id", "predicate", "producer", "reason"});
  EXPECT_EQ(StringOf(At(got, "id")), want.id);
  EXPECT_EQ(StringOf(At(got, "producer")), want.producer);
  EXPECT_EQ(StringOf(At(got, "reason")), want.reason);
  EXPECT_EQ(StringOf(At(got, "confidence")), ToString(want.confidence));
  CheckExpression(At(got, "predicate"), want.predicate);
}

void CheckUnknown(const Value& got, const Unknown& want) {
  ExpectKeys(got, {"blocking_ids", "id", "property", "reason", "reason_code",
                   "suggested_resolution"});
  EXPECT_EQ(StringOf(At(got, "id")), want.id);
  EXPECT_EQ(StringOf(At(got, "reason_code")), ToString(want.reason_code));
  EXPECT_EQ(StringOf(At(got, "reason")), want.reason);
  EXPECT_EQ(StringOf(At(got, "suggested_resolution")),
            want.suggested_resolution);
  ExpectSameStrings(At(got, "blocking_ids"), want.blocking_ids);
  CheckExpression(At(got, "property"), want.property);
}

void CheckConstraint(const Value& got, const Constraint& want) {
  ExpectKeys(got, {"epistemic", "expression", "id", "provenance_id", "scope"});
  EXPECT_EQ(StringOf(At(got, "id")), want.id);
  EXPECT_EQ(StringOf(At(got, "scope")), want.scope);
  EXPECT_EQ(StringOf(At(got, "epistemic")), ToString(want.epistemic));
  EXPECT_EQ(StringOf(At(got, "provenance_id")), want.provenance_id);
  CheckExpression(At(got, "expression"), want.expression);
}

void CheckProvenance(const Value& got, const Provenance& want) {
  ExpectKeys(got, {"analysis_run_id", "configuration", "id", "input_fact_ids",
                   "producer", "rule", "source_anchor_id", "version"});
  EXPECT_EQ(StringOf(At(got, "id")), want.id);
  EXPECT_EQ(StringOf(At(got, "producer")), want.producer);
  EXPECT_EQ(StringOf(At(got, "rule")), want.rule);
  EXPECT_EQ(StringOf(At(got, "source_anchor_id")), want.source_anchor_id);
  EXPECT_EQ(StringOf(At(got, "version")), want.version);
  EXPECT_EQ(StringOf(At(got, "configuration")), want.configuration);
  ExpectSameStrings(At(got, "input_fact_ids"), want.input_fact_ids);
  CheckOptionalStableId(At(got, "analysis_run_id"), want.analysis_run_id);
}

void CheckProofObligation(const Value& got, const ProofObligation& want) {
  ExpectKeys(got, {"budget", "goal_kind", "id", "predicate", "result_id",
                   "status", "verification_producer", "verifier_kinds"});
  EXPECT_EQ(StringOf(At(got, "id")), want.id);
  EXPECT_EQ(StringOf(At(got, "goal_kind")), ToString(want.goal_kind));
  EXPECT_EQ(StringOf(At(got, "status")), ToString(want.status));
  EXPECT_EQ(StringOf(At(got, "result_id")), want.result_id);
  EXPECT_EQ(StringOf(At(got, "verification_producer")),
            want.verification_producer);
  ExpectSameStrings(At(got, "verifier_kinds"), want.verifier_kinds);
  CheckExpression(At(got, "predicate"), want.predicate);
  CheckExpression(At(got, "budget"), want.budget);
}

void CheckSummaryReference(const Value& got, const SummaryReference& want) {
  ExpectKeys(got, {"components", "function_id", "id", "summary_id"});
  EXPECT_EQ(StringOf(At(got, "id")), want.id);
  EXPECT_EQ(StringOf(At(got, "function_id")), want.function_id);
  CheckStableId(At(got, "summary_id"), want.summary_id);
  ExpectSameStrings(At(got, "components"), want.components);
}

void CheckDependency(const Value& got, const Dependency& want) {
  ExpectKeys(got, {"id", "kind", "stable_id"});
  EXPECT_EQ(StringOf(At(got, "id")), want.id);
  EXPECT_EQ(StringOf(At(got, "kind")), ToString(want.kind));
  CheckStableId(At(got, "stable_id"), want.stable_id);
}

void CheckOmission(const Value& got, const Omission& want) {
  ExpectKeys(got, {"expandable", "id", "kind", "reason", "subject"});
  EXPECT_EQ(StringOf(At(got, "id")), want.id);
  EXPECT_EQ(StringOf(At(got, "kind")), want.kind);
  EXPECT_EQ(StringOf(At(got, "subject")), want.subject);
  EXPECT_EQ(StringOf(At(got, "reason")), want.reason);
  EXPECT_EQ(BoolOf(At(got, "expandable")), want.expandable);
}

// Matches a collection's records by their case-local handle rather than by
// position, so the field oracle is independent of the ordering rule — which
// `CollectionsAreInTheCanonicalOrderNotTheModels` checks on its own. A record
// the document omits has no entry to match, a record it invents has no model
// counterpart, and both go red. `ValidateEvidenceCase` refuses a case whose
// members share a handle, so the index is unambiguous.
template <typename Record>
void CheckCollection(const Value& array, const std::vector<Record>& records,
                     void (*check)(const Value&, const Record&)) {
  const Array& items = ArrayOf(array);
  ASSERT_EQ(items.size(), records.size());
  std::map<std::string, const Value*> by_id;
  for (const Value& item : items) {
    const std::string id = StringOf(At(item, "id"));
    EXPECT_TRUE(by_id.emplace(id, &item).second)
        << "the document carries two records with the handle " << id;
  }
  for (const Record& record : records) {
    const auto found = by_id.find(record.id);
    ASSERT_TRUE(found != by_id.end())
        << "the document omits the record " << record.id;
    check(*found->second, record);
  }
}

// The document's collections are in the order `EvidenceCanonicalizer.cpp` fixes
// for its unordered collections, never the order the model happened to be
// assembled in. The key is `(the kind spelled as the payload spells it, the
// canonical stable-ID string, the case-local handle)`, with a component a
// record family does not carry left empty — the same thing `StableIdComponent`
// does for a disengaged optional. The key is computed here from the document's
// own members, so it is an independent implementation of the documented rule
// rather than a call into the writer.
template <typename Family>
void ExpectCanonicalOrder(const Value& array, llvm::StringRef what,
                          const Family& family) {
  std::vector<std::string> keys;
  for (const Value& record : ArrayOf(array)) {
    const std::pair<std::string, std::string> parts = family(record);
    std::string key = parts.first;
    // A byte below every byte a spelling can contain, so a kind that is a
    // prefix of the next component cannot run the two together into one key.
    key.push_back('\x01');
    key += parts.second;
    key.push_back('\x01');
    key += StringOf(At(record, "id"));
    keys.push_back(std::move(key));
  }
  EXPECT_TRUE(std::is_sorted(keys.begin(), keys.end()))
      << what.str() << " are not in canonical order: " << JsonText(array);
}

// The whole case, member by member. `REP-004`'s named failure mode is
// *omission*, so this walks the model rather than the document: every
// collection is checked record by record against the model, and every key set
// is asserted exactly, so a member the writer forgets is a key set that
// disagrees and a red test.
void CheckCase(const std::string& json, const EvidenceCase& want) {
  const Value root = MustParse(json);
  ExpectKeys(root, {"assumptions", "constraints", "dependencies", "edges",
                    "entities", "evidence_id", "facts", "hypotheses", "level",
                    "omissions", "paths", "primary_claim", "program",
                    "proof_obligations", "provenance", "schema_version",
                    "summaries", "unknowns", "verification_state"});

  EXPECT_EQ(StringOf(At(root, "schema_version")), want.schema_version);
  EXPECT_EQ(StringOf(At(root, "level")), ToString(want.level));
  EXPECT_EQ(StringOf(At(root, "verification_state")),
            ToString(want.verification_state));
  CheckStableId(At(root, "evidence_id"), *want.evidence_id);

  CheckProgram(At(root, "program"), want.program);
  CheckClaim(At(root, "primary_claim"), want.primary_claim);

  CheckCollection(At(root, "entities"), want.entities, &CheckEntity);
  CheckCollection(At(root, "edges"), want.edges, &CheckEdge);
  CheckCollection(At(root, "paths"), want.paths, &CheckPath);
  CheckCollection(At(root, "facts"), want.facts, &CheckFact);
  CheckCollection(At(root, "assumptions"), want.assumptions, &CheckAssumption);
  CheckCollection(At(root, "hypotheses"), want.hypotheses, &CheckHypothesis);
  CheckCollection(At(root, "unknowns"), want.unknowns, &CheckUnknown);
  CheckCollection(At(root, "constraints"), want.constraints, &CheckConstraint);
  CheckCollection(At(root, "provenance"), want.provenance, &CheckProvenance);
  CheckCollection(At(root, "proof_obligations"), want.proof_obligations,
                  &CheckProofObligation);
  CheckCollection(At(root, "summaries"), want.summaries,
                  &CheckSummaryReference);
  CheckCollection(At(root, "dependencies"), want.dependencies, &CheckDependency);
  CheckCollection(At(root, "omissions"), want.omissions, &CheckOmission);
}

// --- Expression construction, for the commutative case ----------------------

Expression Reference(std::string name) {
  Expression value;
  value.kind = Expression::Kind::kReference;
  value.text = std::move(name);
  return value;
}

Expression Call(std::string callee, std::vector<Expression> arguments) {
  Expression value;
  value.kind = Expression::Kind::kCall;
  value.text = std::move(callee);
  value.operands = std::move(arguments);
  return value;
}

Expression Conjunction(Expression left, Expression right) {
  Expression value;
  value.kind = Expression::Kind::kAnd;
  value.operands.push_back(std::move(left));
  value.operands.push_back(std::move(right));
  return value;
}

// --- REP-004: determinism ---------------------------------------------------

TEST(EvidenceJsonTest, TheSameCaseTwiceIsByteIdentical) {
  const EvidenceCase value = Finalized(MakeOverflowEvidenceCase());
  const std::string first = MustJson(value);
  const std::string second = MustJson(value);
  EXPECT_EQ(first, second);
  EXPECT_FALSE(first.empty());
}

// The case is identical; only the order the model's unordered collections were
// assembled in changes. `REP-004` requires the document to be blind to it, and
// it must be blind to it for the *identity* too — the canonicalizer sorts the
// same collections — so the two orders are also one `EvidenceID`.
TEST(EvidenceJsonTest, ReversingIndependentMemberInsertionIsByteIdentical) {
  EvidenceCase forward = Finalized(MakeOverflowEvidenceCase());
  EvidenceCase reversed = MakeOverflowEvidenceCase();
  EvidenceScenarioBuilder::Reverse(reversed.entities);
  EvidenceScenarioBuilder::Reverse(reversed.edges);
  EvidenceScenarioBuilder::Reverse(reversed.paths);
  EvidenceScenarioBuilder::Reverse(reversed.facts);
  EvidenceScenarioBuilder::Reverse(reversed.assumptions);
  EvidenceScenarioBuilder::Reverse(reversed.hypotheses);
  EvidenceScenarioBuilder::Reverse(reversed.unknowns);
  EvidenceScenarioBuilder::Reverse(reversed.constraints);
  EvidenceScenarioBuilder::Reverse(reversed.provenance);
  EvidenceScenarioBuilder::Reverse(reversed.proof_obligations);
  EvidenceScenarioBuilder::Reverse(reversed.summaries);
  EvidenceScenarioBuilder::Reverse(reversed.dependencies);
  EvidenceScenarioBuilder::Reverse(reversed.omissions);
  EvidenceScenarioBuilder::Reverse(reversed.program.analyzer_versions);

  EXPECT_TRUE(FinalizeEvidenceIdentity(&reversed).ok());
  EXPECT_EQ(*forward.evidence_id, *reversed.evidence_id);
  EXPECT_EQ(MustJson(forward), MustJson(reversed));
}

// A commutative node's operands are a set: the canonicalizer orders them, so
// reversing them changes neither the identity nor the document. A
// non-commutative node is the opposite — `kCompare`'s {left, right} is what the
// comparison means — and the path segment sequence below is order-sensitive for
// the same reason. This is the pair of properties that makes the ordering rule
// a rule rather than a blanket sort.
TEST(EvidenceJsonTest, CommutativeOperandsAreOrderInvariant) {
  EvidenceCase forward = MakeOverflowEvidenceCase();
  const std::string subject = forward.paths.front().entity_ids.back();
  forward.paths.front().conditions = {
      Conjunction(Call("tainted", {Reference(subject)}),
                  Call("within_capacity", {Reference(subject)})),
  };
  ASSERT_TRUE(FinalizeEvidenceIdentity(&forward).ok());

  EvidenceCase reversed = forward;
  EvidenceScenarioBuilder::Reverse(reversed.paths.front().conditions.front()
                                       .operands);
  ASSERT_TRUE(FinalizeEvidenceIdentity(&reversed).ok());

  EXPECT_EQ(*forward.evidence_id, *reversed.evidence_id);
  EXPECT_EQ(MustJson(forward), MustJson(reversed));
}

// --- REP-004: completeness --------------------------------------------------

// The structural oracle. This is the test that fails when a *specific* semantic
// field is dropped: every key set and every value is asserted against the
// model, so removing a member from the writer, renaming it, or emitting it with
// the wrong contents all turn this red.
TEST(EvidenceJsonTest, EverySemanticFieldOfTheOverflowCaseReachesTheDocument) {
  const EvidenceCase value = Finalized(MakeOverflowEvidenceCase());
  CheckCase(MustJson(value), value);
}

// The order the document emits is the canonicalizer's key order, not the
// model's insertion order. This is the test that would go red if the writer
// emitted records in the order it found them, which the byte-identity test
// above cannot catch on its own when both orders happen to coincide.
TEST(EvidenceJsonTest, CollectionsAreInTheCanonicalOrderNotTheModels) {
  const EvidenceCase value = Finalized(MakeOverflowEvidenceCase());
  const Value root = MustParse(MustJson(value));

  const auto kind_and_stable = [](const Value& record) {
    return std::make_pair(StringOf(At(record, "kind")),
                          OptionalStringOf(At(record, "stable_id")));
  };
  const auto kind_only = [](const Value& record) {
    return std::make_pair(StringOf(At(record, "kind")), std::string());
  };
  const auto stable_only = [](const Value& record) {
    return std::make_pair(std::string(),
                          OptionalStringOf(At(record, "stable_id")));
  };
  const auto handle_only = [](const Value&) {
    return std::make_pair(std::string(), std::string());
  };

  ExpectCanonicalOrder(At(root, "entities"), "entities", kind_and_stable);
  ExpectCanonicalOrder(At(root, "edges"), "edges", kind_only);
  ExpectCanonicalOrder(At(root, "paths"), "paths", kind_only);
  ExpectCanonicalOrder(At(root, "facts"), "facts", stable_only);
  // A proof obligation's kind family is spelled `goal_kind`, not `kind`; the
  // key is the same either way.
  const auto goal_kind_only = [](const Value& record) {
    return std::make_pair(StringOf(At(record, "goal_kind")), std::string());
  };
  ExpectCanonicalOrder(At(root, "proof_obligations"), "proof obligations",
                       goal_kind_only);
  ExpectCanonicalOrder(At(root, "dependencies"), "dependencies",
                       kind_and_stable);
  ExpectCanonicalOrder(At(root, "omissions"), "omissions", handle_only);
  ExpectCanonicalOrder(At(root, "assumptions"), "assumptions", handle_only);
  ExpectCanonicalOrder(At(root, "hypotheses"), "hypotheses", handle_only);
  ExpectCanonicalOrder(At(root, "unknowns"), "unknowns", handle_only);
  ExpectCanonicalOrder(At(root, "constraints"), "constraints", handle_only);
  ExpectCanonicalOrder(At(root, "provenance"), "provenance", handle_only);
  ExpectCanonicalOrder(At(root, "summaries"), "summaries", handle_only);

  // A negative control the sortedness check alone cannot supply. Sortedness
  // asks "is this sequence non-decreasing under the documented key"; it would
  // also pass for a writer that used a *weaker* key the fixture happens not to
  // discriminate. The entity handles are the case in point: the document lists
  // E_vendor_validate, E_decode, E_entry — the stable-ID order — and the same
  // three handles sorted by name are E_decode, E_entry, E_vendor_validate, a
  // *different* sequence. So ordering by the handle alone is not a passing
  // strategy here.
  std::vector<std::string> handles;
  for (const Value& entity : ArrayOf(At(root, "entities"))) {
    handles.push_back(StringOf(At(entity, "id")));
  }
  std::vector<std::string> sorted_handles = handles;
  std::sort(sorted_handles.begin(), sorted_handles.end());
  EXPECT_NE(handles, sorted_handles);
}

TEST(EvidenceJsonTest, EverySemanticFieldOfTheMinimalCaseReachesTheDocument) {
  const EvidenceCase value = Finalized(MakeValidMinimalEvidenceCase());
  CheckCase(MustJson(value), value);
}

// The same oracle, over the case's *optional* members disengaged. The minimal
// case covers "a collection is empty"; this covers "an optional is null", so a
// writer that emitted an empty string instead of `null` — or dropped the key —
// is caught rather than passed over.
//
// The three optional members are not interchangeable. `ProgramBinding::
// analysis_run_id` is *required* by validation — resetting it makes the case
// ill-formed (`missing_program_context`) — so the program's run binding is
// never `null` in a document that this boundary is willing to write, and the
// only optional that can be disengaged in a valid case is a record's
// `stable_id`. Both of those are asserted below rather than assumed.
TEST(EvidenceJsonTest, DisengagedOptionalsAreNullAndNotAbsent) {
  EvidenceCase required = MakeOverflowEvidenceCase();
  required.program.analysis_run_id.reset();
  const Status refused = FinalizeEvidenceIdentity(&required);
  EXPECT_FALSE(refused.ok());
  EXPECT_NE(refused.message().find("analysis_run_id"), std::string_view::npos)
      << refused.message();

  EvidenceCase value = MakeOverflowEvidenceCase();
  for (Entity& entity : value.entities) {
    entity.stable_id.reset();
  }
  for (Fact& fact : value.facts) {
    fact.stable_id.reset();
  }
  const Status status = FinalizeEvidenceIdentity(&value);
  ASSERT_TRUE(status.ok()) << status.message();

  const Value root = MustParse(MustJson(value));
  for (const Value& entity : ArrayOf(At(root, "entities"))) {
    EXPECT_TRUE(IsNull(At(entity, "stable_id"))) << JsonText(entity);
  }
  for (const Value& fact : ArrayOf(At(root, "facts"))) {
    EXPECT_TRUE(IsNull(At(fact, "stable_id"))) << JsonText(fact);
  }
  CheckCase(MustJson(value), value);
}

// The spellings are the grammar's, not the writer's private vocabulary: these
// are the exact terminals `docs/specs/veritas-evidence-ir-formal-specification.md`
// fixes, written out here so a writer that quietly re-cased them is caught. JSON
// carries the same spellings EIR-T does, which is what lets one reader parse
// both.
TEST(EvidenceJsonTest, EnumSpellingsAreTheGrammarsNotTheWriters) {
  const EvidenceCase value = Finalized(MakeOverflowEvidenceCase());
  const Value root = MustParse(MustJson(value));

  EXPECT_EQ(StringOf(At(root, "schema_version")), "eir.v1");
  EXPECT_EQ(StringOf(At(root, "level")), "l1");
  EXPECT_EQ(StringOf(At(root, "verification_state")), "POSSIBLE_DEFECT");

  const Value& claim = At(root, "primary_claim");
  EXPECT_EQ(StringOf(At(claim, "kind")), "buffer_overflow");
  EXPECT_EQ(StringOf(At(claim, "severity")), "high");

  const Value& path = FindById(At(root, "paths"), "P_value_flow");
  EXPECT_EQ(StringOf(At(path, "kind")), "value_flow");
  EXPECT_EQ(StringOf(At(path, "feasibility")), "SAT");

  // One model, two conventions, and the document keeps both: `EpistemicState`
  // and `DependencyKind` are lower-case in the grammar, `RelationKind` is
  // upper-case. A writer that re-cased either to match the other is caught
  // here by name.
  const Value& fact = FindById(At(root, "facts"), "F_capacity");
  EXPECT_EQ(StringOf(At(fact, "epistemic")), "must");
  EXPECT_EQ(StringOf(At(fact, "confidence")), "exact");
  EXPECT_EQ(StringOf(At(At(fact, "predicate"), "kind")), "call");
  EXPECT_EQ(StringOf(At(FindById(At(root, "facts"), "F_alias_may"), "epistemic")),
            "may");
  EXPECT_EQ(
      StringOf(At(FindById(At(root, "facts"), "F_alias_may"), "confidence")),
      "medium");

  EXPECT_EQ(StringOf(At(FindById(At(root, "edges"), "ED_decode_memcpy"), "kind")),
            "CALLS");
  EXPECT_EQ(
      StringOf(At(FindById(At(root, "edges"), "ED_srcbuf_dstbuf"), "kind")),
      "MAY_ALIAS");

  const Value& obligation = FindById(At(root, "proof_obligations"), "O1");
  EXPECT_EQ(StringOf(At(obligation, "goal_kind")), "prove");
  EXPECT_EQ(StringOf(At(obligation, "status")), "PENDING");
  EXPECT_EQ(StringOf(At(At(obligation, "predicate"), "kind")), "forall");
  EXPECT_EQ(StringOf(At(At(obligation, "budget"), "kind")), "integer");

  EXPECT_EQ(
      StringOf(At(FindById(At(root, "unknowns"), "U_vendor_validate_contract"),
                  "reason_code")),
      "EXTERNAL_FUNCTION");
  EXPECT_EQ(StringOf(At(FindById(At(root, "unknowns"), "U_dominating_check_truncated"),
                       "reason_code")),
            "ANALYSIS_TIMEOUT");

  EXPECT_EQ(
      StringOf(At(FindById(At(root, "dependencies"), "DEP_summary"), "kind")),
      "summary");

  // `Omission::kind` is not an enum — it is a free string the producing
  // subsystem owns — so the document carries it verbatim rather than mapping
  // it through any spelling table.
  EXPECT_EQ(StringOf(At(FindById(At(root, "omissions"), "OM_summary_expansion"),
                       "kind")),
            "summary_expansion");
}

// `evidence_id` is excluded from the canonical bytes and is therefore the one
// member no other check in this file can see the value of. It must be the
// case's own content address, because a document that carries a different
// address than the EIR-T boundary would is not the same case.
TEST(EvidenceJsonTest, TheDocumentCarriesTheCasesOwnContentAddress) {
  const EvidenceCase value = Finalized(MakeOverflowEvidenceCase());
  const Value root = MustParse(MustJson(value));
  const std::string address = StringOf(At(root, "evidence_id"));
  EXPECT_EQ(address, core::ToString(*value.evidence_id));
  EXPECT_EQ(address.rfind("evidence:sha256:", 0), 0u);
}

// --- REP-004: escaping ------------------------------------------------------

namespace {

// The exact bytes that must survive: a quote and a backslash, which JSON must
// escape; a tab and a newline, which it may escape or not; and a byte below
// 0x20 that has no short escape, which it must escape as ``.
std::string NastyText() {
  std::string text = "quote \" backslash \\ tab \t newline \n";
  text += "control ";
  text.push_back('\x01');
  text += " end";
  return text;
}

}  // namespace

TEST(EvidenceJsonTest, EscapedStringsSurviveAParseBackToTheOriginalBytes) {
  const std::string nasty = NastyText();
  EvidenceCase value = MakeOverflowEvidenceCase();
  value.primary_claim.description = nasty;
  // A property *key* as well as a value: the object member name is the other
  // place a string can appear, and it takes a different path through
  // `llvm::json`. The key carries the escapes while the expression stays
  // well-formed — a `kReference` has to name a real entity, so the nasty bytes
  // cannot ride there.
  const std::string carrier = value.entities.front().id;
  value.entities.front().properties.emplace(
      "key \" with \\ quote", Call("tainted", {Reference(carrier)}));
  ASSERT_TRUE(FinalizeEvidenceIdentity(&value).ok());

  const std::string json = MustJson(value);

  // The writer escaped rather than emitted raw bytes: a raw 0x01 inside a JSON
  // string is not valid JSON, and a raw quote would end the string early.
  EXPECT_NE(json.find("\\\""), std::string::npos);
  EXPECT_NE(json.find("\\\\"), std::string::npos);
  EXPECT_NE(json.find("\\u0001"), std::string::npos);
  EXPECT_EQ(json.find('\x01'), std::string::npos);

  const Value root = MustParse(json);
  EXPECT_EQ(StringOf(At(At(root, "primary_claim"), "description")), nasty);

  // The property *key* round-trips too: the parser hands back the escaped
  // member name as the unescaped bytes, so a writer that escaped the key
  // wrongly — or not at all — produces an object with a different key.
  const Value& properties = At(FindById(At(root, "entities"), carrier), "properties");
  const Object* members = properties.getAsObject();
  ASSERT_NE(members, nullptr);
  const Value* recovered = members->get("key \" with \\ quote");
  ASSERT_NE(recovered, nullptr) << "the property key did not survive: "
                                << JsonText(properties);
  EXPECT_EQ(StringOf(At(*recovered, "kind")), "call");
  EXPECT_EQ(StringOf(At(*recovered, "text")), "tainted");
}

// --- REP-004: exactly one final newline -------------------------------------

// Asserted by byte: the document ends at the closing brace, then one `\n`, then
// nothing. `HasSubstr("\n")` would pass for a document with none.
TEST(EvidenceJsonTest, EndsWithExactlyOneNewline) {
  const EvidenceCase value = Finalized(MakeOverflowEvidenceCase());
  const std::string json = MustJson(value);

  ASSERT_GE(json.size(), 2u);
  EXPECT_EQ(json.back(), '\n');
  EXPECT_EQ(json[json.size() - 2], '}');
  // No third byte, and no *second* trailing newline anywhere at the end.
  EXPECT_EQ(json.find_last_not_of('\n'), json.size() - 2);
  // The document parses as a whole, so the newline is trailing whitespace and
  // not a separator hiding a second document.
  EXPECT_TRUE(IsObject(MustParse(json)));
}

TEST(EvidenceJsonTest, TheMinimalCaseAlsoEndsWithExactlyOneNewline) {
  const std::string json = MustJson(Finalized(MakeValidMinimalEvidenceCase()));
  ASSERT_GE(json.size(), 2u);
  EXPECT_EQ(json.back(), '\n');
  EXPECT_EQ(json[json.size() - 2], '}');
}

// --- The refusal contract ---------------------------------------------------

// The two boundaries must agree about which cases are serializable. Every case
// here is refused by `WriteEirText`; the JSON writer must refuse it with the
// same `StatusCode`. `MakeOverflowEvidenceCase` is deliberately absent from
// this test: its witnesses carry the producer `"evidence-query"`, whose hyphen
// has no EIR-T spelling, so EIR-T refuses it and JSON writes it. That
// divergence is by design and is pinned by the test below this one.
TEST(EvidenceJsonTest, RefusesWhatTheEirTextWriterRefuses) {
  // No identity at all: the case has not been finalized.
  const EvidenceCase unfinalized = MakeOverflowEvidenceCase();
  ASSERT_FALSE(unfinalized.evidence_id.has_value());

  // A finalized case holding a dangling reference.
  EvidenceCase dangling = Finalized(MakeOverflowEvidenceCase());
  dangling.primary_claim.subject = "E_no_such_entity";

  // A finalized case whose identity no longer matches its semantics.
  EvidenceCase stale = Finalized(MakeOverflowEvidenceCase());
  stale.primary_claim.description = "edited after the identity was computed";

  const std::vector<std::pair<std::string, const EvidenceCase*>> cases = {
      {"unfinalized", &unfinalized},
      {"dangling", &dangling},
      {"stale", &stale},
  };
  for (const auto& entry : cases) {
    const Status json = ToEvidenceJson(*entry.second).status();
    const Status text =
        WriteEirText(*entry.second, EirTextStyle::kCanonical).status();
    EXPECT_FALSE(json.ok()) << entry.first;
    EXPECT_FALSE(text.ok()) << entry.first;
    EXPECT_EQ(json.code(), text.code()) << entry.first;
    EXPECT_EQ(json.code(), StatusCode::kInvalidArgument) << entry.first;
  }
}

TEST(EvidenceJsonTest, RefusesACaseThatIsNotFinalized) {
  const Status status = ToEvidenceJson(MakeOverflowEvidenceCase()).status();
  ASSERT_FALSE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kInvalidArgument);
  EXPECT_NE(status.message().find("evidence_id"), std::string_view::npos)
      << status.message();
}

TEST(EvidenceJsonTest, RefusesAStaleEvidenceId) {
  EvidenceCase value = Finalized(MakeOverflowEvidenceCase());
  value.primary_claim.description = "edited after the identity was computed";
  const Status status = ToEvidenceJson(value).status();
  ASSERT_FALSE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kInvalidArgument);
  EXPECT_NE(status.message().find("content address"), std::string_view::npos)
      << status.message();
}

// The well-formedness gate is reached, not just the identity gate: the message
// carries the validator's own stable code, so a writer that only recomputed and
// compared the address could not produce it.
TEST(EvidenceJsonTest, RefusesAnIllFormedCaseWithTheValidatorsOwnDiagnostic) {
  EvidenceCase value = Finalized(MakeOverflowEvidenceCase());
  value.primary_claim.subject = "E_no_such_entity";
  const Status status = ToEvidenceJson(value).status();
  ASSERT_FALSE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kInvalidArgument);
  EXPECT_NE(status.message().find("invalid evidence case"),
            std::string_view::npos)
      << status.message();
  EXPECT_NE(status.message().find("dangling_reference"), std::string_view::npos)
      << status.message();
}

// A refusal, not a crash and not a silently-repaired value. `llvm::json`
// asserts on a value that is not valid UTF-8, and JSON text is UTF-8 by
// definition, so the writer refuses before the assert can fire. This is the one
// place this boundary is *narrower* than "exactly what EIR-T refuses", and it
// is narrower for a reason the format imposes rather than one invented here:
// the EIR-T producer alphabet is not checked, and must not be.
TEST(EvidenceJsonTest, RefusesAStringThatIsNotValidUtf8) {
  EvidenceCase value = MakeOverflowEvidenceCase();
  value.primary_claim.description = "bad byte: ";
  value.primary_claim.description.push_back('\xff');
  ASSERT_TRUE(FinalizeEvidenceIdentity(&value).ok());

  const Status status = ToEvidenceJson(value).status();
  ASSERT_FALSE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kInvalidArgument);
  EXPECT_NE(status.message().find("UTF-8"), std::string_view::npos)
      << status.message();
}

// --- Every string path refuses, not just the value path ---------------------
//
// A string the case carries reaches `llvm::json` by one of three routes:
// `JsonEmitter::Text` for an object value, `JsonEmitter::Key` for an object
// key, and `JsonEmitter::SortedStrings` for a member of a sorted reference
// list. All three can carry bytes this boundary cannot spell, and all three
// must refuse. The three tests below pin one route each.
//
// Each one asserts first that the mutated case is *legal* —
// `RequireValidEvidenceCase` accepts it and `FinalizeEvidenceIdentity` assigns
// it an identity. That is load-bearing: none of these three positions is
// validated, so a refusal that came from the validator would be the wrong
// refusal, and the assertion proves the refusal is this boundary's own.
//
// `ToEvidenceJson` returns `StatusOr<std::string>`, so "no document" is
// structural — a failed `StatusOr` has no value to return — and the check is
// that the result is not `Ok`.
namespace {

// A property key the validator does not look at, one byte that JSON text
// cannot carry.
constexpr char kBadByte = '\xff';

std::string BadUtf8Name() {
  std::string name = "bad ";
  name.push_back(kBadByte);
  name += " name";
  return name;
}

// Shared tail: the case is legal, and the writer refuses it with
// `InvalidArgument` and no document. The two assertions before the refusal are
// the ones that make the test about this boundary rather than about the
// validator.
void ExpectLegalCaseRefused(const EvidenceCase& value, llvm::StringRef what) {
  const Status valid = RequireValidEvidenceCase(value);
  ASSERT_TRUE(valid.ok()) << what.str() << ": " << valid.message();
  ASSERT_TRUE(value.evidence_id.has_value()) << what.str();

  const StatusOr<std::string> json = ToEvidenceJson(value);
  ASSERT_FALSE(json.ok()) << what.str() << " produced a document";
  EXPECT_EQ(json.status().code(), StatusCode::kInvalidArgument) << what.str();
  EXPECT_NE(json.status().message().find("UTF-8"), std::string_view::npos)
      << what.str() << ": " << json.status().message();
}

}  // namespace

// The object-key path. Unguarded this returns `Ok`: the key reaches
// `llvm::json::Object` without the check, `json::OStream` rewrites it to
// U+FFFD, and the document carries a replaced key while `evidence_id` stays the
// content address of the original bytes — a document that no longer means what
// the case says.
TEST(EvidenceJsonTest, RefusesAnEntityPropertyKeyThatIsNotValidUtf8) {
  EvidenceCase value = MakeOverflowEvidenceCase();
  value.entities.front().properties.emplace(
      BadUtf8Name(), Call("tainted", {Reference(value.entities.front().id)}));
  const Status status = FinalizeEvidenceIdentity(&value);
  ASSERT_TRUE(status.ok()) << status.message();
  ExpectLegalCaseRefused(value, "an entity property key");
}

// The `SortedStrings` path, one of two positions no validator rule reaches.
// Unguarded this aborts inside `llvm::json`'s UTF-8 assertion rather than
// refusing — the header's contract is that a crash is not a refusal, and here a
// crash was the only thing that happened.
TEST(EvidenceJsonTest, RefusesASummaryComponentThatIsNotValidUtf8) {
  EvidenceCase value = MakeOverflowEvidenceCase();
  ASSERT_FALSE(value.summaries.empty());
  value.summaries.front().components.push_back(BadUtf8Name());
  const Status status = FinalizeEvidenceIdentity(&value);
  ASSERT_TRUE(status.ok()) << status.message();
  ExpectLegalCaseRefused(value, "a summary component");
}

// The other unvalidated `SortedStrings` position.
TEST(EvidenceJsonTest, RefusesAVerifierKindThatIsNotValidUtf8) {
  EvidenceCase value = MakeOverflowEvidenceCase();
  ASSERT_FALSE(value.proof_obligations.empty());
  value.proof_obligations.front().verifier_kinds.push_back(BadUtf8Name());
  const Status status = FinalizeEvidenceIdentity(&value);
  ASSERT_TRUE(status.ok()) << status.message();
  ExpectLegalCaseRefused(value, "a verifier kind");
}

// The divergence the refusal contract tolerates on purpose. The fixture's
// query-completion witness carries `producer = "evidence-query"`, whose hyphen
// has no spelling in EIR-T's `Producer` production, so the EIR-T writer refuses
// the case and the JSON writer must not. A producer-alphabet check added here
// "to match" would make the DEM-001 fixture unserializable in the one format
// that can carry it.
TEST(EvidenceJsonTest, WritesACaseWhoseProducerHasNoEirTextSpelling) {
  const EvidenceCase value = Finalized(MakeOverflowEvidenceCase());
  const bool present = std::any_of(
      value.provenance.begin(), value.provenance.end(),
      [](const Provenance& record) {
        return record.producer == kQueryCompletionProducerId;
      });
  ASSERT_TRUE(present) << "the fixture no longer carries the query producer";

  EXPECT_FALSE(WriteEirText(value, EirTextStyle::kCanonical).ok());
  const std::string json = MustJson(value);
  EXPECT_NE(json.find(kQueryCompletionProducerId), std::string::npos);
}

}  // namespace
}  // namespace veritas::evidence
