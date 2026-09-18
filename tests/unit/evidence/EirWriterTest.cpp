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

// EirWriterTest.cpp — `WriteEirText`, the EIR-T writer, and REP-001.
//
// REP-001 is `write → parse → write` stability. On its own that is a
// **self-consistency** check and not a correctness one: both passes could agree
// on the same wrong bytes and the property would still hold. Two things here
// exist to close that gap, and neither is redundant with the round trip:
//
//   * `CanonicalTextOfAHandBuiltCaseIsPinned` compares the writer's output for
//     a case built in this file against text written out from the grammar by
//     hand, so the expected bytes come from the specification rather than from
//     the writer. The mutation that proves the pin bites is a one-token change,
//     not a deletion — a deletion would also be caught by a length check.
//   * `CanonicalTextOfTheHandAuthoredFixtureHasTheSameIdentity` compares the
//     writer's canonical output for the shared fixture against
//     `kOverflowEirText`, the document `tests/support/evidence/EirFixtureText.h`
//     hands down from the formal specification's §15 example. Two documents
//     agree on `EvidenceID` only if they agree on every hashed field, so this
//     is a semantic equality against an independently authored oracle.
//
// The remaining tests fall into four groups:
//
//   * the amended-grammar fields, one test each, asserting the attribute line
//     is present — a field emitted but never checked is how this milestone has
//     lost data before;
//   * the parser-side facts the writer must match byte for byte (the canonical
//     call spelling for `ResolutionAction` / `AssumptionSource` / `Scope`, the
//     `analyzer_versions` sort, the entity identity's position, and the value
//     shapes §5.1 widens);
//   * the refusals — `REP-001` cannot detect a silently dropped value, so every
//     unwritable value gets a typed error and a test that fails when the guard
//     is removed;
//   * determinism, `\r\n` input, and escaping.

#include "veritas/evidence/EirText.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "evidence/EirFixtureText.h"
#include "evidence/EvidenceScenario.h"
#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/evidence/EvidenceCanonicalizer.h"
#include "veritas/evidence/EvidenceCase.h"
#include "veritas/evidence/EvidenceValidator.h"

