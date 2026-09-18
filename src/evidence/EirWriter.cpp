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

// EirWriter.cpp — canonical and pretty EIR-T output. See
// `veritas/evidence/EirText.h` for the public contract: the two styles, the
// emission order, and the full list of refusals.
//
// This is the inverse of `EirParser.cpp`, and it is written against that file
// rather than against the grammar alone, because four facts the parser settles
// are invisible in the published EBNF and each of them breaks `REP-001`
// silently — both passes of a round trip agree on the same wrong bytes:
//
//   1. `ResolutionAction` (`Unknown::suggested_resolution`),
//      `AssumptionSource` (`Assumption::source`), and `Scope`
//      (`Assumption::scope`, `Constraint::scope`) have plain-string model
//      carriers and lower through `ParseCallOrQualifiedId` + `RenderValue` to
//      `callee(arg, ...)`, with a reference argument written **bare** and a
//      string argument quoted. The writer must reproduce that spelling
//      byte-identically, which is why `IsCanonicalCallSpelling` below is a
//      scanner over the exact shape `RenderValue` produces rather than a
//      general "does this look like a call" test.
//   2. `ProgramBinding::analyzer_versions` is an unordered list that the parser
//      keeps in source order, so the writer sorts it; otherwise two source
//      orderings of one case would be written as two different texts.
//   3. The parser admits four value shapes the EBNF does not write (spec §5.1):
//      a bare identifier as a `kSymbol` in both the predicate and the
//      property-value positions, and a `StringLiteral` or `IntegerLiteral` at
//      the primary level. Re-serialisation must stay inside that set. §5.1 also
//      records that `QualifiedId`, `Scope`, and `Producer` are deliberately
//      **not** widened the same way, so those three stay closed here — a scope
//      is one of six keywords or a call, a producer is an identifier, and an
//      assumption source is an identifier or a call.
//   4. An entity may declare a `stable_id` identity *and* carry a property
//      named `stable_id` (§4.1). The identity occupies the body's first
//      position and the property is an ordinary bag entry after it, so the
//      identity is emitted first and every bag entry follows.
//
// THE REFUSAL OBLIGATION
//
// The mirror of the parser's `"unsupported in EIR V0.1"` rejection is this
// side's: a model-holdable value must never be silently dropped. `REP-001`
// cannot detect a dropped value — it is absent from both passes, so the round
// trip stays green — which is why every value with no spelling is a typed
// error here rather than an omission. Spec §4.1 records the three that are
// exactly unwritable (an out-of-set scope, an unspellable producer, and a
// `stable_id` bag entry on an entity with no identity); the rest are the same
// obligation applied to the expression grammar and the identifier alphabet.

#include "veritas/evidence/EirText.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/evidence/EvidenceCanonicalizer.h"
#include "veritas/evidence/EvidenceCase.h"
#include "veritas/evidence/EvidenceValidator.h"