namespace veritas::evidence {
namespace {

using testing::kOverflowEirText;
using testing::kOverflowEirTextWithoutLabel;
using testing::kOverflowRunId;
using testing::MakeOverflowEvidenceCase;
using testing::MakeValidMinimalEvidenceCase;

// --- Harness ----------------------------------------------------------------

// Writes `value`, recording a failure rather than aborting when the write is
// refused: every caller of this helper is a positive test, and a refusal there
// should surface the writer's own message alongside the caller's assertions
// instead of hiding behind a fatal return.
std::string MustWrite(const EvidenceCase& value, EirTextStyle style) {
  StatusOr<std::string> text = WriteEirText(value, style);
  if (!text.ok()) {
    ADD_FAILURE() << "WriteEirText refused: " << text.status().message();
    return {};
  }
  return std::move(text).value();
}

std::string Canonical(const EvidenceCase& value) {
  return MustWrite(value, EirTextStyle::kCanonical);
}

EvidenceCase MustParse(std::string_view source) {
  EirParseError error;
  StatusOr<EvidenceCase> parsed = ParseEirText(source, &error);
  if (!parsed.ok()) {
    ADD_FAILURE() << "ParseEirText refused at line " << error.line << ":"
                  << error.column << ": " << error.message;
    return {};
  }
  return std::move(parsed).value();
}

// Validates and assigns the identity, so the case is in the state the writer
// requires. A fixture that is not well-formed is a broken test, not a failing
// behaviour, so the failure is recorded and the (unfinalized) case returned.
EvidenceCase Finalized(EvidenceCase value) {
  const Status status = FinalizeEvidenceIdentity(&value);
  if (!status.ok()) {
    ADD_FAILURE() << "FinalizeEvidenceIdentity refused: " << status.message();
  }
  return value;
}

// `base` with `mutate` applied and the identity recomputed, so the result is a
// case that is well-formed and self-addressed with respect to its mutation.
template <typename Mutate>
EvidenceCase Derive(const EvidenceCase& base, Mutate mutate) {
  EvidenceCase value = base;
  mutate(&value);
  return Finalized(std::move(value));
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

// The entity with local handle `id`, or null. The M10C fixtures use readable
// deterministic handles, so a test can name the member it is about.
const Entity* FindEntity(const EvidenceCase& value, std::string_view id) {
  for (const Entity& entity : value.entities) {
    if (entity.id == id) {
      return &entity;
    }
  }
  return nullptr;
}

// Every non-blank line of `text`, newline-terminated, in order. Two texts that
// differ in line breaks alone have the same projection, so this is how
// "whitespace only" is asserted: byte-identical lines, some of them dropped.
std::string WithoutBlankLines(std::string_view text) {
  std::string out;
  std::size_t start = 0;
  for (;;) {
    const std::size_t newline = text.find('\n', start);
    const std::size_t end =
        newline == std::string_view::npos ? text.size() : newline;
    if (end > start) {
      out.append(text.substr(start, end - start));
      out.push_back('\n');
    }
    if (newline == std::string_view::npos) {
      return out;
    }
    start = newline + 1;
  }
}

// The body of the first `    <header> { … }` block in `text`, or empty.
//
// Attribute names are not unique across member kinds — `producer` is written by
// four of them, `analysis_run` by two, `stable_id` by two — so an assertion
// about one member's field that searches the whole document is satisfied by a
// sibling's copy and stays green when the field it names is disabled. That is a
// test with no coverage of the thing it is named for, so every per-field
// assertion is scoped by this helper rather than by the document.
// `header` is the declaration up to but not including its brace — `entity
// E_memcpy` matches `entity E_memcpy : callsite {` as well as `entity E_memcpy
// {` — and the match must not be a prefix of a longer handle, so `entity E_a`
// never answers with `entity E_ab`.
std::string BlockBody(std::string_view text, std::string_view header) {
  const std::string open = "    " + std::string(header);
  std::size_t start = text.find(open);
  while (start != std::string_view::npos) {
    const std::size_t after = start + open.size();
    const bool whole_handle =
        after >= text.size() || text[after] == ' ' || text[after] == ':';
    if (whole_handle) {
      const std::size_t brace = text.find(" {\n", after);
      if (brace == std::string_view::npos) {
        return std::string();
      }
      const std::size_t body = brace + 3;
      const std::size_t close = text.find("\n    }\n", body);
      if (close == std::string_view::npos) {
        return std::string();
      }
      return std::string(text.substr(body, close - body + 1));
    }
    start = text.find(open, start + 1);
  }
  return std::string();
}

// --- Model construction -----------------------------------------------------

core::StableId StableId(std::string_view text) {
  StatusOr<core::StableId> parsed = core::ParseStableId(text);
  if (!parsed.ok()) {
    ADD_FAILURE() << "bad fixture stable ID " << text;
    return core::StableId{core::IdKind::kEvidence, std::string()};
  }
  return std::move(parsed).value();
}

Expression Symbol(std::string text) {
  Expression value;
  value.kind = Expression::Kind::kSymbol;
  value.text = std::move(text);
  return value;
}

Expression String(std::string text) {
  Expression value;
  value.kind = Expression::Kind::kString;
  value.text = std::move(text);
  return value;
}

Expression Integer(std::int64_t number) {
  Expression value;
  value.kind = Expression::Kind::kInteger;
  value.integer = number;
  return value;
}

Expression Reference(std::string text) {
  Expression value;
  value.kind = Expression::Kind::kReference;
  value.text = std::move(text);
  return value;
}

Expression Call(std::string callee, std::vector<Expression> arguments) {
  Expression value;
  value.kind = Expression::Kind::kCall;
  value.text = std::move(callee);
  value.operands = std::move(arguments);
  return value;
}

Expression Compare(std::string op, Expression left, Expression right) {
  Expression value;
  value.kind = Expression::Kind::kCompare;
  value.text = std::move(op);
  value.operands = {std::move(left), std::move(right)};
  return value;
}

Expression Not(Expression operand) {
  Expression value;
  value.kind = Expression::Kind::kNot;
  value.operands = {std::move(operand)};
  return value;
}

Expression Connective(Expression::Kind kind, std::vector<Expression> operands) {
  Expression value;
  value.kind = kind;
  value.operands = std::move(operands);
  return value;
}

// --- The hand-built case and its hand-written canonical text ----------------

// The smallest complete case this suite writes down by hand: two entities, the
// one primary claim, and one fact. It exists so the canonical text below is
// derived from the grammar alone, with no fixture and no writer in the loop.
//
// The two entities are declared in reverse canonical order on purpose: the
// expected text declares them `E_sink` then `E_len`, which is the writer's
// sort and not the case's insertion order.
EvidenceCase HandBuiltCase() {
  EvidenceCase value;
  value.schema_version = std::string(kEvidenceSchemaVersion);
  value.level = EvidenceLevel::kL1;
  value.verification_state = VerificationState::kPossibleDefect;

  value.program.repository_id = "radio-stack";
  value.program.revision_id = "a87f03e";
  value.program.build_variant_id = "ARM64_RELEASE";
  value.program.target_triple = "aarch64-unknown-linux-gnu";
  value.program.analysis_configuration_id = "veritas.default";
  value.program.type_layout_id = "layout:aapcs64";
  value.program.analysis_run_id = StableId(kOverflowRunId);
  value.program.analyzer_versions = {
      AnalyzerVersion{"veritas.clang", "17.0.6", "veritas.default"}};

  Entity len;
  len.id = "E_len";
  len.kind = EntityKind::kValue;
  len.properties.emplace("origin", String("packet.length"));

  Entity sink;
  sink.id = "E_sink";
  sink.kind = EntityKind::kCallSite;
  sink.properties.emplace("function", String("memcpy"));

  value.entities = {sink, len};

  value.primary_claim.id = "C1";
  value.primary_claim.kind = ClaimKind::kBufferOverflow;
  value.primary_claim.subject = "E_sink";
  value.primary_claim.predicate =
      Compare(">", Reference("E_len"), Call("capacity", {Reference("E_sink")}));
  value.primary_claim.severity = Severity::kHigh;

  Fact fact;
  fact.id = "F_range";
  fact.predicate = Call("range", {Reference("E_len"), Integer(0),
                                  Integer(65535)});
  fact.epistemic = EpistemicState::kMust;
  fact.confidence = Confidence::kExact;
  fact.producer = "clang";
  value.facts = {fact};

  return value;
}

// The canonical EIR-T for `HandBuiltCase()`, written from the grammar:
//
//   * no case label after `evidence` (§3.1);
//   * `schema`, `level`, `state`, then `context`, then the members;
//   * the context's seven single-valued properties in the writer's fixed
//     order, then the repeatable `analyzer` with both optional arguments;
//   * entities sorted by kind spelling (`callsite` before `value`);
//   * every declaration on one line, four spaces per level, no blank lines.
// The canonical EIR-T for `kOverflowEirTextWithoutLabel`, reviewed line by line
// against the grammar before being pinned. It carries the same case as the
// hand-authored fixture in `EirFixtureText.h` — asserted below through
// `EvidenceID`, which hashes every semantic field — and differs from that
// document only where §19.1's canonical ordering fixes a difference:
//
//   * members are grouped by category and, within a category, sorted by the
//     canonicalizer's `(kind, stable-id, local-id)` key — so the facts lead with
//     the two that have no stable ID, `provenance` follows `constraint`, and
//     the dependencies lead with the `configuration` one;
//   * the four reference lists are sorted (`inputs` is
//     `[$F_capacity, $F_range]`, not the fixture's source order);
//   * the `analyzer` lines are sorted.
//
// Within a declaration the attribute order is the writer's fixed order, which
// the parser does not constrain: `derived` follows `provenance` on
// `F_missing_check`, and `summarized_by` precedes `expandable` on `ED_flow`.
constexpr std::string_view kFixtureCanonical = R"EIR(evidence {
    schema = "eir.v1";
    level = l1;
    state = POSSIBLE_DEFECT;
    context {
        repository = "radio-stack";
        revision = "a87f03e";
        build_variant = "ARM64_RELEASE";
        target = "aarch64-unknown-linux-gnu";
        analyzer_configuration = "veritas.default";
        type_layout = "layout:aapcs64";
        analysis_run = "run:sha256:0d5c1e2f3a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5";
        analyzer = veritas.clang("17.0.6", "veritas.default");
        analyzer = veritas.wpa("1.0");
    }
    entity E_sink : callsite {
        function = "memcpy";
    }
    entity E_validate : function {
        origin = "validate_packet";
    }
    entity E_vendor_validate : function {
        origin = "vendor_validate";
    }
    entity E_dst : memory_object {
        allocation_site = "decode_frame";
    }
    entity E_packet_length : value {
        origin = "packet.length";
    }
    entity E_len : value {
        stable_id = "valref:sha256:1111111111111111111111111111111111111111111111111111111111111111";
        origin = @E_packet_length;
    }
    claim C1 {
        kind = buffer_overflow;
        subject = @E_sink;
        predicate = @E_len > capacity(@E_dst);
        severity = high;
        description = "the destination buffer is smaller than the length copied";
    }
    fact F_capacity {
        predicate = capacity(@E_dst) == 2048;
        epistemic = must;
        confidence = high;
    }
    fact F_missing_check {
        predicate = not dominates(@E_validate, @E_sink);
        epistemic = inferred;
        confidence = medium;
        provenance = @PR_check;
        derived = true;
    }
    fact F_range {
        stable_id = "fact:sha256:2222222222222222222222222222222222222222222222222222222222222222";
        predicate = range(@E_len, 0, 65535);
        epistemic = must;
        confidence = exact;
        source = clang;
    }
    assumption A1 {
        predicate = @E_len >= 0;
        source = infer_contract(E_vendor_validate);
        scope = decode_frame(E_sink);
    }
    unknown U_vendor {
        property = not validated(@E_vendor_validate);
        reason = EXTERNAL_FUNCTION;
        detail = "vendor_validate has no body in this build variant";
        blocking = [@F_missing_check];
        suggested_resolution = infer_contract(E_vendor_validate);
    }
    edge ED_flow {
        from = @E_len;
        to = @E_sink;
        kind = FLOWS_TO;
        epistemic = must;
        provenance = @PR_check;
        summarized_by = @S1;
        expandable = true;
    }
    path P1 value_flow {
        @E_len -> @E_sink;
        conditions {
            @E_len > 2048;
        }
        feasible = SAT;
        provenance = @PR_check;
    }
    constraint K1 {
        expr = @E_len <= 65535;
        scope = global;
        epistemic = must;
        provenance = @PR_check;
    }
    provenance PR_check {
        producer = veritas.wpa;
        rule = "dominates.absence";
        inputs = [$F_capacity, $F_range];
        source_anchor = "decode.cpp:281:9";
        analysis_run = "run:sha256:0d5c1e2f3a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5";
        version = "1.0";
        configuration = "veritas.default";
    }
    provenance PR_result {
        producer = veritas.smt;
        rule = "range.satisfiability";
        inputs = [$F_missing_check];
        analysis_run = "run:sha256:0d5c1e2f3a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5";
    }
    verify V1 {
        prove = @E_len <= capacity(@E_dst);
        using = [smt, symbolic];
        budget = 5000;
        status = PROVED;
        result = @PR_result;
        producer = veritas.smt;
    }
    summary S1 {
        function = @E_vendor_validate;
        summary_id = "summary:sha256:3333333333333333333333333333333333333333333333333333333333333333";
        components = [range, value_flow];
    }
    dependency D2 {
        kind = configuration;
        stable_id = "model:sha256:4444444444444444444444444444444444444444444444444444444444444444";
    }
    dependency D1 {
        kind = summary;
        stable_id = "summary:sha256:3333333333333333333333333333333333333333333333333333333333333333";
    }
    omission O1 {
        kind = analyzer_expansion;
        subject = @S1;
        reason = "the vendor callee is outside this build variant";
        expandable = true;
    }
}
)EIR";

constexpr std::string_view kHandBuiltCanonical = R"EIR(evidence {
    schema = "eir.v1";
    level = l1;
    state = POSSIBLE_DEFECT;
    context {
        repository = "radio-stack";
        revision = "a87f03e";
        build_variant = "ARM64_RELEASE";
        target = "aarch64-unknown-linux-gnu";
        analyzer_configuration = "veritas.default";
        type_layout = "layout:aapcs64";
        analysis_run = "run:sha256:0d5c1e2f3a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5";
        analyzer = veritas.clang("17.0.6", "veritas.default");
    }
    entity E_sink : callsite {
        function = "memcpy";
    }
    entity E_len : value {
        origin = "packet.length";
    }
    claim C1 {
        kind = buffer_overflow;
        subject = @E_sink;
        predicate = @E_len > capacity(@E_sink);
        severity = high;
    }
    fact F_range {
        predicate = range(@E_len, 0, 65535);
        epistemic = must;
        confidence = exact;
        source = clang;
    }
}
)EIR";

// --- The canonical pin and REP-001 -----------------------------------------

TEST(EirWriterTest, CanonicalTextOfAHandBuiltCaseIsPinned) {
  const EvidenceCase value = Finalized(HandBuiltCase());
  EXPECT_EQ(Canonical(value), std::string(kHandBuiltCanonical));
}

TEST(EirWriterTest, HandBuiltPinParsesBackToTheSameCase) {
  const EvidenceCase value = Finalized(HandBuiltCase());
  const EvidenceCase reparsed = MustParse(kHandBuiltCanonical);
  ASSERT_TRUE(reparsed.evidence_id.has_value());
  ASSERT_TRUE(value.evidence_id.has_value());
  EXPECT_EQ(*reparsed.evidence_id, *value.evidence_id);
  EXPECT_EQ(Canonical(reparsed), std::string(kHandBuiltCanonical));
}

TEST(EirWriterTest, CanonicalTextRoundTripsIdentity) {
  EvidenceCase input = MakeValidMinimalEvidenceCase();
  ASSERT_TRUE(FinalizeEvidenceIdentity(&input).ok());
  StatusOr<std::string> text = WriteEirText(input, EirTextStyle::kCanonical);
  ASSERT_TRUE(text.ok()) << text.status().message();
  EirParseError error;
  StatusOr<EvidenceCase> output = ParseEirText(*text, &error);
  ASSERT_TRUE(output.ok()) << error.message;
  EXPECT_EQ(output->evidence_id, input.evidence_id);
  EXPECT_EQ(*WriteEirText(*output, EirTextStyle::kCanonical), *text);
}

// FINDING, not a defect in the writer. `MakeOverflowEvidenceCase()` — the
// DEM-001 L1 fixture — carries `Provenance::producer = "evidence-query"`, the
// `kQueryCompletionProducerId` the M10C builder copies out of the M9 query
// completion binding. That string is not a `QualifiedId`: `Producer` is
// deliberately not widened by §5.1, so the alphabet is letters, digits,
// underscores, and interior dots, and a hyphen has no spelling. The case
// therefore has **no EIR-T serialization at all** today, and §4.1's refusal is
// the correct behaviour rather than a bug in this writer.
//
// This test pins that state so it cannot change silently. When the builder
// starts emitting a spellable producer — `evidence.query`, say — this test goes
// red, and the case it names becomes writable.
TEST(EirWriterTest, RefusesTheAsBuiltDemoCaseForItsUnspellableProducer) {
  EvidenceCase input = MakeOverflowEvidenceCase();
  ASSERT_TRUE(FinalizeEvidenceIdentity(&input).ok());
  const StatusOr<std::string> text =
      WriteEirText(input, EirTextStyle::kCanonical);
  ASSERT_FALSE(text.ok());
  EXPECT_EQ(text.status().code(), StatusCode::kInvalidArgument);
  EXPECT_TRUE(Contains(text.status().message(), "evidence-query"))
      << text.status().message();
}

// The writer's canonical text for the shared fixture is a *third* document
// beside the two hand-authored ones, and it agrees with them on `EvidenceID`.
// Agreement on the content address means agreement on every hashed field, so
// this is semantic equality against an oracle the writer did not produce.
TEST(EirWriterTest, CanonicalTextOfTheHandAuthoredFixtureHasTheSameIdentity) {
  const EvidenceCase labelled = MustParse(kOverflowEirText);
  const EvidenceCase bare = MustParse(kOverflowEirTextWithoutLabel);

  // §3.1: the display label carries no semantic content.
  ASSERT_TRUE(labelled.evidence_id.has_value());
  ASSERT_TRUE(bare.evidence_id.has_value());
  EXPECT_EQ(*labelled.evidence_id, *bare.evidence_id);

  const std::string text = Canonical(bare);
  const EvidenceCase reparsed = MustParse(text);
  ASSERT_TRUE(reparsed.evidence_id.has_value());
  EXPECT_EQ(*reparsed.evidence_id, *labelled.evidence_id);

  // Both source documents and the round-tripped result write one text.
  EXPECT_EQ(Canonical(labelled), text);
  EXPECT_EQ(Canonical(reparsed), text);
}

TEST(EirWriterTest, CanonicalTextIsPinnedForTheSharedFixture) {
  const EvidenceCase value = MustParse(kOverflowEirTextWithoutLabel);
  EXPECT_EQ(Canonical(value), std::string(kFixtureCanonical));
}

// The writer emits NO top-level case identifier. An `E_<digest>` label would
// round-trip to the same `EvidenceID`, so the round-trip tests above cannot see
// it; this asserts the token that follows `evidence` directly, and separately
// that the case's own digest appears nowhere in the text a label could carry it
// into.
TEST(EirWriterTest, CanonicalTextCarriesNoTopLevelCaseLabel) {
  const EvidenceCase value = Finalized(HandBuiltCase());
  const std::string text = Canonical(value);

  ASSERT_GE(text.size(), std::string_view("evidence {").size());
  EXPECT_EQ(text.substr(0, std::string_view("evidence {").size()),
            "evidence {");
  EXPECT_EQ(text[std::string_view("evidence ").size()], '{');

  // The label the plan would have derived from `EvidenceID` would carry the
  // digest's 64 hex characters, and no other string in a case is 64 hex. Its
  // absence is a second, independent way of seeing that no label was emitted —
  // one that a mutation adding a label cannot survive.
  ASSERT_TRUE(value.evidence_id.has_value());
  const std::string spelled = core::ToString(*value.evidence_id);
  const std::size_t colon = spelled.rfind(':');
  ASSERT_NE(colon, std::string::npos);
  const std::string digest = spelled.substr(colon + 1);
  ASSERT_EQ(digest.size(), std::size_t{64});
  EXPECT_FALSE(Contains(text, digest)) << text;
}

// --- Style ------------------------------------------------------------------

TEST(EirWriterTest, PrettyDiffersFromCanonicalInWhitespaceOnly) {
  const EvidenceCase value = MustParse(kOverflowEirTextWithoutLabel);
  const std::string canonical = Canonical(value);
  const std::string pretty = MustWrite(value, EirTextStyle::kPretty);

  // Every non-blank line is byte-identical, in the same order: the modes differ
  // in line breaks and never in a token, a value, or an ordering.
  EXPECT_EQ(WithoutBlankLines(pretty), WithoutBlankLines(canonical));
  // ...and the mode is not a no-op, so the assertion above cannot pass by the
  // two texts being the same one.
  EXPECT_NE(pretty, canonical);
  EXPECT_TRUE(Contains(pretty, "\n\n"));

  const EvidenceCase reparsed = MustParse(pretty);
  ASSERT_TRUE(reparsed.evidence_id.has_value());
  EXPECT_EQ(*reparsed.evidence_id, *value.evidence_id);
}

TEST(EirWriterTest, PrettyOutputIsDeterministic) {
  const EvidenceCase value = MustParse(kOverflowEirTextWithoutLabel);
  EXPECT_EQ(MustWrite(value, EirTextStyle::kPretty),
            MustWrite(value, EirTextStyle::kPretty));
}

// --- Escaping and line endings ---------------------------------------------

TEST(EirWriterTest, EscapesTheFiveLexerEscapesInsideStringLiterals) {
  const EvidenceCase base = Finalized(MakeValidMinimalEvidenceCase());
  ASSERT_FALSE(base.entities.empty());
  const std::string original = "quote:\" back:\\ newline:\n return:\r tab:\t";
  const EvidenceCase value = Derive(base, [&original](EvidenceCase* case_) {
    case_->entities[0].properties["note"] = String(original);
  });

  const std::string text = Canonical(value);
  EXPECT_TRUE(Contains(text,
                       "note = \"quote:\\\" back:\\\\ newline:\\n return:\\r "
                       "tab:\\t\";"))
      << text;

  // No raw line break is ever emitted inside a literal: the `note` declaration
  // is one line, and the text carries no carriage return at all.
  EXPECT_FALSE(Contains(text, "\r"));
  EXPECT_EQ(text.find("note = "), text.rfind("note = "));

  const EvidenceCase reparsed = MustParse(text);
  const Entity* entity = nullptr;
  for (const Entity& candidate : reparsed.entities) {
    if (candidate.id == base.entities[0].id) {
      entity = &candidate;
    }
  }
  ASSERT_NE(entity, nullptr);
  const auto note = entity->properties.find("note");
  ASSERT_NE(note, entity->properties.end());
  EXPECT_EQ(note->second.kind, Expression::Kind::kString);
  EXPECT_EQ(note->second.text, original);
}

// Task 6's finding F4: the lexer's `\r\n` line counting was never exercised,
// and a writer that reads back its own `\r\n`-free output never will. The same
// document with CRLF endings must parse to the same case and write one text.
TEST(EirWriterTest, CarriageReturnInputWritesTheSameCanonicalText) {
  std::string crlf;
  crlf.reserve(kOverflowEirTextWithoutLabel.size());
  for (const char c : kOverflowEirTextWithoutLabel) {
    if (c == '\n') {
      crlf.push_back('\r');
    }
    crlf.push_back(c);
  }
  ASSERT_NE(crlf, std::string(kOverflowEirTextWithoutLabel));

  const EvidenceCase lf_case = MustParse(kOverflowEirTextWithoutLabel);
  const EvidenceCase crlf_case = MustParse(crlf);
  ASSERT_TRUE(lf_case.evidence_id.has_value());
  ASSERT_TRUE(crlf_case.evidence_id.has_value());
  EXPECT_EQ(*crlf_case.evidence_id, *lf_case.evidence_id);
  EXPECT_EQ(Canonical(crlf_case), Canonical(lf_case));
}

// --- Amended-grammar fields -------------------------------------------------

// One test per field the grammar amendment (Task 7a) made writable. Each
// asserts the attribute line's presence, so disabling that field's emission
// turns exactly this test red.

TEST(EirWriterTest, EmitsContextTypeLayoutAndAnalysisRun) {
  const std::string text = Canonical(MustParse(kOverflowEirTextWithoutLabel));
  // Scoped to `context`, because `analysis_run` is written by provenance
  // records too: an unscoped needle passes even when the context emission is
  // disabled, which is a false pass rather than a test.
  const std::string context = BlockBody(text, "context");
  ASSERT_FALSE(context.empty()) << text;
  EXPECT_TRUE(Contains(context, "type_layout = \"layout:aapcs64\";")) << text;
  EXPECT_TRUE(Contains(context, std::string("analysis_run = \"") +
                                    std::string(kOverflowRunId) + "\";"))
      << text;
}