namespace veritas::evidence {
namespace {

// --- Small string helpers ---------------------------------------------------

std::string Joined(std::string_view left, std::string_view right) {
  std::string out;
  out.reserve(left.size() + right.size());
  out.append(left);
  out.append(right);
  return out;
}

std::string Quoted(std::string_view text) {
  std::string out = "'";
  out.append(text);
  out += '\'';
  return out;
}

// --- The lexical alphabet ---------------------------------------------------

bool IsLetter(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

// `Identifier ::= Letter { Letter | Digit | "_" }`, extended by the qualified
// tail `QualifiedId ::= Identifier { "." Identifier }`. This mirrors
// `EirLexer::LexIdentifier` exactly, including its dot rule: a dot continues
// the identifier only when a `Letter` follows it immediately, so `a.b` is one
// identifier while `a.1` and `a.` are `a` followed by a stray dot. A string
// this returns false for can never be read back as the identifier it names.
bool IsIdentifier(std::string_view text) {
  if (text.empty() || !IsLetter(text[0])) {
    return false;
  }
  std::size_t index = 1;
  const auto consume_segment = [&text, &index]() {
    while (index < text.size() &&
           (IsLetter(text[index]) || IsDigit(text[index]) ||
            text[index] == '_')) {
      ++index;
    }
  };
  consume_segment();
  while (index < text.size() && text[index] == '.') {
    if (index + 1 >= text.size() || !IsLetter(text[index + 1])) {
      // A dot that continues nothing ends the identifier, which the caller
      // then sees as a trailing character this predicate rejects.
      break;
    }
    ++index;
    consume_segment();
  }
  return index == text.size();
}

// `true` and `false` lex as identifiers but lower to `BooleanLiteral` in both
// the predicate and the property-value positions, so a `kSymbol` spelled with
// one of them would be read back as a boolean. Neither name is reachable as a
// `kSymbol` from the parser; refusing them keeps a hand-built model from
// writing text that reads back as a different value.
bool IsBooleanSpelling(std::string_view text) {
  return text == "true" || text == "false";
}

// The names `ParsePredicate` (`forall`, `exists`), `ParseUnary` (`not`),
// `ParseAnd` (`and`), `ParseOr` (`or`), and `ParseImplication` (`implies`)
// consume before `ParsePrimary` is reached. A `kSymbol` carrying one of these
// names in a predicate position would be read back as an operator rather than
// as a symbol, so the writer refuses instead of emitting it. None of them is
// reachable as a `kSymbol` from the parser, so no case the reader produces is
// refused here.
bool IsReservedPredicateName(std::string_view text) {
  return IsBooleanSpelling(text) || text == "not" || text == "and" ||
         text == "or" || text == "implies" || text == "forall" ||
         text == "exists";
}

// The names that are claimed before a callee is read. `ParsePredicate` matches
// `forall`/`exists` and `ParseUnary` matches `not` first, so `not(x)` parses as
// a negation of a parenthesised predicate and `forall(x)` as a quantifier, not
// as a call. `and`, `or`, and `implies` are matched only *between* operands, so
// a call named after one of them is unambiguous and is not refused.
bool IsReservedCalleeName(std::string_view text) {
  return text == "not" || text == "forall" || text == "exists";
}

// `Scope ::= "global" | "function" | "path" | "basic_block" | "callsite" |
// "entity" | FunctionCall` (spec §4.1, §10.1). These six keywords are the
// production's whole identifier-shaped half; the parser refuses every other
// bare identifier and so does the writer. The spelling of each is pinned by a
// test, so a drift on either side is caught.
bool IsScopeKeyword(std::string_view text) {
  return text == "global" || text == "function" || text == "path" ||
         text == "basic_block" || text == "callsite" || text == "entity";
}

bool ContainsLineBreak(std::string_view text) {
  return text.find('\n') != std::string_view::npos ||
         text.find('\r') != std::string_view::npos;
}

// --- The canonical call spelling --------------------------------------------

// A scanner over one *canonical* call spelling — the exact shape
// `RenderValue` (EirParser.cpp) produces. It is a fixpoint test, not a
// look-alike test: a string this accepts is one the parser lowers to a call
// that renders back to the same string, and a string it rejects is one the
// writer must not emit.
//
// The shapes it mirrors, one for one:
//
//   * `callee(arg, ...)` — the callee is an identifier immediately followed by
//     `(`, and the arguments are separated by exactly `", "`;
//   * a string argument is quoted with **only** `\"` and `\\` escaped, because
//     `AppendEscaped` escapes exactly those two and nothing else. `\n`, `\r`,
//     and `\t` as escape *sequences* are therefore not canonical here, and a
//     raw newline inside a string argument is rejected outright — see the
//     refusal in `ReadCallCarrier`;
//   * an integer argument is in `std::to_string`'s spelling. The check is
//     literally "does `to_string` reproduce this from the value it selects",
//     which rejects `007`, `-0`, and anything outside `std::int64_t` in one
//     test rather than three;
//   * a bare identifier argument is a reference or a symbol, written bare;
//   * a boolean argument is `true` or `false`, and a nested call is scanned
//     recursively.
class CanonicalCallScanner {
 public:
  explicit CanonicalCallScanner(std::string_view text) : text_(text) {}

  // True when the whole of `text_` is one canonical call spelling.
  bool Scan() { return ScanCall() && AtEnd(); }

 private:
  bool AtEnd() const { return index_ >= text_.size(); }

  bool Eat(char expected) {
    if (AtEnd() || text_[index_] != expected) {
      return false;
    }
    ++index_;
    return true;
  }

  // Consumes one identifier, choosing the longest prefix `IsIdentifier`
  // accepts. That is the same answer the lexer's maximal scan gives: for
  // `a.b.` it stops at `a.b`, and for `a.1` at `a`.
  bool ScanIdentifier() {
    std::size_t length = 0;
    while (index_ + length < text_.size()) {
      const char c = text_[index_ + length];
      if (!IsLetter(c) && !IsDigit(c) && c != '_' && c != '.') {
        break;
      }
      ++length;
    }
    for (std::size_t candidate = length; candidate > 0; --candidate) {
      if (IsIdentifier(text_.substr(index_, candidate))) {
        index_ += candidate;
        return true;
      }
    }
    return false;
  }

  bool ScanCall() {
    if (!ScanIdentifier()) {
      return false;
    }
    if (!Eat('(')) {
      return false;
    }
    if (Eat(')')) {
      return true;
    }
    for (;;) {
      if (!ScanValue()) {
        return false;
      }
      if (Eat(')')) {
        return true;
      }
      if (!Eat(',')) {
        return false;
      }
      if (!Eat(' ')) {
        return false;
      }
    }
  }

  bool ScanValue() {
    if (AtEnd()) {
      return false;
    }
    if (text_[index_] == '"') {
      return ScanString();
    }
    if (text_[index_] == '-' || IsDigit(text_[index_])) {
      return ScanInteger();
    }
    if (!IsLetter(text_[index_])) {
      return false;
    }
    const std::size_t start = index_;
    if (!ScanIdentifier()) {
      return false;
    }
    // `true(` and `false(` are calls, not booleans: `ParsePropertyValue` tests
    // for `"("` before it tests for the literal. Rewind so the nested call is
    // scanned as one.
    if (!AtEnd() && text_[index_] == '(') {
      index_ = start;
      return ScanCall();
    }
    return true;
  }

  bool ScanString() {
    if (!Eat('"')) {
      return false;
    }
    for (;;) {
      if (AtEnd()) {
        return false;
      }
      const char c = text_[index_];
      if (c == '"') {
        ++index_;
        return true;
      }
      if (c == '\n' || c == '\r') {
        return false;
      }
      if (c == '\\') {
        ++index_;
        if (AtEnd()) {
          return false;
        }
        const char escaped = text_[index_];
        if (escaped != '"' && escaped != '\\') {
          return false;
        }
        ++index_;
        continue;
      }
      ++index_;
    }
  }

  bool ScanInteger() {
    const std::size_t start = index_;
    const bool negative = !AtEnd() && text_[index_] == '-';
    if (negative) {
      ++index_;
    }
    const std::size_t digits = index_;
    while (!AtEnd() && IsDigit(text_[index_])) {
      ++index_;
    }
    if (index_ == digits) {
      return false;
    }
    const std::string_view spelling = text_.substr(start, index_ - start);
    std::int64_t value = 0;
    if (!ParseInteger(spelling, &value)) {
      return false;
    }
    return std::to_string(value) == spelling;
  }

  // `std::int64_t` range checked by hand: a spelling outside it has no
  // `IntegerLiteral` the reader could lower. The magnitude is accumulated in
  // `std::uint64_t` so the one value whose negation overflows is not itself
  // undefined.
  static bool ParseInteger(std::string_view spelling, std::int64_t* out) {
    const bool negative = !spelling.empty() && spelling[0] == '-';
    const std::string_view digits = negative ? spelling.substr(1) : spelling;
    if (digits.empty()) {
      return false;
    }
    const auto limit = negative
                           ? static_cast<std::uint64_t>(
                                 std::numeric_limits<std::int64_t>::max()) + 1
                           : static_cast<std::uint64_t>(
                                 std::numeric_limits<std::int64_t>::max());
    std::uint64_t magnitude = 0;
    for (const char c : digits) {
      const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
      if (magnitude > (limit - digit) / 10) {
        return false;
      }
      magnitude = magnitude * 10 + digit;
    }
    if (negative) {
      *out = magnitude == limit
                 ? std::numeric_limits<std::int64_t>::min()
                 : -static_cast<std::int64_t>(magnitude);
    } else {
      *out = static_cast<std::int64_t>(magnitude);
    }
    return true;
  }

  std::string_view text_;
  std::size_t index_ = 0;
};

bool IsCanonicalCallSpelling(std::string_view text) {
  return CanonicalCallScanner(text).Scan();
}

// --- Canonical collection order ---------------------------------------------

// The canonicalizer's ordering key (`EvidenceCanonicalizer.cpp`): a record's
// declared kind family spelled as its payload spells it, then its stable ID,
// then its case-local handle. Every member carries a unique local handle (the
// validator enforces one flat identifier space), so the key is total and the
// canonicalizer's final canonical-encoding tie-break is unreachable here.
struct SortKey {
  std::string kind;
  std::string stable_id;
  std::string id;
};

bool KeyLess(const SortKey& left, const SortKey& right) {
  if (left.kind != right.kind) {
    return left.kind < right.kind;
  }
  if (left.stable_id != right.stable_id) {
    return left.stable_id < right.stable_id;
  }
  return left.id < right.id;
}

std::string StableIdComponent(const std::optional<core::StableId>& stable_id) {
  return stable_id.has_value() ? core::ToString(*stable_id) : std::string();
}

std::vector<std::size_t> CanonicalOrder(const std::vector<SortKey>& keys) {
  std::vector<std::size_t> order(keys.size());
  for (std::size_t index = 0; index < order.size(); ++index) {
    order[index] = index;
  }
  std::stable_sort(order.begin(), order.end(),
                   [&keys](std::size_t left, std::size_t right) {
                     return KeyLess(keys[left], keys[right]);
                   });
  return order;
}

// --- The writer -------------------------------------------------------------

class EirWriter {
 public:
  EirWriter(EirTextStyle style, std::string* out) : out_(out), style_(style) {}

  Status Run(const EvidenceCase& value);

 private:
  // --- Failure --------------------------------------------------------------

  bool ok() const { return failed_.ok(); }

  void Refuse(std::string message) {
    if (failed_.ok()) {
      failed_ = Status::InvalidArgument(std::move(message));
    }
  }

  // --- Layout ---------------------------------------------------------------

  void Raw(std::string_view text) {
    if (ok()) {
      out_->append(text);
    }
  }

  // `depth` levels of four spaces.
  void Indent(int depth) {
    if (!ok()) {
      return;
    }
    for (int level = 0; level < depth; ++level) {
      out_->append("    ");
    }
  }

  // One blank line in pretty mode, nothing in canonical mode. Called before
  // each top-level item after the first, so the two modes differ in line
  // breaks alone.
  void Gap() {
    if (ok() && style_ == EirTextStyle::kPretty) {
      out_->push_back('\n');
    }
  }

  // --- Primitives -----------------------------------------------------------

  // One `"..."` literal. The five escapes below are exactly the ones the lexer
  // decodes (`EirLexer::LexString`), so escaping them is lossless and no other
  // byte needs one. A raw newline is never emitted inside a literal.
  void StringLiteral(std::string_view text) {
    if (!ok()) {
      return;
    }
    std::string literal;
    literal.push_back('"');
    for (const char c : text) {
      switch (c) {
        case '"':
          literal += "\\\"";
          break;
        case '\\':
          literal += "\\\\";
          break;
        case '\n':
          literal += "\\n";
          break;
        case '\r':
          literal += "\\r";
          break;
        case '\t':
          literal += "\\t";
          break;
        default:
          literal.push_back(c);
          break;
      }
    }
    literal.push_back('"');
    out_->append(literal);
  }

  // A bare identifier, refused when it is not one. Every case-local handle,
  // every producer, the omission kind, the quantifier variable, and the
  // `kSymbol` payload pass through here.
  void Identifier(std::string_view what, std::string_view text) {
    if (!ok()) {
      return;
    }
    if (!IsIdentifier(text)) {
      Refuse(Joined("the ", what) +
             " is not an EIR-T identifier (letters, digits, and underscores, "
             "with an interior dot joining two segments), so it has no "
             "spelling: " +
             Quoted(text));
      return;
    }
    out_->append(text);
  }

  // A `@`-introduced case-local reference. The `@` belongs to the syntax and
  // never to the model value, so it is added here and never carried.
  void Reference(std::string_view what, std::string_view text) {
    if (!ok()) {
      return;
    }
    if (!IsIdentifier(text)) {
      Refuse(Joined("the ", what) +
             " is not an EIR-T identifier, so it has no reference spelling: " +
             Quoted(text));
      return;
    }
    out_->push_back('@');
    out_->append(text);
  }

  // One textual enum terminal. The invalid `kUnspecified` default is refused
  // rather than emitted: its `ToString` rendering is "unspecified", which no
  // `Parse*` helper accepts, so emitting it would produce text the reader
  // refuses.
  template <typename T>
  void EnumTerminal(std::string_view what, T value) {
    if (!ok()) {
      return;
    }
    if (value == T::kUnspecified) {
      Refuse(Joined("the ", what) +
             " carries the invalid default, which has no EIR-T spelling");
      return;
    }
    out_->append(ToString(value));
  }

  // `AssumptionSource ::= FunctionCall | QualifiedId` and
  // `ResolutionAction ::= FunctionCall`: carriers the model holds as plain
  // strings, lowered by the parser to the call's canonical spelling.
  void CallCarrier(std::string_view what, std::string_view text,
                   bool allow_bare_qualified_id) {
    if (!ok()) {
      return;
    }
    // A raw line break cannot be emitted: the carrier is written verbatim,
    // because escaping it would change the string the reader renders back, and
    // a raw newline inside a string literal is exactly what this writer must
    // never produce. The value is refused instead of emitted wrongly.
    if (ContainsLineBreak(text)) {
      Refuse(Joined("the ", what) +
             " carries a raw line break, which cannot be emitted without "
             "changing the value it renders back to");
      return;
    }
    if (allow_bare_qualified_id && IsIdentifier(text) &&
        !IsBooleanSpelling(text)) {
      out_->append(text);
      return;
    }
    if (IsCanonicalCallSpelling(text)) {
      out_->append(text);
      return;
    }
    Refuse(Joined("the ", what) + " " + Quoted(text) +
           (allow_bare_qualified_id
                ? " is neither a qualified identifier nor a canonical "
                  "function call, so it has no EIR-T spelling"
                : " is not a canonical function call, so it has no EIR-T "
                  "spelling"));
  }

  // `Scope`. The six keywords plus a call, and nothing else: §4.1 records that
  // a scope outside them — `everywhere`, say — has no spelling at all, that the
  // production is deliberately not widened, and that a writer handed one could
  // not emit text its own parser would read back.
  void ScopeCarrier(std::string_view what, std::string_view text) {
    if (!ok()) {
      return;
    }
    if (ContainsLineBreak(text)) {
      Refuse(Joined("the ", what) +
             " carries a raw line break, which cannot be emitted without "
             "changing the value it renders back to");
      return;
    }
    if (IsScopeKeyword(text) || IsCanonicalCallSpelling(text)) {
      out_->append(text);
      return;
    }
    Refuse(Joined("the ", what) + " " + Quoted(text) +
           " is neither one of the six scope keywords (global, function, "
           "path, basic_block, callsite, entity) nor a canonical function "
           "call, so it has no EIR-T spelling");
  }

  // A `Producer` carrier — `QualifiedId`, and deliberately not widened, so the
  // identifier alphabet is the whole of what it can spell.
  void ProducerCarrier(std::string_view what, std::string_view text) {
    if (!ok()) {
      return;
    }
    if (text.empty()) {
      Refuse(Joined("the ", what) +
             " is empty and a producer has no empty spelling: the alphabet is "
             "letters, digits, underscores, and interior dots");
      return;
    }
    Identifier(what, text);
  }

  // --- Expressions ----------------------------------------------------------

  static int BindingPower(Expression::Kind kind) {
    switch (kind) {
      case Expression::Kind::kBool:
      case Expression::Kind::kInteger:
      case Expression::Kind::kString:
      case Expression::Kind::kSymbol:
      case Expression::Kind::kReference:
      case Expression::Kind::kCall:
        return 60;
      case Expression::Kind::kNot:
        return 50;
      case Expression::Kind::kCompare:
        return 40;
      case Expression::Kind::kAnd:
        return 30;
      case Expression::Kind::kOr:
        return 20;
      case Expression::Kind::kImplies:
        return 10;
      case Expression::Kind::kForAll:
      case Expression::Kind::kExists:
        return 0;
      default:
        break;
    }
    return -1;
  }

  const Expression* Operand(const Expression& value, std::size_t index) {
    if (index < value.operands.size()) {
      return &value.operands[index];
    }
    Refuse("a predicate carries fewer operands than its kind requires, so it "
           "has no spelling");
    return nullptr;
  }

  // `Predicate` in the grammar's predicate position, parenthesised whenever the
  // node binds more loosely than the context admits. `min_bp` is the binding
  // power the enclosing production parses at, taken from the chain the parser
  // implements (`ParsePredicate`, `ParseImplication`, `ParseOr`, `ParseAnd`,
  // `ParseComparison`, `ParseUnary`, `ParsePrimary`): an operand of `and` is
  // read by `ParseComparison` and so needs 40, an operand of `or` is read by
  // `ParseAnd` and so needs 30, an operand of a comparison or of `not` is read
  // by `ParseUnary` and so needs 50, and an implication's antecedent is read by
  // `ParseOr` while its consequent is read by `ParseImplication` itself.
  void Predicate(const Expression& value, int min_bp) {
    if (!ok()) {
      return;
    }
    const int bp = BindingPower(value.kind);
    if (bp < 0) {
      Refuse("an expression carries a kind with no EIR-T spelling");
      return;
    }
    const bool parenthesise = bp < min_bp;
    if (parenthesise) {
      Raw("(");
    }
    switch (value.kind) {
      case Expression::Kind::kBool:
        Raw(value.boolean ? "true" : "false");
        break;
      case Expression::Kind::kInteger:
        Raw(std::to_string(value.integer));
        break;
      case Expression::Kind::kString:
        StringLiteral(value.text);
        break;
      case Expression::Kind::kSymbol:
        if (IsReservedPredicateName(value.text)) {
          Refuse(Joined("the symbol ", Quoted(value.text)) +
                 " is read as an operator or a literal in a predicate "
                 "position, so it has no spelling there");
          break;
        }
        Identifier("symbol", value.text);
        break;
      case Expression::Kind::kReference:
        Reference("expression reference", value.text);
        break;
      case Expression::Kind::kCall:
        Call(value, /*predicate_position=*/true);
        break;
      case Expression::Kind::kNot: {
        const Expression* const operand = Operand(value, 0);
        if (operand == nullptr) {
          break;
        }
        Raw("not ");
        Predicate(*operand, 50);
        break;
      }
      case Expression::Kind::kCompare: {
        const Expression* const left = Operand(value, 0);
        const Expression* const right = Operand(value, 1);
        if (left == nullptr || right == nullptr) {
          break;
        }
        // Both sides are read by `ParseUnary`, so a nested comparison is
        // parenthesised: `kCompare` is non-associative and the parser refuses a
        // chain rather than grouping one.
        Predicate(*left, 50);
        Raw(" ");
        Raw(value.text);
        Raw(" ");
        Predicate(*right, 50);
        break;
      }
      case Expression::Kind::kAnd:
      case Expression::Kind::kOr: {
        const bool conjunction = value.kind == Expression::Kind::kAnd;
        const std::string_view separator = conjunction ? " and " : " or ";
        // The model's connective is n-ary and flat and the parser splices a
        // parenthesised chain into one list, so the chain is written flattened
        // and never re-parenthesised.
        const int operand_bp = conjunction ? 40 : 30;
        if (value.operands.size() < 2) {
          Refuse("a connective carries fewer than two operands, so it has no "
                 "spelling");
          break;
        }
        for (std::size_t index = 0; index < value.operands.size(); ++index) {
          if (index != 0) {
            Raw(separator);
          }
          Predicate(value.operands[index], operand_bp);
        }
        break;
      }
      case Expression::Kind::kImplies: {
        const Expression* const antecedent = Operand(value, 0);
        const Expression* const consequent = Operand(value, 1);
        if (antecedent == nullptr || consequent == nullptr) {
          break;
        }
        Predicate(*antecedent, 20);
        Raw(" implies ");
        Predicate(*consequent, 10);
        break;
      }
      case Expression::Kind::kForAll:
      case Expression::Kind::kExists: {
        const Expression* const domain = Operand(value, 0);
        const Expression* const body = Operand(value, 1);
        if (domain == nullptr || body == nullptr) {
          break;
        }
        Raw(value.kind == Expression::Kind::kForAll ? "forall " : "exists ");
        Identifier("bound variable", value.text);
        Raw(" in ");
        // `Domain ::= Identifier "(" [ ArgumentList ] ")" | Reference`, and
        // `ArgumentList` is a list of `PropertyValue`, so a domain call's
        // arguments are written in the value position, not the predicate one.
        if (domain->kind == Expression::Kind::kReference) {
          Reference("quantifier domain", domain->text);
        } else if (domain->kind == Expression::Kind::kCall) {
          Call(*domain, /*predicate_position=*/false);
        } else {
          Refuse("a quantifier domain is neither a reference nor a function "
                 "call, so it has no EIR-T spelling");
        }
        Raw(": ");
        // A quantifier is the loosest level and owns everything after its
        // colon, so its body is written at the top of the chain.
        Predicate(*body, 0);
        break;
      }
      default:
        Refuse("an expression carries a kind with no EIR-T spelling");
        break;
    }
    if (parenthesise) {
      Raw(")");
    }
  }

  // A `PropertyValue`. This position admits `StringLiteral`, `IntegerLiteral`,
  // `BooleanLiteral`, `Reference`, `FunctionCall`, and — per §5.1's first
  // widening — a bare identifier. It admits no parenthesised sub-expression at
  // all, so nothing here is ever parenthesised: a compound value has no
  // spelling in this position and is refused.
  void Value(const Expression& value) {
    if (!ok()) {
      return;
    }
    switch (value.kind) {
      case Expression::Kind::kString:
        StringLiteral(value.text);
        break;
      case Expression::Kind::kInteger:
        Raw(std::to_string(value.integer));
        break;
      case Expression::Kind::kBool:
        Raw(value.boolean ? "true" : "false");
        break;
      case Expression::Kind::kReference:
        Reference("property reference", value.text);
        break;
      case Expression::Kind::kSymbol:
        if (IsBooleanSpelling(value.text)) {
          Refuse(Joined("the symbol ", Quoted(value.text)) +
                 " is read as a boolean literal, so it has no spelling as a "
                 "symbol");
          break;
        }
        Identifier("symbol", value.text);
        break;
      case Expression::Kind::kCall:
        Call(value, /*predicate_position=*/false);
        break;
      default:
        Refuse("a property value carries a compound expression, which this "
               "position has no spelling for");
        break;
    }
  }

  // `FunctionCall ::= Identifier "(" [ ArgumentList ] ")"`. `predicate_position`
  // selects the argument grammar: a call read by `ParsePredicateArgumentList`
  // takes full predicates, while one read by `ParsePropertyValueList` or
  // `ParseResourceBudget` takes property values only.
  void Call(const Expression& value, bool predicate_position) {
    if (!ok()) {
      return;
    }
    if (predicate_position && IsReservedCalleeName(value.text)) {
      Refuse(Joined("the callee ", Quoted(value.text)) +
             " is claimed by the predicate grammar before a call is read, so "
             "the call has no spelling in a predicate position");
      return;
    }
    Identifier("callee", value.text);
    Raw("(");
    for (std::size_t index = 0; index < value.operands.size(); ++index) {
      if (index != 0) {
        Raw(", ");
      }
      if (predicate_position) {
        Predicate(value.operands[index], 0);
      } else {
        Value(value.operands[index]);
      }
    }
    Raw(")");
  }

  // --- Declarations ---------------------------------------------------------

  void WriteContext(const ProgramBinding& program);
  void WriteEntity(const Entity& entity);
  void WriteClaim(const Claim& claim);
  void WriteFact(const Fact& fact);
  void WriteAssumption(const Assumption& assumption);
  void WriteHypothesis(const Hypothesis& hypothesis);
  void WriteUnknown(const Unknown& unknown);
  void WriteEdge(const Edge& edge);
  void WritePath(const Path& path);
  void WriteConstraint(const Constraint& constraint);
  void WriteProvenance(const Provenance& record);
  void WriteVerification(const ProofObligation& obligation);
  void WriteSummary(const SummaryReference& summary);
  void WriteDependency(const Dependency& dependency);
  void WriteOmission(const Omission& omission);

  // One `name = value;` line at `depth`, for an attribute written as a bare
  // identifier.
  void AttributeLine(int depth, std::string_view name,
                     std::string_view value) {
    if (!ok()) {
      return;
    }
    Indent(depth);
    Raw(name);
    Raw(" = ");
    Raw(value);
    Raw(";\n");
  }

  // One `name = "value";` line at `depth`.
  void StringAttributeLine(int depth, std::string_view name,
                           std::string_view value) {
    if (!ok()) {
      return;
    }
    Indent(depth);
    Raw(name);
    Raw(" = ");
    StringLiteral(value);
    Raw(";\n");
  }

  // One enum terminal and its `;`.
  template <typename T>
  void EnumAttributeLine(int depth, std::string_view name,
                         std::string_view what, T value) {
    if (!ok()) {
      return;
    }
    Indent(depth);
    Raw(name);
    Raw(" = ");
    EnumTerminal(what, value);
    Raw(";\n");
  }

  // One `name = @reference;` line at `depth`.
  void ReferenceAttributeLine(int depth, std::string_view name,
                              std::string_view reference) {
    if (!ok()) {
      return;
    }
    Indent(depth);
    Raw(name);
    Raw(" = ");
    Reference(name, reference);
    Raw(";\n");
  }

  // A `[a, b]` identifier list, sorted, on one line at `depth`.
  void IdentifierListLine(int depth, std::string_view name,
                          std::vector<std::string> values) {
    if (!ok()) {
      return;
    }
    std::sort(values.begin(), values.end());
    Indent(depth);
    Raw(name);
    Raw(" = [");
    for (std::size_t index = 0; index < values.size(); ++index) {
      if (index != 0) {
        Raw(", ");
      }
      Identifier(name, values[index]);
    }
    Raw("];\n");
  }

  // A `[$a, $b]` fact-reference list, sorted, on one line at `depth`.
  void FactReferenceListLine(int depth, std::string_view name,
                             std::vector<std::string> values) {
    if (!ok()) {
      return;
    }
    std::sort(values.begin(), values.end());
    Indent(depth);
    Raw(name);
    Raw(" = [");
    for (std::size_t index = 0; index < values.size(); ++index) {
      if (index != 0) {
        Raw(", ");
      }
      Raw("$");
      Identifier(name, values[index]);
    }
    Raw("];\n");
  }

  // A `[@a, @b]` reference list, sorted, on one line at `depth`.
  void ReferenceListLine(int depth, std::string_view name,
                         std::vector<std::string> values) {
    if (!ok()) {
      return;
    }
    std::sort(values.begin(), values.end());
    Indent(depth);
    Raw(name);
    Raw(" = [");
    for (std::size_t index = 0; index < values.size(); ++index) {
      if (index != 0) {
        Raw(", ");
      }
      Reference(name, values[index]);
    }
    Raw("];\n");
  }

  // A `name = <predicate>;` line at `depth`.
  void PredicateLine(int depth, std::string_view name,
                     const Expression& predicate) {
    if (!ok()) {
      return;
    }
    Indent(depth);
    Raw(name);
    Raw(" = ");
    Predicate(predicate, 0);
    Raw(";\n");
  }

  // A `name = <property value>;` line at `depth`.
  void ValueLine(int depth, std::string_view name, const Expression& value) {
    if (!ok()) {
      return;
    }
    Indent(depth);
    Raw(name);
    Raw(" = ");
    Value(value);
    Raw(";\n");
  }

  std::string* out_;
  EirTextStyle style_;
  Status failed_;
};

void EirWriter::WriteContext(const ProgramBinding& program) {
  Indent(1);
  Raw("context {\n");

  StringAttributeLine(2, "repository", program.repository_id);
  StringAttributeLine(2, "revision", program.revision_id);
  StringAttributeLine(2, "build_variant", program.build_variant_id);
  StringAttributeLine(2, "target", program.target_triple);
  StringAttributeLine(2, "analyzer_configuration",
                      program.analysis_configuration_id);
  StringAttributeLine(2, "type_layout", program.type_layout_id);

  if (!program.analysis_run_id.has_value()) {
    // The validator requires the run, so this is unreachable through
    // `WriteEirText`; it is refused rather than skipped so the emission has no
    // path that silently omits a hashed field.
    Refuse("the program binding carries no analysis_run, which has no spelling "
           "when it is absent");
  } else {
    StringAttributeLine(2, "analysis_run",
                        core::ToString(*program.analysis_run_id));
  }

  // §19.1 requires the analyzer-version list to be sorted. The parser keeps
  // source order, so without this sort two source orderings of one case would
  // be written as two different texts. The key is the whole record, which is
  // total and does not depend on the order the versions arrived in.
  std::vector<AnalyzerVersion> versions = program.analyzer_versions;
  std::sort(versions.begin(), versions.end(),
            [](const AnalyzerVersion& left, const AnalyzerVersion& right) {
              return left < right;
            });
  for (const AnalyzerVersion& version : versions) {
    if (!ok()) {
      return;
    }
    Indent(2);
    Raw("analyzer = ");
    ProducerCarrier("analyzer producer", version.producer);
    Raw("(");
    // `AnalyzerVersion ::= Producer "(" [ StringLiteral [ "," StringLiteral ]
    // ")"`, so the configuration can only be written when the version precedes
    // it. An empty version with a non-empty configuration is written as `""`.
    const bool have_configuration = !version.configuration.empty();
    if (!version.version.empty() || have_configuration) {
      StringLiteral(version.version);
    }
    if (have_configuration) {
      Raw(", ");
      StringLiteral(version.configuration);
    }
    Raw(");\n");
  }

  Indent(1);
  Raw("}\n");
}

void EirWriter::WriteEntity(const Entity& entity) {
  Indent(1);
  Raw("entity ");
  Identifier("entity identifier", entity.id);
  Raw(" : ");
  EnumTerminal("entity kind", entity.kind);
  Raw(" {\n");

  // The identity is positional: it is the body's first attribute and only that
  // position binds it, so it is emitted before any property — including one
  // named `stable_id`, which §4.1 admits as an ordinary bag entry beside it.
  if (entity.stable_id.has_value()) {
    StringAttributeLine(2, "stable_id", core::ToString(*entity.stable_id));
  } else if (entity.properties.find("stable_id") != entity.properties.end()) {
    // §4.1 records this as unwritable: with no leading identity the first bag
    // entry of that name is read as the identity, so the property's value
    // would come back as an identity rather than as itself.
    Refuse("an entity declares a property named 'stable_id' but no identity, "
           "so the entry would be read back as the identity: the value has no "
           "EIR-T spelling");
  }

  for (const auto& property : entity.properties) {
    if (!ok()) {
      return;
    }
    ValueLine(2, property.first, property.second);
  }

  Indent(1);
  Raw("}\n");
}

void EirWriter::WriteClaim(const Claim& claim) {
  Indent(1);
  Raw("claim ");
  Identifier("claim identifier", claim.id);
  Raw(" {\n");
  EnumAttributeLine(2, "kind", "claim kind", claim.kind);
  ReferenceAttributeLine(2, "subject", claim.subject);
  PredicateLine(2, "predicate", claim.predicate);
  EnumAttributeLine(2, "severity", "severity", claim.severity);
  if (!claim.description.empty()) {
    StringAttributeLine(2, "description", claim.description);
  }
  Indent(1);
  Raw("}\n");
}

void EirWriter::WriteFact(const Fact& fact) {
  Indent(1);
  Raw("fact ");
  Identifier("fact identifier", fact.id);
  Raw(" {\n");
  if (fact.stable_id.has_value()) {
    StringAttributeLine(2, "stable_id", core::ToString(*fact.stable_id));
  }
  PredicateLine(2, "predicate", fact.predicate);
  EnumAttributeLine(2, "epistemic", "epistemic state", fact.epistemic);
  if (fact.confidence != Confidence::kUnspecified) {
    EnumAttributeLine(2, "confidence", "confidence", fact.confidence);
  }
  if (!fact.producer.empty()) {
    Indent(2);
    Raw("source = ");
    ProducerCarrier("fact producer", fact.producer);
    Raw(";\n");
  }
  if (!fact.provenance_id.empty()) {
    ReferenceAttributeLine(2, "provenance", fact.provenance_id);
  }
  if (fact.derived) {
    AttributeLine(2, "derived", "true");
  }
  Indent(1);
  Raw("}\n");
}

void EirWriter::WriteAssumption(const Assumption& assumption) {
  Indent(1);
  Raw("assumption ");
  Identifier("assumption identifier", assumption.id);
  Raw(" {\n");
  PredicateLine(2, "predicate", assumption.predicate);
  // `source` is a required attribute of the production, so it is emitted
  // unconditionally and an empty one is refused by the carrier rather than
  // omitted: omitting it would produce a document the reader refuses.
  Indent(2);
  Raw("source = ");
  CallCarrier("assumption source", assumption.source,
              /*allow_bare_qualified_id=*/true);
  Raw(";\n");
  if (!assumption.scope.empty()) {
    Indent(2);
    Raw("scope = ");
    ScopeCarrier("assumption scope", assumption.scope);
    Raw(";\n");
  }
  Indent(1);
  Raw("}\n");
}

void EirWriter::WriteHypothesis(const Hypothesis& hypothesis) {
  Indent(1);
  Raw("hypothesis ");
  Identifier("hypothesis identifier", hypothesis.id);
  Raw(" {\n");
  PredicateLine(2, "predicate", hypothesis.predicate);
  Indent(2);
  Raw("producer = ");
  ProducerCarrier("hypothesis producer", hypothesis.producer);
  Raw(";\n");
  if (!hypothesis.reason.empty()) {
    StringAttributeLine(2, "reason", hypothesis.reason);
  }
  if (hypothesis.confidence != Confidence::kUnspecified) {
    EnumAttributeLine(2, "confidence", "confidence", hypothesis.confidence);
  }
  Indent(1);
  Raw("}\n");
}

void EirWriter::WriteUnknown(const Unknown& unknown) {
  Indent(1);
  Raw("unknown ");
  Identifier("unknown identifier", unknown.id);
  Raw(" {\n");
  PredicateLine(2, "property", unknown.property);
  EnumAttributeLine(2, "reason", "unknown reason", unknown.reason_code);
  if (!unknown.reason.empty()) {
    // The closed reason code above and this free-text sentence are separate
    // fields; the sentence is the detail the case observed and is never rounded
    // to the nearest terminal.
    StringAttributeLine(2, "detail", unknown.reason);
  }
  if (!unknown.blocking_ids.empty()) {
    ReferenceListLine(2, "blocking", unknown.blocking_ids);
  }
  if (!unknown.suggested_resolution.empty()) {
    Indent(2);
    Raw("suggested_resolution = ");
    CallCarrier("suggested resolution", unknown.suggested_resolution,
                /*allow_bare_qualified_id=*/false);
    Raw(";\n");
  }
  Indent(1);
  Raw("}\n");
}

void EirWriter::WriteEdge(const Edge& edge) {
  Indent(1);
  Raw("edge ");
  Identifier("edge identifier", edge.id);
  Raw(" {\n");
  ReferenceAttributeLine(2, "from", edge.from);
  ReferenceAttributeLine(2, "to", edge.to);
  EnumAttributeLine(2, "kind", "relation kind", edge.kind);
  if (edge.epistemic != EpistemicState::kUnspecified) {
    EnumAttributeLine(2, "epistemic", "epistemic state", edge.epistemic);
  }
  if (!edge.provenance_id.empty()) {
    ReferenceAttributeLine(2, "provenance", edge.provenance_id);
  }
  if (!edge.summarized_by.empty()) {
    ReferenceAttributeLine(2, "summarized_by", edge.summarized_by);
  }
  if (edge.expandable) {
    AttributeLine(2, "expandable", "true");
  }
  Indent(1);
  Raw("}\n");
}

void EirWriter::WritePath(const Path& path) {
  Indent(1);
  Raw("path ");
  Identifier("path identifier", path.id);
  Raw(" ");
  EnumTerminal("path kind", path.kind);
  Raw(" {\n");

  // `PathExpression ::= Reference { PathOp Reference } ";"`. The sequence is
  // the path and is never reordered, so it is written in model order.
  Indent(2);
  for (std::size_t index = 0; index < path.entity_ids.size(); ++index) {
    if (index != 0) {
      Raw(" -> ");
    }
    Reference("path segment", path.entity_ids[index]);
  }
  Raw(";\n");

  if (!path.conditions.empty()) {
    Indent(2);
    Raw("conditions {\n");
    for (const Expression& condition : path.conditions) {
      if (!ok()) {
        return;
      }
      Indent(3);
      Predicate(condition, 0);
      Raw(";\n");
    }
    Indent(2);
    Raw("}\n");
  }

  if (path.feasibility != Feasibility::kUnspecified) {
    EnumAttributeLine(2, "feasible", "feasibility", path.feasibility);
  }
  if (!path.provenance_id.empty()) {
    ReferenceAttributeLine(2, "provenance", path.provenance_id);
  }
  Indent(1);
  Raw("}\n");
}

void EirWriter::WriteConstraint(const Constraint& constraint) {
  Indent(1);
  Raw("constraint ");
  Identifier("constraint identifier", constraint.id);
  Raw(" {\n");
  PredicateLine(2, "expr", constraint.expression);
  if (!constraint.scope.empty()) {
    Indent(2);
    Raw("scope = ");
    ScopeCarrier("constraint scope", constraint.scope);
    Raw(";\n");
  }
  if (constraint.epistemic != EpistemicState::kUnspecified) {
    EnumAttributeLine(2, "epistemic", "epistemic state", constraint.epistemic);
  }
  if (!constraint.provenance_id.empty()) {
    ReferenceAttributeLine(2, "provenance", constraint.provenance_id);
  }
  Indent(1);
  Raw("}\n");
}

void EirWriter::WriteProvenance(const Provenance& record) {
  Indent(1);
  Raw("provenance ");
  Identifier("provenance identifier", record.id);
  Raw(" {\n");
  Indent(2);
  Raw("producer = ");
  ProducerCarrier("provenance producer", record.producer);
  Raw(";\n");
  if (!record.rule.empty()) {
    StringAttributeLine(2, "rule", record.rule);
  }
  if (!record.input_fact_ids.empty()) {
    FactReferenceListLine(2, "inputs", record.input_fact_ids);
  }
  if (!record.source_anchor_id.empty()) {
    // `source_anchor`, never `location`: the two name different things, only
    // the former exists in the model, and the parser refuses `location` with a
    // typed "unsupported in EIR V0.1" diagnostic. Lowering one onto the other
    // would change `EvidenceID`.
    StringAttributeLine(2, "source_anchor", record.source_anchor_id);
  }
  if (!record.analysis_run_id.has_value()) {
    Refuse("a provenance record carries no analysis_run, which has no spelling "
           "when it is absent");
  } else {
    // The run is written explicitly rather than derived from the case binding:
    // it is a semantic field of the record with a textual representation of its
    // own, and the equality with the case's own run is a constraint the
    // document satisfies.
    StringAttributeLine(2, "analysis_run",
                        core::ToString(*record.analysis_run_id));
  }
  if (!record.version.empty()) {
    StringAttributeLine(2, "version", record.version);
  }
  if (!record.configuration.empty()) {
    StringAttributeLine(2, "configuration", record.configuration);
  }
  Indent(1);
  Raw("}\n");
}

void EirWriter::WriteVerification(const ProofObligation& obligation) {
  Indent(1);
  Raw("verify ");
  Identifier("proof obligation identifier", obligation.id);
  Raw(" {\n");

  // The goal is the one attribute whose *name* is its value, so it is written
  // as the attribute name rather than as a value under a fixed name.
  Indent(2);
  EnumTerminal("proof goal kind", obligation.goal_kind);
  Raw(" = ");
  Predicate(obligation.predicate, 0);
  Raw(";\n");

  if (!obligation.verifier_kinds.empty()) {
    IdentifierListLine(2, "using", obligation.verifier_kinds);
  }
  // An absent budget is `kUnspecified`; §12.1 makes it optional, so it is
  // omitted rather than written as an empty value. `ParseResourceBudget` admits
  // an integer literal or a call and nothing else.
  if (obligation.budget.kind == Expression::Kind::kInteger ||
      obligation.budget.kind == Expression::Kind::kCall) {
    Indent(2);
    Raw("budget = ");
    Value(obligation.budget);
    Raw(";\n");
  } else if (obligation.budget.kind != Expression::Kind::kUnspecified) {
    Refuse("a proof budget is neither an integer literal nor a function call, "
           "so it has no EIR-T spelling");
  }
  if (obligation.status != ProofStatus::kUnspecified) {
    EnumAttributeLine(2, "status", "proof status", obligation.status);
  }
  if (!obligation.result_id.empty()) {
    ReferenceAttributeLine(2, "result", obligation.result_id);
  }
  if (!obligation.verification_producer.empty()) {
    Indent(2);
    Raw("producer = ");
    ProducerCarrier("verification producer",
                    obligation.verification_producer);
    Raw(";\n");
  }
  Indent(1);
  Raw("}\n");
}

void EirWriter::WriteSummary(const SummaryReference& summary) {
  Indent(1);
  Raw("summary ");
  Identifier("summary reference identifier", summary.id);
  Raw(" {\n");
  ReferenceAttributeLine(2, "function", summary.function_id);
  StringAttributeLine(2, "summary_id", core::ToString(summary.summary_id));
  if (!summary.components.empty()) {
    IdentifierListLine(2, "components", summary.components);
  }
  Indent(1);
  Raw("}\n");
}

void EirWriter::WriteDependency(const Dependency& dependency) {
  Indent(1);
  Raw("dependency ");
  Identifier("dependency identifier", dependency.id);
  Raw(" {\n");
  EnumAttributeLine(2, "kind", "dependency kind", dependency.kind);
  StringAttributeLine(2, "stable_id", core::ToString(dependency.stable_id));
  Indent(1);
  Raw("}\n");
}

void EirWriter::WriteOmission(const Omission& omission) {
  Indent(1);
  Raw("omission ");
  Identifier("omission identifier", omission.id);
  Raw(" {\n");
  // `OmissionKind` is a `QualifiedId`, not a closed enumeration.
  Indent(2);
  Raw("kind = ");
  Identifier("omission kind", omission.kind);
  Raw(";\n");
  ReferenceAttributeLine(2, "subject", omission.subject);
  StringAttributeLine(2, "reason", omission.reason);
  // `expandable` is required by the production, so it is always written —
  // including when false, which has a spelling and is not an absence.
  AttributeLine(2, "expandable", omission.expandable ? "true" : "false");
  Indent(1);
  Raw("}\n");
}

Status EirWriter::Run(const EvidenceCase& value) {
  // `evidence {` — and no case identifier. §3.1 makes the label a display
  // label the model has no member for, so a writer that derived one from
  // `EvidenceID` would emit text that parses back to the same case and would
  // therefore pass a round-trip test. The first token after `evidence` is `{`.
  Raw("evidence {\n");

  StringAttributeLine(1, "schema", value.schema_version);
  EnumAttributeLine(1, "level", "evidence level", value.level);
  EnumAttributeLine(1, "state", "verification state", value.verification_state);

  Gap();
  WriteContext(value.program);

  {
    std::vector<SortKey> keys;
    keys.reserve(value.entities.size());
    for (const Entity& entity : value.entities) {
      keys.push_back(SortKey{std::string(ToString(entity.kind)),
                             StableIdComponent(entity.stable_id), entity.id});
    }
    const std::vector<std::size_t> order = CanonicalOrder(keys);
    for (const std::size_t index : order) {
      if (!ok()) {
        break;
      }
      Gap();
      WriteEntity(value.entities[index]);
    }
  }

  // Exactly one primary claim. `PrimaryClaim` is a single record rather than a
  // collection, so there is no order to choose.
  Gap();
  if (!value.primary_claim.id.empty()) {
    WriteClaim(value.primary_claim);
  } else {
    Refuse("the case declares no primary claim, which cannot be written");
  }

  {
    std::vector<SortKey> keys;
    keys.reserve(value.facts.size());
    for (const Fact& fact : value.facts) {
      keys.push_back(
          SortKey{std::string(), StableIdComponent(fact.stable_id), fact.id});
    }
    const std::vector<std::size_t> order = CanonicalOrder(keys);
    for (const std::size_t index : order) {
      if (!ok()) {
        break;
      }
      Gap();
      WriteFact(value.facts[index]);
    }
  }

  {
    std::vector<SortKey> keys;
    keys.reserve(value.assumptions.size());
    for (const Assumption& assumption : value.assumptions) {
      keys.push_back(SortKey{std::string(), std::string(), assumption.id});
    }
    const std::vector<std::size_t> order = CanonicalOrder(keys);
    for (const std::size_t index : order) {
      if (!ok()) {
        break;
      }
      Gap();
      WriteAssumption(value.assumptions[index]);
    }
  }

  {
    std::vector<SortKey> keys;
    keys.reserve(value.hypotheses.size());
    for (const Hypothesis& hypothesis : value.hypotheses) {
      keys.push_back(SortKey{std::string(), std::string(), hypothesis.id});
    }
    const std::vector<std::size_t> order = CanonicalOrder(keys);
    for (const std::size_t index : order) {
      if (!ok()) {
        break;
      }
      Gap();
      WriteHypothesis(value.hypotheses[index]);
    }
  }

  {
    std::vector<SortKey> keys;
    keys.reserve(value.unknowns.size());
    for (const Unknown& unknown : value.unknowns) {
      keys.push_back(SortKey{std::string(), std::string(), unknown.id});
    }
    const std::vector<std::size_t> order = CanonicalOrder(keys);
    for (const std::size_t index : order) {
      if (!ok()) {
        break;
      }
      Gap();
      WriteUnknown(value.unknowns[index]);
    }
  }

  {
    std::vector<SortKey> keys;
    keys.reserve(value.edges.size());
    for (const Edge& edge : value.edges) {
      keys.push_back(SortKey{std::string(ToString(edge.kind)), std::string(),
                             edge.id});
    }
    const std::vector<std::size_t> order = CanonicalOrder(keys);
    for (const std::size_t index : order) {
      if (!ok()) {
        break;
      }
      Gap();
      WriteEdge(value.edges[index]);
    }
  }

  {
    std::vector<SortKey> keys;
    keys.reserve(value.paths.size());
    for (const Path& path : value.paths) {
      keys.push_back(
          SortKey{std::string(ToString(path.kind)), std::string(), path.id});
    }
    const std::vector<std::size_t> order = CanonicalOrder(keys);
    for (const std::size_t index : order) {
      if (!ok()) {
        break;
      }
      Gap();
      WritePath(value.paths[index]);
    }
  }

  {
    std::vector<SortKey> keys;
    keys.reserve(value.constraints.size());
    for (const Constraint& constraint : value.constraints) {
      keys.push_back(SortKey{std::string(), std::string(), constraint.id});
    }
    const std::vector<std::size_t> order = CanonicalOrder(keys);
    for (const std::size_t index : order) {
      if (!ok()) {
        break;
      }
      Gap();
      WriteConstraint(value.constraints[index]);
    }
  }

  {
    std::vector<SortKey> keys;
    keys.reserve(value.provenance.size());
    for (const Provenance& record : value.provenance) {
      keys.push_back(SortKey{std::string(), std::string(), record.id});
    }
    const std::vector<std::size_t> order = CanonicalOrder(keys);
    for (const std::size_t index : order) {
      if (!ok()) {
        break;
      }
      Gap();
      WriteProvenance(value.provenance[index]);
    }
  }

  {
    std::vector<SortKey> keys;
    keys.reserve(value.proof_obligations.size());
    for (const ProofObligation& obligation : value.proof_obligations) {
      keys.push_back(SortKey{std::string(ToString(obligation.goal_kind)),
                             std::string(), obligation.id});
    }
    const std::vector<std::size_t> order = CanonicalOrder(keys);
    for (const std::size_t index : order) {
      if (!ok()) {
        break;
      }
      Gap();
      WriteVerification(value.proof_obligations[index]);
    }
  }

  {
    std::vector<SortKey> keys;
    keys.reserve(value.summaries.size());
    for (const SummaryReference& summary : value.summaries) {
      keys.push_back(SortKey{std::string(),
                             core::ToString(summary.summary_id), summary.id});
    }
    const std::vector<std::size_t> order = CanonicalOrder(keys);
    for (const std::size_t index : order) {
      if (!ok()) {
        break;
      }
      Gap();
      WriteSummary(value.summaries[index]);
    }
  }

  {
    std::vector<SortKey> keys;
    keys.reserve(value.dependencies.size());
    for (const Dependency& dependency : value.dependencies) {
      keys.push_back(SortKey{std::string(ToString(dependency.kind)),
                             core::ToString(dependency.stable_id),
                             dependency.id});
    }
    const std::vector<std::size_t> order = CanonicalOrder(keys);
    for (const std::size_t index : order) {
      if (!ok()) {
        break;
      }
      Gap();
      WriteDependency(value.dependencies[index]);
    }
  }

  {
    std::vector<SortKey> keys;
    keys.reserve(value.omissions.size());
    for (const Omission& omission : value.omissions) {
      keys.push_back(SortKey{std::string(), std::string(), omission.id});
    }
    const std::vector<std::size_t> order = CanonicalOrder(keys);
    for (const std::size_t index : order) {
      if (!ok()) {
        break;
      }
      Gap();
      WriteOmission(value.omissions[index]);
    }
  }

  Raw("}\n");
  return failed_;
}

}  // namespace

StatusOr<std::string> WriteEirText(const EvidenceCase& value,
                                   EirTextStyle style) {
  // The identity gate first. A case that has not been finalized carries no
  // `evidence_id`, and one whose identity is not its recomputed content address
  // is a case whose declared identity disagrees with its bytes; both are
  // refused rather than written. `FinalizeEvidenceIdentity` runs the
  // well-formedness gate and recomputes rather than trusting the ID, so this
  // one call covers both the validator and the identity check.
  if (!value.evidence_id.has_value()) {
    return Status::InvalidArgument(
        "the case carries no evidence_id: the canonical writer requires a "
        "finalized case, so call FinalizeEvidenceIdentity first");
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
        "semantics, so the writer will not stamp it onto bytes that disagree "
        "with it");
  }

  std::string out;
  EirWriter writer(style, &out);
  const Status status = writer.Run(value);
  if (!status.ok()) {
    return status;
  }
  return out;
}

}  // namespace veritas::evidence