TEST(EirWriterTest, EmitsEveryAnalyzerVersionInCanonicalOrder) {
  const std::string text = Canonical(MustParse(kOverflowEirTextWithoutLabel));
  EXPECT_TRUE(Contains(text,
                       "analyzer = veritas.clang(\"17.0.6\", "
                       "\"veritas.default\");"))
      << text;
  EXPECT_TRUE(Contains(text, "analyzer = veritas.wpa(\"1.0\");")) << text;
  // Sorted, so the WPA analyzer follows the clang one even though the two
  // source orderings are equally valid.
  EXPECT_LT(text.find("analyzer = veritas.clang"), text.find("wpa"));
}

TEST(EirWriterTest, EmitsTheEntityStableId) {
  const EvidenceCase value = MustParse(kOverflowEirTextWithoutLabel);
  const std::string text = Canonical(value);
  const Entity* entity = FindEntity(value, "E_len");
  ASSERT_NE(entity, nullptr);
  ASSERT_TRUE(entity->stable_id.has_value());
  // The value comes from the case; its presence in the text comes from the
  // writer. Disabling the emission removes the line and fails this test.
  EXPECT_TRUE(Contains(text, "stable_id = \"" +
                                  core::ToString(*entity->stable_id) + "\";"))
      << text;
}

TEST(EirWriterTest, EmitsTheFactStableIdAndDerivedFlag) {
  const EvidenceCase value = MustParse(kOverflowEirTextWithoutLabel);
  const std::string text = Canonical(value);
  ASSERT_FALSE(value.facts.empty());
  const Fact& fact = value.facts[0];
  ASSERT_TRUE(fact.stable_id.has_value());
  EXPECT_TRUE(
      Contains(text, "stable_id = \"" + core::ToString(*fact.stable_id) + "\";"))
      << text;
  EXPECT_TRUE(Contains(text, "derived = true;")) << text;
}

TEST(EirWriterTest, EmitsEveryProvenanceField) {
  const EvidenceCase value = MustParse(kOverflowEirTextWithoutLabel);
  const std::string text = Canonical(value);
  const Provenance* record = nullptr;
  for (const Provenance& candidate : value.provenance) {
    if (candidate.id == "PR_check") {
      record = &candidate;
    }
  }
  ASSERT_NE(record, nullptr);
  ASSERT_TRUE(record->analysis_run_id.has_value());
  // Scoped to `PR_check`: `analysis_run` is written by the context block too,
  // and §11.1 requires the two values to match, so an unscoped needle passes
  // even with this record's emission disabled. `producer` and `stable_id` are
  // likewise written by other member kinds.
  const std::string block = BlockBody(text, "provenance PR_check");
  ASSERT_FALSE(block.empty()) << text;
  EXPECT_TRUE(Contains(block, "producer = veritas.wpa;")) << text;
  EXPECT_TRUE(Contains(block, "rule = \"dominates.absence\";")) << text;
  EXPECT_TRUE(Contains(block, "inputs = [$F_capacity, $F_range];")) << text;
  EXPECT_TRUE(Contains(block, "source_anchor = \"decode.cpp:281:9\";")) << text;
  EXPECT_TRUE(Contains(block, "analysis_run = \"" +
                                  core::ToString(*record->analysis_run_id) +
                                  "\";"))
      << text;
  EXPECT_TRUE(Contains(block, "version = \"1.0\";")) << text;
  EXPECT_TRUE(Contains(block, "configuration = \"veritas.default\";")) << text;
  // `location` names something else and the model has no member for it, so the
  // writer must never emit it — a document carrying one is refused by the
  // parser with a typed "unsupported in EIR V0.1" diagnostic. The needle is the
  // attribute form, not the bare word: `allocation_site` is a substring.
  EXPECT_FALSE(Contains(text, "location = ")) << text;
}

TEST(EirWriterTest, EmitsTheUnknownDetail) {
  const EvidenceCase value = MustParse(kOverflowEirTextWithoutLabel);
  const std::string text = Canonical(value);
  ASSERT_FALSE(value.unknowns.empty());
  EXPECT_TRUE(Contains(text, "detail = \"" + value.unknowns[0].reason + "\";"))
      << text;
  EXPECT_TRUE(Contains(text, "reason = EXTERNAL_FUNCTION;")) << text;
}

TEST(EirWriterTest, EmitsTheVerificationProducer) {
  const EvidenceCase value = MustParse(kOverflowEirTextWithoutLabel);
  const std::string text = Canonical(value);
  ASSERT_FALSE(value.proof_obligations.empty());
  // Scoped to the obligation: provenance record `PR_result` writes the same
  // `producer = veritas.smt;`, so an unscoped needle passes even with this
  // emission disabled.
  const std::string block = BlockBody(text, "verify " + value.proof_obligations[0].id);
  ASSERT_FALSE(block.empty()) << text;
  EXPECT_TRUE(Contains(
      block, "producer = " + value.proof_obligations[0].verification_producer +
                 ";"))
      << text;
  EXPECT_TRUE(Contains(block, "prove = @E_len <= capacity(@E_dst);")) << text;
}

// `Hypothesis::producer` is required by the grammar — unlike `Fact`'s and the
// verification obligation's, it has no absent spelling — so it is one of the
// two carriers whose emptiness is unwritable rather than omissible.
TEST(EirWriterTest, EmitsTheHypothesisProducer) {
  const EvidenceCase base = Finalized(MakeValidMinimalEvidenceCase());
  const EvidenceCase value = Derive(base, [](EvidenceCase* case_) {
    Hypothesis record;
    record.id = "H1";
    record.predicate = Symbol("guessed");
    record.producer = "veritas.agent";
    record.confidence = Confidence::kLow;
    case_->hypotheses.push_back(std::move(record));
  });
  const std::string text = Canonical(value);
  const std::string block = BlockBody(text, "hypothesis H1");
  ASSERT_FALSE(block.empty()) << text;
  EXPECT_TRUE(Contains(block, "producer = veritas.agent;")) << text;
  EXPECT_TRUE(Contains(block, "predicate = guessed;")) << text;
  EXPECT_EQ(Canonical(MustParse(text)), text);
}

TEST(EirWriterTest, OmitsAttributesThatAreAbsentRatherThanWritingADefault) {
  const EvidenceCase value = Finalized(HandBuiltCase());
  const std::string text = Canonical(value);
  // The hand-built fact carries no provenance, no stable ID, and is not
  // derived; none of those attributes may be invented on the way out.
  EXPECT_FALSE(Contains(text, "provenance = ")) << text;
  EXPECT_FALSE(Contains(text, "derived = ")) << text;
  EXPECT_FALSE(Contains(text, "description = ")) << text;
  EXPECT_FALSE(Contains(text, "confidence = unspecified")) << text;
  // ...and the case still round-trips, which is what makes the omission
  // lossless rather than lossy.
  const EvidenceCase reparsed = MustParse(text);
  EXPECT_EQ(*reparsed.evidence_id, *value.evidence_id);
}

// --- Parser-side facts the writer must match --------------------------------

// The call-shaped carriers — `Unknown::suggested_resolution`,
// `Assumption::source`, and `Assumption::scope` / `Constraint::scope` — are
// plain strings in the model and lower to the call's canonical EIR-T spelling,
// which writes a reference argument **bare**. The writer must reproduce that
// spelling byte for byte, and a bare-argument call must be its own fixpoint.
TEST(EirWriterTest, CallShapedCarriersRoundTripThroughTheCanonicalSpelling) {
  const EvidenceCase base = Finalized(MakeValidMinimalEvidenceCase());

  const std::string bare_call = "infer_contract(E_vendor_validate)";
  const std::string with_arguments =
      "infer_contract(E_vendor_validate, \"packet\", 1)";
  const std::string call_scope = "decode_frame(E_sink)";

  const EvidenceCase value = Derive(base, [&](EvidenceCase* case_) {
    Assumption assumption;
    assumption.id = "A1";
    assumption.predicate = Symbol("assumed");
    assumption.source = bare_call;
    assumption.scope = call_scope;
    case_->assumptions = {assumption};

    Unknown unknown;
    unknown.id = "U1";
    unknown.property = Symbol("unproven");
    unknown.reason_code = UnknownReasonCode::kExternalFunction;
    unknown.suggested_resolution = with_arguments;
    case_->unknowns = {unknown};

    Constraint constraint;
    constraint.id = "K1";
    constraint.expression = Symbol("holds");
    constraint.scope = "global";
    constraint.epistemic = EpistemicState::kMust;
    case_->constraints = {constraint};
  });

  const std::string text = Canonical(value);
  EXPECT_TRUE(Contains(text, "source = " + bare_call + ";")) << text;
  EXPECT_TRUE(Contains(text, "scope = " + call_scope + ";")) << text;
  EXPECT_TRUE(Contains(text, "suggested_resolution = " + with_arguments + ";"))
      << text;
  EXPECT_TRUE(Contains(text, "scope = global;")) << text;

  const EvidenceCase reparsed = MustParse(text);
  EXPECT_EQ(Canonical(reparsed), text);
  ASSERT_EQ(reparsed.assumptions.size(), std::size_t{1});
  EXPECT_EQ(reparsed.assumptions[0].source, bare_call);
  EXPECT_EQ(reparsed.assumptions[0].scope, call_scope);
  ASSERT_EQ(reparsed.unknowns.size(), std::size_t{1});
  EXPECT_EQ(reparsed.unknowns[0].suggested_resolution, with_arguments);
  ASSERT_EQ(reparsed.constraints.size(), std::size_t{1});
  EXPECT_EQ(reparsed.constraints[0].scope, "global");
}

// `analyzer_versions` order is not semantic (spec §198-200) but the parser
// preserves source order, so a writer that did not sort would give one case two
// texts and let two spellings of one case disagree after any later re-parse.
TEST(EirWriterTest, AnalyzerVersionOrderDoesNotChangeTextOrIdentity) {
  const EvidenceCase base = Finalized(MakeValidMinimalEvidenceCase());
  const std::vector<AnalyzerVersion> forward = {
      AnalyzerVersion{"veritas.clang", "17.0.6", "veritas.default"},
      AnalyzerVersion{"veritas.wpa", "1.0", ""},
      AnalyzerVersion{"veritas.smt", "", "z3.4"}};
  std::vector<AnalyzerVersion> reversed = forward;
  testing::EvidenceScenarioBuilder::Reverse(reversed);

  const EvidenceCase left = Derive(base, [&forward](EvidenceCase* case_) {
    case_->program.analyzer_versions = forward;
  });
  const EvidenceCase right = Derive(base, [&reversed](EvidenceCase* case_) {
    case_->program.analyzer_versions = reversed;
  });

  ASSERT_TRUE(left.evidence_id.has_value());
  ASSERT_TRUE(right.evidence_id.has_value());
  EXPECT_EQ(*left.evidence_id, *right.evidence_id);
  const std::string text = Canonical(left);
  EXPECT_EQ(Canonical(right), text);
  EXPECT_TRUE(Contains(text, "analyzer = veritas.smt(\"\", \"z3.4\");")) << text;
}

// §4.1: an entity may declare a `stable_id` identity *and* carry a property
// named `stable_id`. The identity is bound by the body's first position alone,
// so the writer must emit it first; emitting the bag entry first would silently
// re-bind identity on the next parse.
TEST(EirWriterTest, EntityIdentityIsEmittedBeforeASameNamedProperty) {
  const EvidenceCase base = Finalized(MakeValidMinimalEvidenceCase());
  ASSERT_FALSE(base.entities.empty());
  const std::string identity =
      "valref:sha256:55555555555555555555555555555555555555555555555555555555"
      "55555555";
  const std::string property = "not an identity";

  const EvidenceCase value = Derive(base, [&](EvidenceCase* case_) {
    case_->entities[0].stable_id = StableId(identity);
    case_->entities[0].properties["stable_id"] = String(property);
  });

  const std::string text = Canonical(value);
  const std::string identity_line = "stable_id = \"" + identity + "\";";
  const std::string property_line = "stable_id = \"" + property + "\";";
  ASSERT_TRUE(Contains(text, identity_line)) << text;
  ASSERT_TRUE(Contains(text, property_line)) << text;
  EXPECT_LT(text.find(identity_line), text.find(property_line));

  const EvidenceCase reparsed = MustParse(text);
  ASSERT_TRUE(reparsed.evidence_id.has_value());
  EXPECT_EQ(*reparsed.evidence_id, *value.evidence_id);

  const Entity* entity = nullptr;
  for (const Entity& candidate : reparsed.entities) {
    if (candidate.id == base.entities[0].id) {
      entity = &candidate;
    }
  }
  ASSERT_NE(entity, nullptr);
  ASSERT_TRUE(entity->stable_id.has_value());
  EXPECT_EQ(core::ToString(*entity->stable_id), identity);
  const auto bag = entity->properties.find("stable_id");
  ASSERT_NE(bag, entity->properties.end());
  EXPECT_EQ(bag->second.kind, Expression::Kind::kString);
  EXPECT_EQ(bag->second.text, property);
}

// §5.1's widenings: a bare identifier is a `kSymbol` in both the predicate and
// the property-value positions, and a `StringLiteral` or `IntegerLiteral` is
// admissible at the primary level. Re-serialisation must stay inside that set.
TEST(EirWriterTest, WidenedValueShapesRoundTrip) {
  const EvidenceCase base = Finalized(MakeValidMinimalEvidenceCase());
  const EvidenceCase value = Derive(base, [](EvidenceCase* case_) {
    ASSERT_FALSE(case_->entities.empty());
    case_->entities[0].properties["bare"] = Symbol("some_symbol");
    case_->entities[0].properties["text"] = String("a string");
    case_->entities[0].properties["count"] = Integer(-7);
    case_->facts[0].predicate = Symbol("bare_symbol_predicate");
  });

  const std::string text = Canonical(value);
  EXPECT_TRUE(Contains(text, "bare = some_symbol;")) << text;
  EXPECT_TRUE(Contains(text, "text = \"a string\";")) << text;
  EXPECT_TRUE(Contains(text, "count = -7;")) << text;
  EXPECT_TRUE(Contains(text, "predicate = bare_symbol_predicate;")) << text;

  const EvidenceCase reparsed = MustParse(text);
  EXPECT_EQ(*reparsed.evidence_id, *value.evidence_id);
  EXPECT_EQ(Canonical(reparsed), text);
}

// A whole predicate that is a lone integer or string literal, and a nested call
// argument that is one, are both legal primaries.
TEST(EirWriterTest, LiteralPrimariesAndNestedCallsRoundTrip) {
  const EvidenceCase base = Finalized(MakeValidMinimalEvidenceCase());
  ASSERT_GE(base.entities.size(), std::size_t{2});
  const std::string left = base.entities[0].id;
  const std::string right = base.entities[1].id;

  const EvidenceCase value = Derive(base, [](EvidenceCase* case_) {
    case_->facts[0].predicate = Integer(65535);
  });
  const std::string text = Canonical(value);
  EXPECT_TRUE(Contains(text, "predicate = 65535;")) << text;
  EXPECT_EQ(Canonical(MustParse(text)), text);

  const EvidenceCase nested =
      Derive(base, [&left, &right](EvidenceCase* case_) {
        case_->facts[0].predicate = Not(
            Call("dominates", {Reference(left), Reference(right)}));
      });
  const std::string nested_text = Canonical(nested);
  EXPECT_TRUE(Contains(nested_text, "predicate = not dominates(@" + left +
                                        ", @" + right + ");"))
      << nested_text;
  EXPECT_EQ(Canonical(MustParse(nested_text)), nested_text);
}

// --- Expression precedence --------------------------------------------------

TEST(EirWriterTest, PrecedenceDecidesWhereParenthesesGo) {
  const EvidenceCase base = Finalized(MakeValidMinimalEvidenceCase());

  // A conjunction nested inside a disjunction needs no parentheses when it is
  // on the left, and the model's `and` is n-ary and flat.
  const EvidenceCase flat = Derive(base, [](EvidenceCase* case_) {
    case_->facts[0].predicate = Connective(
        Expression::Kind::kOr,
        {Connective(Expression::Kind::kAnd,
                    {Symbol("a"), Symbol("b"), Symbol("c")}),
         Symbol("d")});
  });
  const std::string flat_text = Canonical(flat);
  EXPECT_TRUE(Contains(flat_text, "predicate = a and b and c or d;"))
      << flat_text;
  EXPECT_EQ(Canonical(MustParse(flat_text)), flat_text);

  // The mirror image does need them: a disjunction inside a conjunction binds
  // more loosely than the position admits.
  const EvidenceCase nested = Derive(base, [](EvidenceCase* case_) {
    case_->facts[0].predicate = Connective(
        Expression::Kind::kAnd,
        {Symbol("a"),
         Connective(Expression::Kind::kOr, {Symbol("b"), Symbol("c")})});
  });
  const std::string nested_text = Canonical(nested);
  EXPECT_TRUE(Contains(nested_text, "predicate = a and (b or c);"))
      << nested_text;
  EXPECT_EQ(Canonical(MustParse(nested_text)), nested_text);

  // A comparison binds tighter than `not`, so the negation needs none; but the
  // comparison's own operands may not be formulas, so this is the only shape in
  // which a comparison can sit inside one.
  const EvidenceCase negated = Derive(base, [](EvidenceCase* case_) {
    case_->facts[0].predicate = Not(Compare("<", Symbol("a"), Symbol("b")));
  });
  const std::string negated_text = Canonical(negated);
  EXPECT_TRUE(Contains(negated_text, "predicate = not (a < b);"))
      << negated_text;
  EXPECT_EQ(Canonical(MustParse(negated_text)), negated_text);

  // `implies` is right-associative: a right-nested chain is written bare...
  const EvidenceCase right = Derive(base, [](EvidenceCase* case_) {
    case_->facts[0].predicate = Connective(
        Expression::Kind::kImplies,
        {Symbol("a"),
         Connective(Expression::Kind::kImplies, {Symbol("b"), Symbol("c")})});
  });
  const std::string right_text = Canonical(right);
  EXPECT_TRUE(Contains(right_text, "predicate = a implies b implies c;"))
      << right_text;
  EXPECT_EQ(Canonical(MustParse(right_text)), right_text);

  // ...and a left-nested one must be parenthesised, or the chain would re-read
  // as the right-nested case above.
  const EvidenceCase left = Derive(base, [](EvidenceCase* case_) {
    case_->facts[0].predicate = Connective(
        Expression::Kind::kImplies,
        {Connective(Expression::Kind::kImplies, {Symbol("a"), Symbol("b")}),
         Symbol("c")});
  });
  const std::string left_text = Canonical(left);
  EXPECT_TRUE(Contains(left_text, "predicate = (a implies b) implies c;"))
      << left_text;
  EXPECT_EQ(Canonical(MustParse(left_text)), left_text);

  // An `and` antecedent binds tighter than the implication, so it is bare.
  const EvidenceCase antecedent = Derive(base, [](EvidenceCase* case_) {
    case_->facts[0].predicate = Connective(
        Expression::Kind::kImplies,
        {Connective(Expression::Kind::kAnd, {Symbol("a"), Symbol("b")}),
         Symbol("c")});
  });
  const std::string antecedent_text = Canonical(antecedent);
  EXPECT_TRUE(Contains(antecedent_text, "predicate = a and b implies c;"))
      << antecedent_text;
  EXPECT_EQ(Canonical(MustParse(antecedent_text)), antecedent_text);
}

// --- Determinism ------------------------------------------------------------

// The writer's order is a function of the case's meaning, not of the order the
// case happened to be assembled in: the canonicalizer sorts every unordered
// member collection, and the writer follows it.
TEST(EirWriterTest, MemberOrderIsIndependentOfInsertionOrder) {
  const EvidenceCase base = MakeValidMinimalEvidenceCase();

  std::vector<EvidenceCase> permutations;
  permutations.push_back(base);
  permutations.push_back(base);
  testing::EvidenceScenarioBuilder::Reverse(permutations[1].entities);
  testing::EvidenceScenarioBuilder::Reverse(permutations[1].facts);
  testing::EvidenceScenarioBuilder::Reverse(permutations[1].edges);
  testing::EvidenceScenarioBuilder::Reverse(permutations[1].paths);
  testing::EvidenceScenarioBuilder::Reverse(permutations[1].provenance);
  testing::EvidenceScenarioBuilder::Reverse(permutations[1].summaries);
  testing::EvidenceScenarioBuilder::Reverse(permutations[1].dependencies);
  testing::EvidenceScenarioBuilder::Reverse(permutations[1].omissions);
  testing::EvidenceScenarioBuilder::Reverse(permutations[1].unknowns);

  const EvidenceCase forward = Finalized(permutations[0]);
  const EvidenceCase reversed = Finalized(permutations[1]);
  ASSERT_TRUE(forward.evidence_id.has_value());
  ASSERT_TRUE(reversed.evidence_id.has_value());
  EXPECT_EQ(*forward.evidence_id, *reversed.evidence_id);
  EXPECT_EQ(Canonical(reversed), Canonical(forward));
}

// --- Refusals ---------------------------------------------------------------

// The three values §4.1 records as having no EIR-T spelling at all. `REP-001`
// cannot detect any of them — a dropped value is absent from both passes — so
// each is a typed error, and each test here fails when its guard is removed.

TEST(EirWriterTest, RefusesAScopeOutsideTheSixKeywordsAndFunctionCall) {
  const EvidenceCase base = Finalized(MakeValidMinimalEvidenceCase());
  const EvidenceCase value = Derive(base, [](EvidenceCase* case_) {
    Constraint constraint;
    constraint.id = "K_scope";
    constraint.expression = Symbol("holds");
    constraint.scope = "everywhere";
    constraint.epistemic = EpistemicState::kMust;
    case_->constraints.push_back(std::move(constraint));
  });
  const StatusOr<std::string> text =
      WriteEirText(value, EirTextStyle::kCanonical);
  ASSERT_FALSE(text.ok());
  EXPECT_EQ(text.status().code(), StatusCode::kInvalidArgument);
  EXPECT_TRUE(Contains(text.status().message(), "everywhere"))
      << text.status().message();
  // The document is still well-formed and still carries an identity, so the
  // refusal is the writer's own and not the validator's.
  EXPECT_TRUE(RequireValidEvidenceCase(value).ok());
}

TEST(EirWriterTest, RefusesAProducerTheQualifiedIdAlphabetCannotSpell) {
  const EvidenceCase base = Finalized(MakeValidMinimalEvidenceCase());
  for (const std::string_view producer : {"veritas-wpa", "veritas wpa"}) {
    const EvidenceCase value =
        Derive(base, [&producer](EvidenceCase* case_) {
          ASSERT_FALSE(case_->facts.empty());
          case_->facts[0].producer = std::string(producer);
        });
    // Still a well-formed case that carries an identity: the refusal is the
    // writer's own, because the value genuinely has no spelling.
    EXPECT_TRUE(RequireValidEvidenceCase(value).ok()) << producer;
    const StatusOr<std::string> text =
        WriteEirText(value, EirTextStyle::kCanonical);
    ASSERT_FALSE(text.ok()) << producer;
    EXPECT_EQ(text.status().code(), StatusCode::kInvalidArgument) << producer;
    EXPECT_TRUE(Contains(text.status().message(), producer))
        << text.status().message();
  }

  // An *absent* producer is not the same value as an unspellable one: `Fact`,
  // the analyzer version, and the verification obligation all have an optional
  // producer, so an empty one is spelled by writing nothing and reads back as
  // empty. Pinned here so the guard above cannot be widened into a blanket
  // "empty producer is fatal" rule that would refuse writable cases.
  const EvidenceCase omitted = Derive(base, [](EvidenceCase* case_) {
    ASSERT_FALSE(case_->facts.empty());
    case_->facts[0].producer.clear();
  });
  const StatusOr<std::string> text =
      WriteEirText(omitted, EirTextStyle::kCanonical);
  ASSERT_TRUE(text.ok()) << text.status().message();
  const EvidenceCase reparsed = MustParse(*text);
  ASSERT_EQ(reparsed.facts.size(), std::size_t{1});
  EXPECT_TRUE(reparsed.facts[0].producer.empty());

  // By contrast `Hypothesis` and `Provenance` have no optional producer: the
  // grammar requires the attribute, so there is nothing to omit and the empty
  // string is genuinely unwritable. Without these two cases the emptiness
  // guard in `ProducerCarrier` is dead code — no test fails when it is
  // removed, which is the defect this test exists to prevent.
  const EvidenceCase hypothesis = Derive(base, [](EvidenceCase* case_) {
    Hypothesis record;
    record.id = "H1";
    record.predicate = Symbol("guessed");
    record.producer.clear();
    case_->hypotheses.push_back(std::move(record));
  });
  EXPECT_TRUE(RequireValidEvidenceCase(hypothesis).ok());
  const StatusOr<std::string> hypothesis_text =
      WriteEirText(hypothesis, EirTextStyle::kCanonical);
  ASSERT_FALSE(hypothesis_text.ok());
  EXPECT_EQ(hypothesis_text.status().code(), StatusCode::kInvalidArgument);
  // The needle is the emptiness diagnostic, not merely the refusal: the
  // identifier check would refuse an empty string anyway, so a weaker
  // assertion would stay green with the guard deleted. This one names the
  // reason, so deleting the guard turns it red.
  EXPECT_TRUE(Contains(hypothesis_text.status().message(),
                       "hypothesis producer is empty and a producer has no "
                       "empty spelling"))
      << hypothesis_text.status().message();

  const EvidenceCase provenance = Derive(base, [](EvidenceCase* case_) {
    ASSERT_FALSE(case_->provenance.empty());
    case_->provenance[0].producer.clear();
  });
  EXPECT_TRUE(RequireValidEvidenceCase(provenance).ok());
  const StatusOr<std::string> provenance_text =
      WriteEirText(provenance, EirTextStyle::kCanonical);
  ASSERT_FALSE(provenance_text.ok());
  EXPECT_EQ(provenance_text.status().code(), StatusCode::kInvalidArgument);
  EXPECT_TRUE(Contains(provenance_text.status().message(),
                       "provenance producer is empty and a producer has no "
                       "empty spelling"))
      << provenance_text.status().message();
}

TEST(EirWriterTest, RefusesAStableIdPropertyOnAnEntityWithNoIdentity) {
  const EvidenceCase base = Finalized(MakeValidMinimalEvidenceCase());
  const EvidenceCase value = Derive(base, [](EvidenceCase* case_) {
    ASSERT_FALSE(case_->entities.empty());
    case_->entities[0].stable_id.reset();
    case_->entities[0].properties["stable_id"] = String("not an identity");
  });
  EXPECT_TRUE(RequireValidEvidenceCase(value).ok());
  const StatusOr<std::string> text =
      WriteEirText(value, EirTextStyle::kCanonical);
  ASSERT_FALSE(text.ok());
  EXPECT_EQ(text.status().code(), StatusCode::kInvalidArgument);
  EXPECT_TRUE(Contains(text.status().message(), "stable_id"))
      << text.status().message();
}

TEST(EirWriterTest, RefusesAnUnfinalizedCase) {
  const EvidenceCase value = MakeOverflowEvidenceCase();
  ASSERT_FALSE(value.evidence_id.has_value());
  const StatusOr<std::string> text =
      WriteEirText(value, EirTextStyle::kCanonical);
  ASSERT_FALSE(text.ok());
  EXPECT_EQ(text.status().code(), StatusCode::kInvalidArgument);
  // The wording unique to *this* branch. Both `WriteEirText` refusals name
  // `evidence_id`, so the loose needle stayed green with the guard deleted —
  // and that guard is load-bearing beyond the diagnostic: without it the case
  // falls through to the identity comparison and dereferences an empty
  // `optional`.
  EXPECT_TRUE(Contains(text.status().message(), "carries no evidence_id"))
      << text.status().message();
  EXPECT_TRUE(Contains(text.status().message(), "FinalizeEvidenceIdentity"))
      << text.status().message();
}

TEST(EirWriterTest, RefusesAnEvidenceIdThatIsNotTheContentAddress) {
  const EvidenceCase base = Finalized(MakeValidMinimalEvidenceCase());
  const EvidenceCase value = [&base]() {
    EvidenceCase mutated = base;
    // A semantic change with the identity left stale: the case is well-formed
    // but its declared identity no longer describes its bytes.
    mutated.primary_claim.description += " (revised)";
    return mutated;
  }();
  EXPECT_TRUE(RequireValidEvidenceCase(value).ok());
  const StatusOr<std::string> text =
      WriteEirText(value, EirTextStyle::kCanonical);
  ASSERT_FALSE(text.ok());
  EXPECT_EQ(text.status().code(), StatusCode::kInvalidArgument);
  // The sibling of `RefusesAnUnfinalizedCase`'s needle: the two refusals share
  // the word `evidence_id`, so each asserts the wording only it can produce.
  EXPECT_TRUE(Contains(text.status().message(), "not the content address"))
      << text.status().message();
}

TEST(EirWriterTest, RefusesARawLineBreakInACallShapedCarrier) {
  const EvidenceCase base = Finalized(MakeValidMinimalEvidenceCase());
  const EvidenceCase value = Derive(base, [](EvidenceCase* case_) {
    case_->facts[0].provenance_id = "";
    Assumption assumption;
    assumption.id = "A1";
    assumption.predicate = Symbol("assumed");
    assumption.source = "infer_contract(\"a\nb\")";
    case_->assumptions = {assumption};
  });
  const StatusOr<std::string> text =
      WriteEirText(value, EirTextStyle::kCanonical);
  ASSERT_FALSE(text.ok());
  EXPECT_EQ(text.status().code(), StatusCode::kInvalidArgument);
}

TEST(EirWriterTest, RefusesACarrierThatIsNotACanonicalCallSpelling) {
  const EvidenceCase base = Finalized(MakeValidMinimalEvidenceCase());
  for (const std::string_view source :
       {"infer_contract(a,b)", "infer_contract(@E_x)",
        "infer_contract() trailing", "infer_contract(\"a\\nb\")"}) {
    const EvidenceCase value = Derive(base, [&source](EvidenceCase* case_) {
      Assumption assumption;
      assumption.id = "A1";
      assumption.predicate = Symbol("assumed");
      assumption.source = std::string(source);
      case_->assumptions = {assumption};
    });
    const StatusOr<std::string> text =
        WriteEirText(value, EirTextStyle::kCanonical);
    ASSERT_FALSE(text.ok()) << source;
    EXPECT_EQ(text.status().code(), StatusCode::kInvalidArgument);
  }
}

TEST(EirWriterTest, RefusesACompoundPropertyValue) {
  const EvidenceCase base = Finalized(MakeValidMinimalEvidenceCase());
  const EvidenceCase value = Derive(base, [](EvidenceCase* case_) {
    ASSERT_FALSE(case_->entities.empty());
    case_->entities[0].properties["x"] =
        Connective(Expression::Kind::kAnd,
                   {Symbol("a"), Symbol("b")});
  });
  // The property position admits no parenthesised sub-expression, so the value
  // is unspellable there — and the validator, which checks only that the
  // property holds a well-formed expression, does not reject it.
  EXPECT_TRUE(RequireValidEvidenceCase(value).ok());
  const StatusOr<std::string> text =
      WriteEirText(value, EirTextStyle::kCanonical);
  ASSERT_FALSE(text.ok());
  EXPECT_EQ(text.status().code(), StatusCode::kInvalidArgument);
}

TEST(EirWriterTest, RefusesAnIdentifierThatIsNotAnIdentifier) {
  const EvidenceCase base = Finalized(MakeValidMinimalEvidenceCase());
  const EvidenceCase value = Derive(base, [](EvidenceCase* case_) {
    ASSERT_FALSE(case_->facts.empty());
    case_->facts[0].id = "not an identifier";
  });
  EXPECT_TRUE(RequireValidEvidenceCase(value).ok());
  const StatusOr<std::string> text =
      WriteEirText(value, EirTextStyle::kCanonical);
  ASSERT_FALSE(text.ok());
  EXPECT_EQ(text.status().code(), StatusCode::kInvalidArgument);
}

TEST(EirWriterTest, RefusesASymbolThatWouldReadBackAsAnOperatorOrLiteral) {
  const EvidenceCase base = Finalized(MakeValidMinimalEvidenceCase());
  for (const std::string_view name : {"not", "and", "or", "implies", "forall",
                                      "exists", "true", "false"}) {
    const EvidenceCase value = Derive(base, [&name](EvidenceCase* case_) {
      case_->facts[0].predicate = Symbol(std::string(name));
    });
    const StatusOr<std::string> text =
        WriteEirText(value, EirTextStyle::kCanonical);
    ASSERT_FALSE(text.ok()) << name;
    EXPECT_EQ(text.status().code(), StatusCode::kInvalidArgument);
  }
}

// §4.1's fourth unwritable value, and the only one no round trip can reach: a
// property-bag key that is not an `Identifier`. The key arrives as an arbitrary
// `std::string` from an open `std::map`, `RequireValidEvidenceCase` does not
// examine it, and the grammar fixes `PropertyKey ::= Identifier`, so the writer
// is the only gate there is.
//
// It has two distinct outcomes, and the second is why the guard must be on the
// key rather than on whether the output parses. Each is a test of its own, so
// that removing the guard fails both of them by name: an assertion shared with
// scenario (2) would abort here first and leave that one unobserved.
//
// Scenario (1): the key produces text the grammar does not derive.
TEST(EirWriterTest, RefusesAPropertyKeyTheGrammarCannotDerive) {
  const EvidenceCase base = Finalized(MakeValidMinimalEvidenceCase());
  ASSERT_FALSE(base.entities.empty());
  const std::string owner = base.entities[0].id;

  // `not` is an operator, so the parser stops at `expected '=' after the
  // attribute 'not'` — `REP-001` broken by text the writer produced itself.
  const EvidenceCase unparsable = Derive(base, [](EvidenceCase* case_) {
    case_->entities[0].properties["not an identifier"] = String("v");
  });
  // The refusal is the writer's own: the case is well-formed and identified.
  EXPECT_TRUE(RequireValidEvidenceCase(unparsable).ok());
  const StatusOr<std::string> refused =
      WriteEirText(unparsable, EirTextStyle::kCanonical);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.status().code(), StatusCode::kInvalidArgument);
  EXPECT_TRUE(Contains(refused.status().message(), owner))
      << refused.status().message();
  EXPECT_TRUE(Contains(refused.status().message(), "not an identifier"))
      << refused.status().message();
}

// Scenario (2), the silent one: text that *is* derivable and denotes a
// different case. Emitted verbatim, the key `x = 1; y` writes
// `x = 1; y = "v";` — a legal property `x`, then a legal property `y` — so the
// parser accepts it and returns a case the writer was never handed. `denoted`
// is exactly that case, and its identity differs from `injected`'s, which is
// the defect in one assertion.
TEST(EirWriterTest, RefusesAPropertyKeyThatWouldSubstituteADifferentCase) {
  const EvidenceCase base = Finalized(MakeValidMinimalEvidenceCase());
  ASSERT_FALSE(base.entities.empty());

  const EvidenceCase injected = Derive(base, [](EvidenceCase* case_) {
    case_->entities[0].properties["x = 1; y"] = String("v");
  });
  const EvidenceCase denoted = Derive(base, [](EvidenceCase* case_) {
    case_->entities[0].properties["x"] = Integer(1);
    case_->entities[0].properties["y"] = String("v");
  });
  ASSERT_TRUE(injected.evidence_id.has_value());
  ASSERT_TRUE(denoted.evidence_id.has_value());
  EXPECT_NE(*denoted.evidence_id, *injected.evidence_id);

  EXPECT_TRUE(RequireValidEvidenceCase(injected).ok());
  const StatusOr<std::string> injection =
      WriteEirText(injected, EirTextStyle::kCanonical);
  ASSERT_FALSE(injection.ok());
  EXPECT_EQ(injection.status().code(), StatusCode::kInvalidArgument);
  EXPECT_TRUE(Contains(injection.status().message(), "x = 1; y"))
      << injection.status().message();
}

// The positive control for both refusals above: the guard refuses the alphabet,
// not the bag. A legal key still writes, still round-trips, and — the point of
// the second half — still spells out what the pre-fix writer would have emitted
// for scenario (2). That text reparses *without error* into `denoted`, which is
// what makes scenario (2) a silent substitution rather than a hypothetical.
TEST(EirWriterTest, WritesALegalPropertyKeyVerbatimAndRoundTrips) {
  const EvidenceCase base = Finalized(MakeValidMinimalEvidenceCase());
  ASSERT_FALSE(base.entities.empty());
  const std::string owner = base.entities[0].id;

  const EvidenceCase legal = Derive(base, [](EvidenceCase* case_) {
    case_->entities[0].properties["note"] = String("v");
  });
  const std::string text = Canonical(legal);
  EXPECT_TRUE(Contains(BlockBody(text, "entity " + owner), "note = \"v\";"))
      << text;
  EXPECT_EQ(*MustParse(text).evidence_id, *legal.evidence_id);

  const EvidenceCase injected = Derive(base, [](EvidenceCase* case_) {
    case_->entities[0].properties["x = 1; y"] = String("v");
  });
  const EvidenceCase denoted = Derive(base, [](EvidenceCase* case_) {
    case_->entities[0].properties["x"] = Integer(1);
    case_->entities[0].properties["y"] = String("v");
  });

  std::string pre_fix = text;
  const std::size_t at = pre_fix.find("note = \"v\";");
  ASSERT_NE(at, std::string::npos) << text;
  pre_fix.replace(at, std::string("note").size(), "x = 1; y");
  const EvidenceCase reparsed = MustParse(pre_fix);
  EXPECT_EQ(*reparsed.evidence_id, *denoted.evidence_id);
  EXPECT_NE(*reparsed.evidence_id, *injected.evidence_id);
}

// The canonicalizer sorts `summaries` by local handle alone — its middle key
// component is empty — so the writer must too. A `summary_id` inserted into the
// middle component reorders the text whenever it disagrees with the handle's
// order, printing a different sequence from the one the case's identity is
// built on. `S1` carries the higher `summary_id` and so sorts first by handle
// and last by summary ID: the two orders disagree by construction.
TEST(EirWriterTest, SummaryAndDependencyOrderMatchesTheCanonicalizer) {
  const EvidenceCase base = Finalized(MakeValidMinimalEvidenceCase());
  ASSERT_FALSE(base.entities.empty());
  const std::string function = base.entities[0].id;

  const std::string high = "summary:sha256:" + std::string(64, 'f');
  const std::string low = "summary:sha256:" + std::string(64, '0');

  const EvidenceCase value = Derive(base, [&](EvidenceCase* case_) {
    // Inserted in the order the pre-fix writer would have printed them, so the
    // assertion below is not satisfied by the model happening to be sorted.
    SummaryReference second;
    second.id = "S2";
    second.function_id = function;
    second.summary_id = StableId(low);
    case_->summaries.push_back(second);
    SummaryReference first;
    first.id = "S1";
    first.function_id = function;
    first.summary_id = StableId(high);
    case_->summaries.push_back(first);

    Dependency two;
    two.id = "D2";
    two.kind = DependencyKind::kSummary;
    two.stable_id = StableId(low);
    case_->dependencies.push_back(two);
    Dependency one;
    one.id = "D1";
    one.kind = DependencyKind::kSummary;
    one.stable_id = StableId(high);
    case_->dependencies.push_back(one);
  });

  const std::string text = Canonical(value);
  // `id` order, not `summary_id`/`stable_id` order.
  ASSERT_TRUE(Contains(text, "summary S1 {")) << text;
  EXPECT_LT(text.find("summary S1 {"), text.find("summary S2 {")) << text;
  EXPECT_LT(text.find("dependency D1 {"), text.find("dependency D2 {")) << text;

  // The order is the canonicalizer's, so the round trip is exact.
  const EvidenceCase reparsed = MustParse(text);
  EXPECT_EQ(*reparsed.evidence_id, *value.evidence_id);
  EXPECT_EQ(Canonical(reparsed), text);
}

// --- Reachable refusal guards -----------------------------------------------

// Each guard below is reachable by direct probe but was pinned by no test, so
// removing it left the whole suite green. That is how an over-accepting writer
// is re-admitted silently: the guard is the only thing keeping the written
// language inside the grammar, and nothing would notice its loss. One test per
// guard, each failing when that guard is removed.

// A `kSymbol` in a *value* position whose text is `true` or `false` reads back
// as a boolean literal, not as a symbol, so `PropertyValue`'s `kSymbol` branch
// refuses it. The predicate-position form is a different guard and is covered
// by `RefusesASymbolThatWouldReadBackAsAnOperatorOrLiteral`.
TEST(EirWriterTest, RefusesABooleanSpellingAsAPropertyValueSymbol) {
  const EvidenceCase base = Finalized(MakeValidMinimalEvidenceCase());
  for (const std::string_view name : {"true", "false"}) {
    const EvidenceCase value = Derive(base, [&name](EvidenceCase* case_) {
      case_->entities[0].properties["flag"] = Symbol(std::string(name));
    });
    EXPECT_TRUE(RequireValidEvidenceCase(value).ok()) << name;
    const StatusOr<std::string> text =
        WriteEirText(value, EirTextStyle::kCanonical);
    ASSERT_FALSE(text.ok()) << name;
    EXPECT_EQ(text.status().code(), StatusCode::kInvalidArgument) << name;
    EXPECT_TRUE(Contains(text.status().message(), "boolean literal")) << name;
  }
}

// A callee named `not`, `forall`, or `exists` is claimed by the predicate
// grammar before a call is ever read, so those three have no spelling in the
// predicate position. (`and`, `or`, and `implies` are infix and cannot start a
// call at all, which is why they are not in this set.)
TEST(EirWriterTest, RefusesAReservedWordAsAPredicatePositionCallee) {
  const EvidenceCase base = Finalized(MakeValidMinimalEvidenceCase());
  for (const std::string_view name : {"not", "forall", "exists"}) {
    const EvidenceCase value = Derive(base, [&name](EvidenceCase* case_) {
      case_->facts[0].predicate =
          Call(std::string(name), {Symbol("p")});
    });
    EXPECT_TRUE(RequireValidEvidenceCase(value).ok()) << name;
    const StatusOr<std::string> text =
        WriteEirText(value, EirTextStyle::kCanonical);
    ASSERT_FALSE(text.ok()) << name;
    EXPECT_EQ(text.status().code(), StatusCode::kInvalidArgument) << name;
    EXPECT_TRUE(Contains(text.status().message(), "claimed by the predicate"))
        << name;
  }
}

// `Domain ::= Identifier "(" [ ArgumentList ] ")" | Reference`: a quantifier's
// domain is a call or a reference, and nothing else. The validator checks only
// the arity and the bound variable, so a symbol domain reaches the writer.
TEST(EirWriterTest, RefusesAQuantifierDomainThatIsNeitherCallNorReference) {
  const EvidenceCase base = Finalized(MakeValidMinimalEvidenceCase());
  const EvidenceCase value = Derive(base, [](EvidenceCase* case_) {
    Expression quantified;
    quantified.kind = Expression::Kind::kForAll;
    quantified.text = "i";
    quantified.operands = {Symbol("length"), Symbol("p")};
    case_->facts[0].predicate = quantified;
  });
  EXPECT_TRUE(RequireValidEvidenceCase(value).ok());
  const StatusOr<std::string> text =
      WriteEirText(value, EirTextStyle::kCanonical);
  ASSERT_FALSE(text.ok());
  EXPECT_EQ(text.status().code(), StatusCode::kInvalidArgument);
  EXPECT_TRUE(Contains(text.status().message(), "quantifier domain"))
      << text.status().message();
}

// `Budget ::= IntegerLiteral | FunctionCall`, and an absent budget is
// `kUnspecified`. A string, symbol, or reference budget has no spelling: §12.1
// admits neither the optional's absence nor a value of another kind, and no
// validator constrains the field.
TEST(EirWriterTest, RefusesAProofBudgetThatIsNeitherIntegerNorCall) {
  const EvidenceCase base = Finalized(MakeValidMinimalEvidenceCase());
  ASSERT_FALSE(base.facts.empty());
  const EvidenceCase value = Derive(base, [](EvidenceCase* case_) {
    ProofObligation obligation;
    obligation.id = "V1";
    obligation.goal_kind = ProofGoalKind::kProve;
    obligation.predicate = Call("holds", {Symbol("p")});
    obligation.status = ProofStatus::kPending;
    obligation.budget = String("5000");
    case_->proof_obligations.push_back(std::move(obligation));
  });
  EXPECT_TRUE(RequireValidEvidenceCase(value).ok());
  const StatusOr<std::string> text =
      WriteEirText(value, EirTextStyle::kCanonical);
  ASSERT_FALSE(text.ok());
  EXPECT_EQ(text.status().code(), StatusCode::kInvalidArgument);
  EXPECT_TRUE(Contains(text.status().message(), "proof budget"))
      << text.status().message();
}

}  // namespace
}  // namespace veritas::evidence
