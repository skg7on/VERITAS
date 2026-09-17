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

// EirParser.cpp — EIR-T parsing and lowering. See `EirSyntax.h` for the parser
// contract and `veritas/evidence/EirText.h` for the public failure contract.
//
// TWO DIRECTIONAL RULES
//
// Every production of the stabilized grammar is parsed, including the optional
// attributes Task 7a added, because a field the grammar can write and the parser
// drops is a field that silently leaves `EvidenceID`.
//
// The reverse direction is a rejection, not a lowering. The EIR-T 1.0 language
// surface is deliberately wider than the V0.1 model, so a construct the model
// cannot carry is refused with a typed `"unsupported in EIR V0.1"` diagnostic.
// Two families reach it:
//
//   * a member attribute with no model home — `ProvenanceDecl`'s `location`
//     (distinct from `source_anchor`, which the model does carry) and
//     `EdgeDecl`'s `condition`, `transfer`, and `summary`; and
//   * a textual enum terminal outside the V0.1 subset the model's enums
//     declare, which the `Parse*` helpers in `EvidenceCase.h` already refuse.
//
// Lowering `location` onto `source_anchor_id`, or dropping an edge condition,
// would give two documents that differ only in that attribute one identity.
//
// WHAT IS LOWERED, AND HOW
//
// Three grammar forms have a plain `std::string` as their model carrier rather
// than a structured record: `ResolutionAction` (`Unknown::suggested_resolution`),
// `AssumptionSource` (`Assumption::source`), and `Scope` (`Assumption::scope`,
// `Constraint::scope`). Each is `FunctionCall`-shaped, and the model holds no
// call structure for any of them. They lower to the call's **canonical EIR-T
// spelling** — `callee(arg, ...)`, with a reference argument written bare, as
// the model writes every case-local handle. That spelling is a fixpoint of this
// lowering: re-parsing it yields an expression that renders to the same string,
// which is what keeps REP-001 true for those fields.
//
// `FactReference ::= "$" Identifier` lowers to its bare handle, like every other
// case-local reference: the `$` and the `@` belong to EIR-T syntax, and the
// model never stores them.
//
// ONE DELIBERATE WIDENING
//
// `AtomicPredicate` has no bare-identifier alternative, and `PropertyValue` has
// none either, yet the model's `Expression` declares `kSymbol` ("a bare name
// such as a domain value or a constant symbol"), and the specification's own §15
// example compares a reference against one (`@packet.type == EXTENSION`).
// Refusing the spelling would make that model value unrepresentable, and a
// writer that emitted it could not read its own output back. A bare identifier
// is therefore accepted in both positions and lowered to `kSymbol`. Accepting it
// cannot over-reject a document the grammar admits; rejecting it would break
// REP-001 for any case that carries a symbol. The inconsistency between §5.1's
// closed `AtomicPredicate` and §15's example is reported rather than resolved
// here.

#include "evidence/EirSyntax.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/evidence/EvidenceCanonicalizer.h"
#include "veritas/evidence/EvidenceCase.h"

namespace veritas::evidence {

namespace {

// The diagnostic every grammar-valid, model-unrepresentable construct is
// refused with. `docs/specs/veritas-evidence-ir-formal-specification.md` §11.1
// fixes the wording.
constexpr std::string_view kUnsupportedInEirV01 = "unsupported in EIR V0.1";

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

// The refused-attribute diagnostic: the attribute is named, the reason is
// stated, and the closing sentence says what a reader must not do instead. One
// spelling serves the whole family so a caller can match on it once.
std::string UnsupportedAttribute(std::string_view member,
                                 std::string_view attribute) {
  std::string message = "the ";
  message.append(member);
  message += " attribute ";
  message.append(Quoted(attribute));
  message += " is ";
  message.append(kUnsupportedInEirV01);
  message += ": the eir.v1 model carries no member for it, and the parser never "
             "drops it and never lowers it onto a neighbouring field";
  return message;
}

// --- Expression construction ------------------------------------------------

Expression MakeExpression(Expression::Kind kind, std::string text) {
  Expression expression;
  expression.kind = kind;
  expression.text = std::move(text);
  return expression;
}

Expression MakeReference(std::string text) {
  return MakeExpression(Expression::Kind::kReference, std::move(text));
}

Expression MakeString(std::string text) {
  return MakeExpression(Expression::Kind::kString, std::move(text));
}

Expression MakeSymbol(std::string text) {
  return MakeExpression(Expression::Kind::kSymbol, std::move(text));
}

Expression MakeBoolean(bool value) {
  Expression expression;
  expression.kind = Expression::Kind::kBool;
  expression.boolean = value;
  return expression;
}

Expression MakeCall(std::string callee, std::vector<Expression> arguments) {
  Expression expression;
  expression.kind = Expression::Kind::kCall;
  expression.text = std::move(callee);
  expression.operands = std::move(arguments);
  return expression;
}

// `kAnd` and `kOr` are n-ary and flat in the model, and `EvidenceValidator`
// rejects a directly nested same-kind operand. Parentheses can build exactly
// that shape (`(a and b) and c`), and the two spellings are the same formula, so
// same-kind children are spliced into one operand list. Both spellings then
// canonicalize to one node — and so to one `EvidenceID`.
Expression MakeConnective(Expression::Kind kind,
                          std::vector<Expression> operands) {
  std::vector<Expression> flattened;
  for (Expression& operand : operands) {
    if (operand.kind != kind) {
      flattened.push_back(std::move(operand));
      continue;
    }
    for (Expression& nested : operand.operands) {
      flattened.push_back(std::move(nested));
    }
  }
  Expression expression;
  expression.kind = kind;
  expression.operands = std::move(flattened);
  return expression;
}

// The comparison operator `kind` spells, or an empty string when it spells
// none. `ComparisonOp ::= "==" | "!=" | "<" | "<=" | ">" | ">="`.
std::string_view ComparisonText(TokenKind kind) {
  switch (kind) {
    case TokenKind::kEqualEqual:
      return "==";
    case TokenKind::kNotEqual:
      return "!=";
    case TokenKind::kLess:
      return "<";
    case TokenKind::kLessEqual:
      return "<=";
    case TokenKind::kGreater:
      return ">";
    case TokenKind::kGreaterEqual:
      return ">=";
    default:
      return {};
  }
}

// --- Call rendering ---------------------------------------------------------

void AppendEscaped(std::string_view text, std::string* out) {
  out->push_back('"');
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      out->push_back('\\');
    }
    out->push_back(c);
  }
  out->push_back('"');
}

// The canonical EIR-T spelling of one value inside a call argument list. A
// reference is written bare because the model writes every case-local handle
// bare; a string keeps its quotes so that a string argument is not read back as
// a symbol.
std::string RenderValue(const Expression& value) {
  switch (value.kind) {
    case Expression::Kind::kString: {
      std::string out;
      AppendEscaped(value.text, &out);
      return out;
    }
    case Expression::Kind::kInteger:
      return std::to_string(value.integer);
    case Expression::Kind::kBool:
      return value.boolean ? "true" : "false";
    case Expression::Kind::kReference:
    case Expression::Kind::kSymbol:
      return value.text;
    case Expression::Kind::kCall: {
      std::string out = value.text;
      out += '(';
      for (std::size_t index = 0; index < value.operands.size(); ++index) {
        if (index != 0) {
          out += ", ";
        }
        out += RenderValue(value.operands[index]);
      }
      out += ')';
      return out;
    }
    default:
      break;
  }
  return value.text;
}

}  // namespace

// --- Attribute bodies -------------------------------------------------------

// One `{ attribute = value ";" ... }` body. It owns the attribute names and the
// refusal vocabulary; the caller dispatches on the name it hands back and owns
// the value grammar. A repeated attribute is refused here — the model's fields
// are single-valued, so a second spelling can only be a document whose two
// readers would disagree on which one is the case.
class EirParser::AttributeReader {
 public:
  AttributeReader(EirParser* parser, std::string_view member)
      : parser_(parser), member_(member) {}

  Status Open() {
    return parser_->Expect(TokenKind::kLeftBrace,
                           Joined("'{' after the ", member_)
                               .append(" identifier"));
  }

  // The next attribute name, or a `kEnd` token once the body's `}` has been
  // consumed. The closing brace's own position is kept, because that is where a
  // reader that finds a required attribute missing is standing.
  StatusOr<Token> Next() {
    if (parser_->Peek().kind == TokenKind::kRightBrace) {
      close_ = parser_->Peek();
      parser_->Advance();
      return Token{};
    }
    if (parser_->AtEnd()) {
      return parser_->Fail(Joined("unterminated ", member_) +
                           " declaration: expected '}'");
    }
    return parser_->TakeIdentifier(Joined("a ", member_) + " attribute name");
  }

  // Records that `attribute` has been bound, refusing a repeat.
  Status Bind(const Token& attribute) {
    const auto inserted = bound_.emplace(attribute.text, attribute.offset);
    if (inserted.second) {
      return Status::Ok();
    }
    return parser_->FailAt(
        attribute, Joined("the ", member_) + " attribute " +
                       Quoted(attribute.text) + " appears more than once");
  }

  Status ExpectEqual(const Token& attribute) {
    return parser_->Expect(
        TokenKind::kEqual,
        Joined("'=' after the attribute ", Quoted(attribute.text)));
  }

  Status ExpectSemicolon(const Token& attribute) {
    return parser_->Expect(
        TokenKind::kSemicolon,
        Joined("';' after the value of ", Quoted(attribute.text)));
  }

  // The refusal for an attribute the grammar admits and the model cannot carry.
  Status Refuse(const Token& attribute) {
    return parser_->FailAt(attribute,
                           UnsupportedAttribute(member_, attribute.text));
  }

  // The refusal for an attribute the production does not list at all. Distinct
  // from `Refuse`: this one is a misspelling or a construct EIR-T never had,
  // not a widening of the language surface.
  Status Unknown(const Token& attribute) {
    return parser_->FailAt(attribute, Joined("unknown ", member_) +
                                          " attribute " +
                                          Quoted(attribute.text));
  }

  // The refusal for a body that closed with a required attribute unbound. It is
  // reported at the closing brace rather than left to the validator, which has
  // no source position to give.
  Status Missing(std::string_view attribute) {
    return parser_->FailAt(close_, Joined("the ", member_) +
                                       " declaration omits its required "
                                       "attribute " +
                                       Quoted(attribute));
  }

 private:
  EirParser* parser_;
  std::string_view member_;
  std::map<std::string, std::size_t> bound_;
  Token close_;
};

// --- Cursor and diagnostics -------------------------------------------------

std::string EirParser::Describe(const Token& token) {
  switch (token.kind) {
    case TokenKind::kEnd:
      return "end of input";
    case TokenKind::kIdentifier:
      return Joined("identifier ", Quoted(token.text));
    case TokenKind::kString:
      return "a string literal";
    case TokenKind::kInteger:
      return Joined("the integer literal ", Quoted(token.text));
    default:
      break;
  }
  return Joined("the token ", Quoted(token.text));
}

EirParser::EirParser(const std::vector<Token>& tokens, EirParseError* error)
    : tokens_(tokens), error_(error) {}

const Token& EirParser::Peek(std::size_t lookahead) const {
  // The lexer always appends exactly one `kEnd`, so the empty stream is only
  // reachable from a caller that built one by hand. It is answered with a
  // `kEnd` token rather than by reading past the end of an empty vector, so the
  // parser's contract does not depend on a property of its input that the
  // lexer's does not also state.
  static const Token kAbsent;
  const std::size_t index = cursor_ + lookahead;
  if (index >= tokens_.size()) {
    return tokens_.empty() ? kAbsent : tokens_.back();
  }
  return tokens_[index];
}

const Token& EirParser::Advance() {
  const Token& token = Peek();
  if (cursor_ + 1 < tokens_.size()) {
    ++cursor_;
  }
  return token;
}

bool EirParser::Match(TokenKind kind) {
  if (Peek().kind != kind) {
    return false;
  }
  Advance();
  return true;
}

bool EirParser::MatchKeyword(std::string_view keyword) {
  const Token& token = Peek();
  if (token.kind != TokenKind::kIdentifier || token.text != keyword) {
    return false;
  }
  Advance();
  return true;
}

Status EirParser::FailAt(const Token& token, std::string message) {
  if (error_ != nullptr) {
    error_->offset = token.offset;
    error_->line = token.line;
    error_->column = token.column;
    error_->message = message;
  }
  return Status::InvalidArgument(std::move(message));
}

Status EirParser::Fail(std::string message) {
  return FailAt(Peek(), std::move(message));
}

Status EirParser::Expect(TokenKind kind, std::string_view what) {
  if (Peek().kind != kind) {
    return Fail(Joined("expected ", what) + ", found " + Describe(Peek()));
  }
  Advance();
  return Status::Ok();
}

Status EirParser::ExpectKeyword(std::string_view keyword) {
  const Token& token = Peek();
  if (token.kind != TokenKind::kIdentifier || token.text != keyword) {
    return Fail(Joined("expected the keyword ", Quoted(keyword)) +
                ", found " + Describe(token));
  }
  Advance();
  return Status::Ok();
}

Status EirParser::ExpectPositionedKeyword(std::string_view keyword,
                                          std::string_view position) {
  const Token& token = Peek();
  if (token.kind == TokenKind::kIdentifier && token.text == keyword) {
    Advance();
    return Status::Ok();
  }
  // One message serves the four header keywords so a reader who sees any of
  // them learns the whole shape of the block; `position` and the found token
  // say which one was expected and what arrived instead.
  std::string message = "expected the '";
  message.append(keyword);
  message.append("' declaration ");
  message.append(position);
  message.append(
      ": the case declares its schema, level, and state once each, then its "
      "context, and only then its members; found ");
  message += Describe(token);
  return FailAt(token, std::move(message));
}

StatusOr<Token> EirParser::TakeIdentifier(std::string_view what) {
  const Token& token = Peek();
  if (token.kind != TokenKind::kIdentifier) {
    return Fail(Joined("expected ", what) + ", found " + Describe(token));
  }
  return Advance();
}

StatusOr<std::string> EirParser::TakeString(std::string_view what) {
  const Token& token = Peek();
  if (token.kind != TokenKind::kString) {
    return Fail(Joined("expected ", what) + ", found " + Describe(token));
  }
  Advance();
  return token.text;
}

StatusOr<std::string> EirParser::TakeReference(std::string_view what) {
  const Token& token = Peek();
  if (token.kind != TokenKind::kAt) {
    return Fail(Joined("expected ", what) + " introduced by '@', found " +
                Describe(token));
  }
  Advance();
  StatusOr<Token> name = TakeIdentifier(Joined(what, " after '@'"));
  if (!name.ok()) {
    return name.status();
  }
  return name.value().text;
}

// --- Case and context -------------------------------------------------------

Status EirParser::ParseEvidenceCase(EvidenceCase* out) {
  Status status = ExpectKeyword("evidence");
  if (!status.ok()) {
    return status;
  }
  // `EvidenceCase ::= "evidence" [ Identifier ] "{"`. The label is a display
  // label with no semantic content and no model member, so it is consumed and
  // discarded; it never reaches a field, and a case with a label and the same
  // case without one are the same object with the same `EvidenceID` (§3.1).
  if (Peek().kind == TokenKind::kIdentifier) {
    Advance();
  }
  status = Expect(TokenKind::kLeftBrace, "'{'");
  if (!status.ok()) {
    return status;
  }

  // `SchemaDecl LevelDecl StateDecl`, once each, in that order. Each of the
  // four keywords below is required rather than optional-and-repeated, so a
  // second declaration of any of the three, or a member that arrives before the
  // context, is refused at the position it was expected in.
  status = ExpectPositionedKeyword("schema", "first");
  if (!status.ok()) {
    return status;
  }
  status = Expect(TokenKind::kEqual, "'=' after the 'schema' declaration");
  if (!status.ok()) {
    return status;
  }
  StatusOr<std::string> schema = TakeString("the schema version string");
  if (!schema.ok()) {
    return schema.status();
  }
  out->schema_version = std::move(schema).value();
  // The literal is bound exactly as written, including a version this build does
  // not implement. The accepted version is a semantic field of the case, so a
  // different one is refused by the validator's `kSchemaVersion` check rather
  // than by a spelling rule here: "the schema version is not supported" and "the
  // schema declaration is malformed" are different rejections, and a caller that
  // saw only the second could not tell them apart.
  status = Expect(TokenKind::kSemicolon, "';' after the schema version");
  if (!status.ok()) {
    return status;
  }

  status = ExpectPositionedKeyword("level", "after the schema");
  if (!status.ok()) {
    return status;
  }
  status = Expect(TokenKind::kEqual, "'=' after 'level'");
  if (!status.ok()) {
    return status;
  }
  StatusOr<EvidenceLevel> level = TakeEnum("evidence level", ParseEvidenceLevel);
  if (!level.ok()) {
    return level.status();
  }
  out->level = level.value();
  status = Expect(TokenKind::kSemicolon, "';' after the evidence level");
  if (!status.ok()) {
    return status;
  }

  status = ExpectPositionedKeyword("state", "after the level");
  if (!status.ok()) {
    return status;
  }
  status = Expect(TokenKind::kEqual, "'=' after 'state'");
  if (!status.ok()) {
    return status;
  }
  StatusOr<VerificationState> state =
      TakeEnum("verification state", ParseVerificationState);
  if (!state.ok()) {
    return state.status();
  }
  out->verification_state = state.value();
  status = Expect(TokenKind::kSemicolon, "';' after the verification state");
  if (!status.ok()) {
    return status;
  }

  // `ContextDecl` follows the three declarations and precedes every member. A
  // member before the context is refused by requiring the keyword here rather
  // than by accepting either: the level and the state are hashed into
  // `EvidenceID`, so a parser that looped over the block and took the first or
  // the last would give two readers different identities for one text.
  status = ParseContext(&out->program);
  if (!status.ok()) {
    return status;
  }

  while (!Match(TokenKind::kRightBrace)) {
    if (AtEnd()) {
      return Fail("unterminated evidence case: expected '}' to close the "
                  "declaration opened at the start of the case");
    }
    status = ParseEvidenceMember(out);
    if (!status.ok()) {
      return status;
    }
  }
  return Status::Ok();
}

Status EirParser::ParseContext(ProgramBinding* out) {
  const Status opened = ExpectPositionedKeyword("context", "after the state");
  if (!opened.ok()) {
    return opened;
  }
  Status status = Expect(TokenKind::kLeftBrace, "'{' after 'context'");
  if (!status.ok()) {
    return status;
  }

  bool seen_repository = false;
  bool seen_revision = false;
  bool seen_build_variant = false;
  bool seen_target = false;
  bool seen_analyzer_configuration = false;
  bool seen_type_layout = false;
  bool seen_analysis_run = false;

  while (!Match(TokenKind::kRightBrace)) {
    if (AtEnd()) {
      return Fail("unterminated context declaration: expected '}'");
    }
    StatusOr<Token> key = TakeIdentifier("a context property name");
    if (!key.ok()) {
      return key.status();
    }
    const Token property = key.value();

    // `analyzer` is the only property that may repeat, once per contributing
    // analyzer; the other seven appear at most once each (§3.1). A repeat is
    // refused rather than letting the last spelling win, because every one of
    // these fields is hashed and two readers would otherwise disagree on the
    // identity of the same text.
    bool* seen = nullptr;
    if (property.text == "repository") {
      seen = &seen_repository;
    } else if (property.text == "revision") {
      seen = &seen_revision;
    } else if (property.text == "build_variant") {
      seen = &seen_build_variant;
    } else if (property.text == "target") {
      seen = &seen_target;
    } else if (property.text == "analyzer_configuration") {
      seen = &seen_analyzer_configuration;
    } else if (property.text == "type_layout") {
      seen = &seen_type_layout;
    } else if (property.text == "analysis_run") {
      seen = &seen_analysis_run;
    } else if (property.text != "analyzer") {
      return FailAt(
          property, Joined("unknown context property ", Quoted(property.text)));
    }
    if (seen != nullptr) {
      if (*seen) {
        return FailAt(property,
                      Joined("context property ", Quoted(property.text)) +
                          " appears more than once");
      }
      *seen = true;
    }

    status = Expect(TokenKind::kEqual,
                    Joined("'=' after ", Quoted(property.text)));
    if (!status.ok()) {
      return status;
    }

    if (property.text == "analyzer") {
      AnalyzerVersion version;
      status = ParseAnalyzerVersion(&version);
      if (!status.ok()) {
        return status;
      }
      out->analyzer_versions.push_back(std::move(version));
    } else {
      StatusOr<std::string> value =
          TakeString(Joined("the ", property.text).append(" string"));
      if (!value.ok()) {
        return value.status();
      }
      if (property.text == "repository") {
        out->repository_id = std::move(value).value();
      } else if (property.text == "revision") {
        out->revision_id = std::move(value).value();
      } else if (property.text == "build_variant") {
        out->build_variant_id = std::move(value).value();
      } else if (property.text == "target") {
        out->target_triple = std::move(value).value();
      } else if (property.text == "analyzer_configuration") {
        out->analysis_configuration_id = std::move(value).value();
      } else if (property.text == "type_layout") {
        out->type_layout_id = std::move(value).value();
      } else {
        StatusOr<core::StableId> run = core::ParseStableId(value.value());
        if (!run.ok()) {
          return FailAt(property,
                        Joined("the analysis_run property is not a stable ID "
                               "of the form <kind>:sha256:<digest>: ",
                               run.status().message()));
        }
        out->analysis_run_id = std::move(run).value();
      }
    }

    status = Expect(TokenKind::kSemicolon,
                    Joined("';' after the ", property.text).append(" property"));
    if (!status.ok()) {
      return status;
    }
  }
  return Status::Ok();
}

Status EirParser::ParseAnalyzerVersion(AnalyzerVersion* out) {
  // `AnalyzerVersion ::= Producer "(" [ StringLiteral [ "," StringLiteral ] ]
  // ")"`. The first argument is the analyzer's version and the second is the
  // configuration it ran under; both are optional, so a producer that ran with
  // neither is `producer()`.
  StatusOr<Token> callee = TakeIdentifier("an analyzer producer name");
  if (!callee.ok()) {
    return callee.status();
  }
  out->producer = callee.value().text;
  Status status = Expect(TokenKind::kLeftParen,
                         Joined("'(' after the analyzer producer ",
                                Quoted(out->producer)));
  if (!status.ok()) {
    return status;
  }
  if (Match(TokenKind::kRightParen)) {
    return Status::Ok();
  }
  StatusOr<std::string> version = TakeString("the analyzer version string");
  if (!version.ok()) {
    return version.status();
  }
  out->version = std::move(version).value();
  if (Match(TokenKind::kComma)) {
    StatusOr<std::string> configuration =
        TakeString("the analyzer configuration string");
    if (!configuration.ok()) {
      return configuration.status();
    }
    out->configuration = std::move(configuration).value();
  }
  return Expect(TokenKind::kRightParen,
                Joined("')' closing the analyzer version declared for ",
                       Quoted(out->producer)));
}

// --- Members ----------------------------------------------------------------

Status EirParser::ParseEvidenceMember(EvidenceCase* out) {
  const Token& token = Peek();
  if (token.kind != TokenKind::kIdentifier) {
    return Fail(Joined("expected an evidence member, found ", Describe(token)));
  }
  const std::string keyword = token.text;
  if (keyword == "claim") {
    return ParseClaim(out);
  }
  if (keyword == "entity") {
    return ParseEntity(out);
  }
  if (keyword == "fact") {
    return ParseFact(out);
  }
  if (keyword == "assumption") {
    return ParseAssumption(out);
  }
  if (keyword == "hypothesis") {
    return ParseHypothesis(out);
  }
  if (keyword == "unknown") {
    return ParseUnknown(out);
  }
  if (keyword == "edge") {
    return ParseEdge(out);
  }
  if (keyword == "path") {
    return ParsePath(out);
  }
  if (keyword == "constraint") {
    return ParseConstraint(out);
  }
  if (keyword == "provenance") {
    return ParseProvenance(out);
  }
  if (keyword == "verify") {
    return ParseVerification(out);
  }
  if (keyword == "summary") {
    return ParseSummary(out);
  }
  if (keyword == "dependency") {
    return ParseDependency(out);
  }
  if (keyword == "omission") {
    return ParseOmission(out);
  }
  return Fail(Joined("unknown evidence member ", Quoted(keyword)));
}

Status EirParser::ParseClaim(EvidenceCase* out) {
  const Token keyword = Peek();
  Advance();
  if (claim_declared_) {
    return FailAt(keyword,
                  "the case declares a second primary claim: exactly one claim "
                  "declaration is allowed");
  }
  claim_declared_ = true;

  Claim claim;
  StatusOr<Token> id = TakeIdentifier("a claim identifier");
  if (!id.ok()) {
    return id.status();
  }
  claim.id = id.value().text;

  AttributeReader body(this, "claim");
  Status status = body.Open();
  if (!status.ok()) {
    return status;
  }
  bool have_kind = false;
  bool have_subject = false;
  bool have_predicate = false;
  bool have_severity = false;
  for (;;) {
    StatusOr<Token> attribute = body.Next();
    if (!attribute.ok()) {
      return attribute.status();
    }
    const Token key = attribute.value();
    if (key.kind == TokenKind::kEnd) {
      break;
    }
    status = body.Bind(key);
    if (!status.ok()) {
      return status;
    }
    status = body.ExpectEqual(key);
    if (!status.ok()) {
      return status;
    }
    if (key.text == "kind") {
      StatusOr<ClaimKind> kind = TakeEnum("claim kind", ParseClaimKind);
      if (!kind.ok()) {
        return kind.status();
      }
      claim.kind = kind.value();
      have_kind = true;
    } else if (key.text == "subject") {
      StatusOr<std::string> subject = TakeReference("the claim subject");
      if (!subject.ok()) {
        return subject.status();
      }
      claim.subject = std::move(subject).value();
      have_subject = true;
    } else if (key.text == "predicate") {
      StatusOr<Expression> predicate = ParsePredicate();
      if (!predicate.ok()) {
        return predicate.status();
      }
      claim.predicate = std::move(predicate).value();
      have_predicate = true;
    } else if (key.text == "severity") {
      StatusOr<Severity> severity = TakeEnum("severity", ParseSeverity);
      if (!severity.ok()) {
        return severity.status();
      }
      claim.severity = severity.value();
      have_severity = true;
    } else if (key.text == "description") {
      StatusOr<std::string> description =
          TakeString("the claim description string");
      if (!description.ok()) {
        return description.status();
      }
      claim.description = std::move(description).value();
    } else {
      return body.Unknown(key);
    }
    status = body.ExpectSemicolon(key);
    if (!status.ok()) {
      return status;
    }
  }
  if (!have_kind) {
    return body.Missing("kind");
  }
  if (!have_subject) {
    return body.Missing("subject");
  }
  if (!have_predicate) {
    return body.Missing("predicate");
  }
  if (!have_severity) {
    return body.Missing("severity");
  }
  out->primary_claim = std::move(claim);
  return Status::Ok();
}

Status EirParser::ParseEntity(EvidenceCase* out) {
  Advance();  // "entity"
  Entity entity;
  StatusOr<Token> id = TakeIdentifier("an entity identifier");
  if (!id.ok()) {
    return id.status();
  }
  entity.id = id.value().text;

  Status status = Expect(TokenKind::kColon,
                         Joined("':' after the entity identifier ",
                                Quoted(entity.id)));
  if (!status.ok()) {
    return status;
  }
  StatusOr<EntityKind> kind = TakeEnum("entity kind", ParseEntityKind);
  if (!kind.ok()) {
    return kind.status();
  }
  entity.kind = kind.value();

  AttributeReader body(this, "entity");
  status = body.Open();
  if (!status.ok()) {
    return status;
  }
  // `[ "stable_id" "=" StringLiteral ";" ]` occupies the first position of the
  // body and only that position, so the identity is bound by position rather
  // than by name. Every later `stable_id` is an ordinary entry of the open
  // property bag, which is what lets an entity that declares an identity also
  // carry a property named `stable_id` (§4.1).
  bool at_first_attribute = true;
  for (;;) {
    StatusOr<Token> attribute = body.Next();
    if (!attribute.ok()) {
      return attribute.status();
    }
    const Token key = attribute.value();
    if (key.kind == TokenKind::kEnd) {
      break;
    }
    const bool is_identity = at_first_attribute && key.text == "stable_id";
    at_first_attribute = false;
    status = body.Bind(key);
    if (!status.ok()) {
      return status;
    }
    status = body.ExpectEqual(key);
    if (!status.ok()) {
      return status;
    }
    if (is_identity) {
      StatusOr<std::string> text = TakeString("the entity stable ID string");
      if (!text.ok()) {
        return text.status();
      }
      StatusOr<core::StableId> stable = core::ParseStableId(text.value());
      if (!stable.ok()) {
        return FailAt(key,
                      Joined("the entity stable_id is not a stable ID of the "
                             "form <kind>:sha256:<digest>: ",
                             stable.status().message()));
      }
      entity.stable_id = std::move(stable).value();
    } else {
      StatusOr<Expression> value = ParsePropertyValue();
      if (!value.ok()) {
        return value.status();
      }
      entity.properties.emplace(key.text, std::move(value).value());
    }
    status = body.ExpectSemicolon(key);
    if (!status.ok()) {
      return status;
    }
  }
  out->entities.push_back(std::move(entity));
  return Status::Ok();
}

Status EirParser::ParseFact(EvidenceCase* out) {
  Advance();  // "fact"
  Fact fact;
  StatusOr<Token> id = TakeIdentifier("a fact identifier");
  if (!id.ok()) {
    return id.status();
  }
  fact.id = id.value().text;

  AttributeReader body(this, "fact");
  Status status = body.Open();
  if (!status.ok()) {
    return status;
  }
  bool have_predicate = false;
  bool have_epistemic = false;
  for (;;) {
    StatusOr<Token> attribute = body.Next();
    if (!attribute.ok()) {
      return attribute.status();
    }
    const Token key = attribute.value();
    if (key.kind == TokenKind::kEnd) {
      break;
    }
    status = body.Bind(key);
    if (!status.ok()) {
      return status;
    }
    status = body.ExpectEqual(key);
    if (!status.ok()) {
      return status;
    }
    if (key.text == "predicate") {
      StatusOr<Expression> predicate = ParsePredicate();
      if (!predicate.ok()) {
        return predicate.status();
      }
      fact.predicate = std::move(predicate).value();
      have_predicate = true;
    } else if (key.text == "epistemic") {
      StatusOr<EpistemicState> epistemic =
          TakeEnum("epistemic state", ParseEpistemicState);
      if (!epistemic.ok()) {
        return epistemic.status();
      }
      fact.epistemic = epistemic.value();
      have_epistemic = true;
    } else if (key.text == "confidence") {
      StatusOr<Confidence> confidence =
          TakeEnum("confidence", ParseConfidence);
      if (!confidence.ok()) {
        return confidence.status();
      }
      fact.confidence = confidence.value();
    } else if (key.text == "source") {
      StatusOr<Token> producer = TakeIdentifier("a fact producer");
      if (!producer.ok()) {
        return producer.status();
      }
      fact.producer = producer.value().text;
    } else if (key.text == "provenance") {
      StatusOr<std::string> provenance = TakeReference("the fact provenance");
      if (!provenance.ok()) {
        return provenance.status();
      }
      fact.provenance_id = std::move(provenance).value();
    } else if (key.text == "stable_id") {
      StatusOr<std::string> text = TakeString("the fact stable ID string");
      if (!text.ok()) {
        return text.status();
      }
      StatusOr<core::StableId> stable = core::ParseStableId(text.value());
      if (!stable.ok()) {
        return FailAt(key, Joined("the fact stable_id is not a stable ID of the "
                                  "form <kind>:sha256:<digest>: ",
                                  stable.status().message()));
      }
      fact.stable_id = std::move(stable).value();
    } else if (key.text == "derived") {
      StatusOr<Expression> value = ParsePropertyValue();
      if (!value.ok()) {
        return value.status();
      }
      if (value.value().kind != Expression::Kind::kBool) {
        return FailAt(key,
                      "the fact 'derived' attribute takes a boolean literal");
      }
      fact.derived = value.value().boolean;
    } else {
      return body.Unknown(key);
    }
    status = body.ExpectSemicolon(key);
    if (!status.ok()) {
      return status;
    }
  }
  if (!have_predicate) {
    return body.Missing("predicate");
  }
  if (!have_epistemic) {
    return body.Missing("epistemic");
  }
  out->facts.push_back(std::move(fact));
  return Status::Ok();
}

Status EirParser::ParseAssumption(EvidenceCase* out) {
  Advance();  // "assumption"
  Assumption assumption;
  StatusOr<Token> id = TakeIdentifier("an assumption identifier");
  if (!id.ok()) {
    return id.status();
  }
  assumption.id = id.value().text;

  AttributeReader body(this, "assumption");
  Status status = body.Open();
  if (!status.ok()) {
    return status;
  }
  bool have_predicate = false;
  bool have_source = false;
  for (;;) {
    StatusOr<Token> attribute = body.Next();
    if (!attribute.ok()) {
      return attribute.status();
    }
    const Token key = attribute.value();
    if (key.kind == TokenKind::kEnd) {
      break;
    }
    status = body.Bind(key);
    if (!status.ok()) {
      return status;
    }
    status = body.ExpectEqual(key);
    if (!status.ok()) {
      return status;
    }
    if (key.text == "predicate") {
      StatusOr<Expression> predicate = ParsePredicate();
      if (!predicate.ok()) {
        return predicate.status();
      }
      assumption.predicate = std::move(predicate).value();
      have_predicate = true;
    } else if (key.text == "source") {
      StatusOr<std::string> source = ParseCallOrQualifiedId(key, "source");
      if (!source.ok()) {
        return source.status();
      }
      assumption.source = std::move(source).value();
      have_source = true;
    } else if (key.text == "scope") {
      StatusOr<std::string> scope = ParseCallOrQualifiedId(key, "scope");
      if (!scope.ok()) {
        return scope.status();
      }
      assumption.scope = std::move(scope).value();
    } else {
      return body.Unknown(key);
    }
    status = body.ExpectSemicolon(key);
    if (!status.ok()) {
      return status;
    }
  }
  if (!have_predicate) {
    return body.Missing("predicate");
  }
  if (!have_source) {
    return body.Missing("source");
  }
  out->assumptions.push_back(std::move(assumption));
  return Status::Ok();
}

Status EirParser::ParseHypothesis(EvidenceCase* out) {
  Advance();  // "hypothesis"
  Hypothesis hypothesis;
  StatusOr<Token> id = TakeIdentifier("a hypothesis identifier");
  if (!id.ok()) {
    return id.status();
  }
  hypothesis.id = id.value().text;

  AttributeReader body(this, "hypothesis");
  Status status = body.Open();
  if (!status.ok()) {
    return status;
  }
  bool have_predicate = false;
  bool have_producer = false;
  for (;;) {
    StatusOr<Token> attribute = body.Next();
    if (!attribute.ok()) {
      return attribute.status();
    }
    const Token key = attribute.value();
    if (key.kind == TokenKind::kEnd) {
      break;
    }
    status = body.Bind(key);
    if (!status.ok()) {
      return status;
    }
    status = body.ExpectEqual(key);
    if (!status.ok()) {
      return status;
    }
    if (key.text == "predicate") {
      StatusOr<Expression> predicate = ParsePredicate();
      if (!predicate.ok()) {
        return predicate.status();
      }
      hypothesis.predicate = std::move(predicate).value();
      have_predicate = true;
    } else if (key.text == "producer") {
      StatusOr<Token> producer = TakeIdentifier("a hypothesis producer");
      if (!producer.ok()) {
        return producer.status();
      }
      hypothesis.producer = producer.value().text;
      have_producer = true;
    } else if (key.text == "reason") {
      StatusOr<std::string> reason = TakeString("the hypothesis reason string");
      if (!reason.ok()) {
        return reason.status();
      }
      hypothesis.reason = std::move(reason).value();
    } else if (key.text == "confidence") {
      StatusOr<Confidence> confidence =
          TakeEnum("confidence", ParseConfidence);
      if (!confidence.ok()) {
        return confidence.status();
      }
      hypothesis.confidence = confidence.value();
    } else {
      return body.Unknown(key);
    }
    status = body.ExpectSemicolon(key);
    if (!status.ok()) {
      return status;
    }
  }
  if (!have_predicate) {
    return body.Missing("predicate");
  }
  if (!have_producer) {
    return body.Missing("producer");
  }
  out->hypotheses.push_back(std::move(hypothesis));
  return Status::Ok();
}

Status EirParser::ParseUnknown(EvidenceCase* out) {
  Advance();  // "unknown"
  Unknown unknown;
  StatusOr<Token> id = TakeIdentifier("an unknown identifier");
  if (!id.ok()) {
    return id.status();
  }
  unknown.id = id.value().text;

  AttributeReader body(this, "unknown");
  Status status = body.Open();
  if (!status.ok()) {
    return status;
  }
  bool have_property = false;
  bool have_reason = false;
  for (;;) {
    StatusOr<Token> attribute = body.Next();
    if (!attribute.ok()) {
      return attribute.status();
    }
    const Token key = attribute.value();
    if (key.kind == TokenKind::kEnd) {
      break;
    }
    status = body.Bind(key);
    if (!status.ok()) {
      return status;
    }
    status = body.ExpectEqual(key);
    if (!status.ok()) {
      return status;
    }
    if (key.text == "property") {
      StatusOr<Expression> property = ParsePredicate();
      if (!property.ok()) {
        return property.status();
      }
      unknown.property = std::move(property).value();
      have_property = true;
    } else if (key.text == "reason") {
      StatusOr<UnknownReasonCode> reason =
          TakeEnum("unknown reason", ParseUnknownReasonCode);
      if (!reason.ok()) {
        return reason.status();
      }
      unknown.reason_code = reason.value();
      have_reason = true;
    } else if (key.text == "detail") {
      // The closed `reason` code travels with the free-text sentence the
      // analysis recorded, and the model keeps both. Rounding an observed
      // sentence to its nearest terminal would lose what the case knows (§7.3).
      StatusOr<std::string> detail = TakeString("the unknown detail string");
      if (!detail.ok()) {
        return detail.status();
      }
      unknown.reason = std::move(detail).value();
    } else if (key.text == "blocking") {
      StatusOr<std::vector<std::string>> blocking =
          ParseReferenceList("a blocking fact");
      if (!blocking.ok()) {
        return blocking.status();
      }
      unknown.blocking_ids = std::move(blocking).value();
    } else if (key.text == "suggested_resolution") {
      StatusOr<Expression> action = ParsePropertyValue();
      if (!action.ok()) {
        return action.status();
      }
      // `ResolutionAction ::= FunctionCall`. The model carries it as a plain
      // string, so it lowers to the call's canonical EIR-T spelling, which is a
      // fixpoint of this lowering.
      if (action.value().kind != Expression::Kind::kCall) {
        return FailAt(key,
                      "the unknown 'suggested_resolution' attribute takes a "
                      "function call");
      }
      unknown.suggested_resolution = RenderValue(action.value());
    } else {
      return body.Unknown(key);
    }
    status = body.ExpectSemicolon(key);
    if (!status.ok()) {
      return status;
    }
  }
  if (!have_property) {
    return body.Missing("property");
  }
  if (!have_reason) {
    return body.Missing("reason");
  }
  out->unknowns.push_back(std::move(unknown));
  return Status::Ok();
}

Status EirParser::ParseEdge(EvidenceCase* out) {
  Advance();  // "edge"
  Edge edge;
  StatusOr<Token> id = TakeIdentifier("an edge identifier");
  if (!id.ok()) {
    return id.status();
  }
  edge.id = id.value().text;

  AttributeReader body(this, "edge");
  Status status = body.Open();
  if (!status.ok()) {
    return status;
  }
  bool have_from = false;
  bool have_to = false;
  bool have_kind = false;
  for (;;) {
    StatusOr<Token> attribute = body.Next();
    if (!attribute.ok()) {
      return attribute.status();
    }
    const Token key = attribute.value();
    if (key.kind == TokenKind::kEnd) {
      break;
    }
    // The three `EdgeDecl` attributes the model cannot carry are refused before
    // the attribute is recorded as bound, so a document that uses one is
    // rejected rather than reduced: `condition` is a predicate on the relation,
    // `transfer` is its transfer function, and `summary` is the function call it
    // expands to. `Edge` has no member for any of them, and dropping one would
    // give two edges that differ only in their condition the same identity.
    if (key.text == "condition" || key.text == "transfer" ||
        key.text == "summary") {
      return body.Refuse(key);
    }
    status = body.Bind(key);
    if (!status.ok()) {
      return status;
    }
    status = body.ExpectEqual(key);
    if (!status.ok()) {
      return status;
    }
    if (key.text == "from") {
      StatusOr<std::string> from = TakeReference("the edge source");
      if (!from.ok()) {
        return from.status();
      }
      edge.from = std::move(from).value();
      have_from = true;
    } else if (key.text == "to") {
      StatusOr<std::string> to = TakeReference("the edge target");
      if (!to.ok()) {
        return to.status();
      }
      edge.to = std::move(to).value();
      have_to = true;
    } else if (key.text == "kind") {
      StatusOr<RelationKind> kind =
          TakeEnum("relation kind", ParseRelationKind);
      if (!kind.ok()) {
        return kind.status();
      }
      edge.kind = kind.value();
      have_kind = true;
    } else if (key.text == "epistemic") {
      StatusOr<EpistemicState> epistemic =
          TakeEnum("epistemic state", ParseEpistemicState);
      if (!epistemic.ok()) {
        return epistemic.status();
      }
      edge.epistemic = epistemic.value();
    } else if (key.text == "provenance") {
      StatusOr<std::string> provenance = TakeReference("the edge provenance");
      if (!provenance.ok()) {
        return provenance.status();
      }
      edge.provenance_id = std::move(provenance).value();
    } else if (key.text == "summarized_by") {
      StatusOr<std::string> summarized =
          TakeReference("the edge's summary reference");
      if (!summarized.ok()) {
        return summarized.status();
      }
      edge.summarized_by = std::move(summarized).value();
    } else if (key.text == "expandable") {
      StatusOr<Expression> value = ParsePropertyValue();
      if (!value.ok()) {
        return value.status();
      }
      if (value.value().kind != Expression::Kind::kBool) {
        return FailAt(
            key, "the edge 'expandable' attribute takes a boolean literal");
      }
      edge.expandable = value.value().boolean;
    } else {
      return body.Unknown(key);
    }
    status = body.ExpectSemicolon(key);
    if (!status.ok()) {
      return status;
    }
  }
  if (!have_from) {
    return body.Missing("from");
  }
  if (!have_to) {
    return body.Missing("to");
  }
  if (!have_kind) {
    return body.Missing("kind");
  }
  out->edges.push_back(std::move(edge));
  return Status::Ok();
}

Status EirParser::ParsePath(EvidenceCase* out) {
  Advance();  // "path"
  Path path;
  StatusOr<Token> id = TakeIdentifier("a path identifier");
  if (!id.ok()) {
    return id.status();
  }
  path.id = id.value().text;

  StatusOr<PathKind> kind = TakeEnum("path kind", ParsePathKind);
  if (!kind.ok()) {
    return kind.status();
  }
  path.kind = kind.value();

  Status status = Expect(TokenKind::kLeftBrace, "'{' after the path kind");
  if (!status.ok()) {
    return status;
  }

  // `PathExpression ::= Reference { PathOp Reference } ";"`. The sequence is the
  // path and is never reordered, so it is kept in source order.
  StatusOr<std::string> first = TakeReference("the first path segment");
  if (!first.ok()) {
    return first.status();
  }
  path.entity_ids.push_back(std::move(first).value());
  while (Match(TokenKind::kArrow)) {
    StatusOr<std::string> segment = TakeReference("a path segment after '->'");
    if (!segment.ok()) {
      return segment.status();
    }
    path.entity_ids.push_back(std::move(segment).value());
  }
  status = Expect(TokenKind::kSemicolon, "';' after the path expression");
  if (!status.ok()) {
    return status;
  }

  bool have_conditions = false;
  bool have_feasible = false;
  bool have_provenance = false;
  while (!Match(TokenKind::kRightBrace)) {
    if (AtEnd()) {
      return Fail("unterminated path declaration: expected '}'");
    }
    StatusOr<Token> key = TakeIdentifier("a path attribute name");
    if (!key.ok()) {
      return key.status();
    }
    const Token attribute = key.value();
    if (attribute.text == "conditions") {
      if (have_conditions) {
        return FailAt(attribute,
                      "the path 'conditions' block appears more than once");
      }
      have_conditions = true;
      status = Expect(TokenKind::kLeftBrace, "'{' after 'conditions'");
      if (!status.ok()) {
        return status;
      }
      while (!Match(TokenKind::kRightBrace)) {
        if (AtEnd()) {
          return Fail("unterminated path conditions block: expected '}'");
        }
        StatusOr<Expression> condition = ParsePredicate();
        if (!condition.ok()) {
          return condition.status();
        }
        path.conditions.push_back(std::move(condition).value());
        status = Expect(TokenKind::kSemicolon, "';' after a path condition");
        if (!status.ok()) {
          return status;
        }
      }
      continue;
    }
    status = Expect(TokenKind::kEqual,
                    Joined("'=' after the attribute ", Quoted(attribute.text)));
    if (!status.ok()) {
      return status;
    }
    if (attribute.text == "feasible") {
      if (have_feasible) {
        return FailAt(attribute,
                      "the path 'feasible' attribute appears more than once");
      }
      have_feasible = true;
      StatusOr<Feasibility> feasibility =
          TakeEnum("feasibility", ParseFeasibility);
      if (!feasibility.ok()) {
        return feasibility.status();
      }
      path.feasibility = feasibility.value();
    } else if (attribute.text == "provenance") {
      if (have_provenance) {
        return FailAt(attribute,
                      "the path 'provenance' attribute appears more than once");
      }
      have_provenance = true;
      StatusOr<std::string> provenance = TakeReference("the path provenance");
      if (!provenance.ok()) {
        return provenance.status();
      }
      path.provenance_id = std::move(provenance).value();
    } else {
      return FailAt(
          attribute, Joined("unknown path attribute ", Quoted(attribute.text)));
    }
    status = Expect(TokenKind::kSemicolon,
                    Joined("';' after the value of ", Quoted(attribute.text)));
    if (!status.ok()) {
      return status;
    }
  }
  out->paths.push_back(std::move(path));
  return Status::Ok();
}

Status EirParser::ParseConstraint(EvidenceCase* out) {
  Advance();  // "constraint"
  Constraint constraint;
  StatusOr<Token> id = TakeIdentifier("a constraint identifier");
  if (!id.ok()) {
    return id.status();
  }
  constraint.id = id.value().text;

  AttributeReader body(this, "constraint");
  Status status = body.Open();
  if (!status.ok()) {
    return status;
  }
  bool have_expression = false;
  for (;;) {
    StatusOr<Token> attribute = body.Next();
    if (!attribute.ok()) {
      return attribute.status();
    }
    const Token key = attribute.value();
    if (key.kind == TokenKind::kEnd) {
      break;
    }
    status = body.Bind(key);
    if (!status.ok()) {
      return status;
    }
    status = body.ExpectEqual(key);
    if (!status.ok()) {
      return status;
    }
    if (key.text == "expr") {
      StatusOr<Expression> expression = ParsePredicate();
      if (!expression.ok()) {
        return expression.status();
      }
      constraint.expression = std::move(expression).value();
      have_expression = true;
    } else if (key.text == "scope") {
      StatusOr<std::string> scope = ParseCallOrQualifiedId(key, "scope");
      if (!scope.ok()) {
        return scope.status();
      }
      constraint.scope = std::move(scope).value();
    } else if (key.text == "epistemic") {
      StatusOr<EpistemicState> epistemic =
          TakeEnum("epistemic state", ParseEpistemicState);
      if (!epistemic.ok()) {
        return epistemic.status();
      }
      constraint.epistemic = epistemic.value();
    } else if (key.text == "provenance") {
      StatusOr<std::string> provenance =
          TakeReference("the constraint provenance");
      if (!provenance.ok()) {
        return provenance.status();
      }
      constraint.provenance_id = std::move(provenance).value();
    } else {
      return body.Unknown(key);
    }
    status = body.ExpectSemicolon(key);
    if (!status.ok()) {
      return status;
    }
  }
  if (!have_expression) {
    return body.Missing("expr");
  }
  out->constraints.push_back(std::move(constraint));
  return Status::Ok();
}

Status EirParser::ParseProvenance(EvidenceCase* out) {
  Advance();  // "provenance"
  Provenance record;
  StatusOr<Token> id = TakeIdentifier("a provenance identifier");
  if (!id.ok()) {
    return id.status();
  }
  record.id = id.value().text;

  AttributeReader body(this, "provenance");
  Status status = body.Open();
  if (!status.ok()) {
    return status;
  }
  bool have_producer = false;
  for (;;) {
    StatusOr<Token> attribute = body.Next();
    if (!attribute.ok()) {
      return attribute.status();
    }
    const Token key = attribute.value();
    if (key.kind == TokenKind::kEnd) {
      break;
    }
    // `location` is grammar-valid and model-unrepresentable. It is refused here,
    // before it can be recorded as bound, and it is never lowered onto
    // `source_anchor_id`: `location` and `source_anchor` name different things,
    // and either a substitution or a silent drop would change `EvidenceID` for
    // such an input and break REP-001 (formal specification §11.1).
    if (key.text == "location") {
      return body.Refuse(key);
    }
    status = body.Bind(key);
    if (!status.ok()) {
      return status;
    }
    status = body.ExpectEqual(key);
    if (!status.ok()) {
      return status;
    }
    if (key.text == "producer") {
      StatusOr<Token> producer = TakeIdentifier("a provenance producer");
      if (!producer.ok()) {
        return producer.status();
      }
      record.producer = producer.value().text;
      have_producer = true;
    } else if (key.text == "rule") {
      StatusOr<std::string> rule = TakeString("the provenance rule string");
      if (!rule.ok()) {
        return rule.status();
      }
      record.rule = std::move(rule).value();
    } else if (key.text == "inputs") {
      StatusOr<std::vector<std::string>> inputs = ParseFactReferenceList();
      if (!inputs.ok()) {
        return inputs.status();
      }
      record.input_fact_ids = std::move(inputs).value();
    } else if (key.text == "version") {
      StatusOr<std::string> version =
          TakeString("the provenance version string");
      if (!version.ok()) {
        return version.status();
      }
      record.version = std::move(version).value();
    } else if (key.text == "configuration") {
      StatusOr<std::string> configuration =
          TakeString("the provenance configuration string");
      if (!configuration.ok()) {
        return configuration.status();
      }
      record.configuration = std::move(configuration).value();
    } else if (key.text == "source_anchor") {
      StatusOr<std::string> anchor =
          TakeString("the provenance source anchor string");
      if (!anchor.ok()) {
        return anchor.status();
      }
      record.source_anchor_id = std::move(anchor).value();
    } else if (key.text == "analysis_run") {
      // Declared explicitly and never derived from the case binding: the run is
      // a semantic field of the record, so it has a textual representation of
      // its own, and the equality with the case's own run is a constraint the
      // document satisfies rather than a substitution the parser performs on the
      // reader's behalf (§11.1).
      StatusOr<std::string> text =
          TakeString("the provenance analysis run string");
      if (!text.ok()) {
        return text.status();
      }
      StatusOr<core::StableId> run = core::ParseStableId(text.value());
      if (!run.ok()) {
        return FailAt(key,
                      Joined("the provenance analysis_run is not a stable ID "
                             "of the form <kind>:sha256:<digest>: ",
                             run.status().message()));
      }
      record.analysis_run_id = std::move(run).value();
    } else {
      return body.Unknown(key);
    }
    status = body.ExpectSemicolon(key);
    if (!status.ok()) {
      return status;
    }
  }
  if (!have_producer) {
    return body.Missing("producer");
  }
  out->provenance.push_back(std::move(record));
  return Status::Ok();
}

Status EirParser::ParseVerification(EvidenceCase* out) {
  Advance();  // "verify"
  ProofObligation obligation;
  StatusOr<Token> id = TakeIdentifier("a proof obligation identifier");
  if (!id.ok()) {
    return id.status();
  }
  obligation.id = id.value().text;

  AttributeReader body(this, "verify");
  Status status = body.Open();
  if (!status.ok()) {
    return status;
  }
  bool have_goal = false;
  for (;;) {
    StatusOr<Token> attribute = body.Next();
    if (!attribute.ok()) {
      return attribute.status();
    }
    const Token key = attribute.value();
    if (key.kind == TokenKind::kEnd) {
      break;
    }
    status = body.Bind(key);
    if (!status.ok()) {
      return status;
    }
    status = body.ExpectEqual(key);
    if (!status.ok()) {
      return status;
    }
    if (key.text == "prove" || key.text == "refute" || key.text == "check") {
      if (have_goal) {
        return FailAt(key, "the proof obligation declares more than one goal");
      }
      have_goal = true;
      // The three spellings the branch above admits are exactly the three
      // `ParseProofGoalKind` recognises, so the mapping cannot fail; a fourth
      // goal spelling is an unknown attribute and is refused by `body.Unknown`.
      obligation.goal_kind = ParseProofGoalKind(key.text).value();
      StatusOr<Expression> predicate = ParsePredicate();
      if (!predicate.ok()) {
        return predicate.status();
      }
      obligation.predicate = std::move(predicate).value();
    } else if (key.text == "using") {
      StatusOr<std::vector<std::string>> backends =
          ParseIdentifierList("a verification backend name");
      if (!backends.ok()) {
        return backends.status();
      }
      obligation.verifier_kinds = std::move(backends).value();
    } else if (key.text == "budget") {
      StatusOr<Expression> budget = ParseResourceBudget();
      if (!budget.ok()) {
        return budget.status();
      }
      obligation.budget = std::move(budget).value();
    } else if (key.text == "status") {
      StatusOr<ProofStatus> proof_status =
          TakeEnum("proof status", ParseProofStatus);
      if (!proof_status.ok()) {
        return proof_status.status();
      }
      obligation.status = proof_status.value();
    } else if (key.text == "result") {
      StatusOr<std::string> result = TakeReference("the proof result");
      if (!result.ok()) {
        return result.status();
      }
      obligation.result_id = std::move(result).value();
    } else if (key.text == "producer") {
      StatusOr<Token> producer = TakeIdentifier("a verification producer");
      if (!producer.ok()) {
        return producer.status();
      }
      obligation.verification_producer = producer.value().text;
    } else {
      return body.Unknown(key);
    }
    status = body.ExpectSemicolon(key);
    if (!status.ok()) {
      return status;
    }
  }
  if (!have_goal) {
    return body.Missing("prove");
  }
  out->proof_obligations.push_back(std::move(obligation));
  return Status::Ok();
}

Status EirParser::ParseSummary(EvidenceCase* out) {
  Advance();  // "summary"
  SummaryReference summary;
  StatusOr<Token> id = TakeIdentifier("a summary reference identifier");
  if (!id.ok()) {
    return id.status();
  }
  summary.id = id.value().text;

  AttributeReader body(this, "summary");
  Status status = body.Open();
  if (!status.ok()) {
    return status;
  }
  bool have_function = false;
  bool have_summary_id = false;
  for (;;) {
    StatusOr<Token> attribute = body.Next();
    if (!attribute.ok()) {
      return attribute.status();
    }
    const Token key = attribute.value();
    if (key.kind == TokenKind::kEnd) {
      break;
    }
    status = body.Bind(key);
    if (!status.ok()) {
      return status;
    }
    status = body.ExpectEqual(key);
    if (!status.ok()) {
      return status;
    }
    if (key.text == "function") {
      StatusOr<std::string> function = TakeReference("the summarized function");
      if (!function.ok()) {
        return function.status();
      }
      summary.function_id = std::move(function).value();
      have_function = true;
    } else if (key.text == "summary_id") {
      StatusOr<std::string> text = TakeString("the summary ID string");
      if (!text.ok()) {
        return text.status();
      }
      StatusOr<core::StableId> stable = core::ParseStableId(text.value());
      if (!stable.ok()) {
        return FailAt(key,
                      Joined("the summary_id is not a stable ID of the form "
                             "<kind>:sha256:<digest>: ",
                             stable.status().message()));
      }
      summary.summary_id = std::move(stable).value();
      have_summary_id = true;
    } else if (key.text == "components") {
      StatusOr<std::vector<std::string>> components =
          ParseIdentifierList("a summary component name");
      if (!components.ok()) {
        return components.status();
      }
      summary.components = std::move(components).value();
    } else {
      return body.Unknown(key);
    }
    status = body.ExpectSemicolon(key);
    if (!status.ok()) {
      return status;
    }
  }
  if (!have_function) {
    return body.Missing("function");
  }
  if (!have_summary_id) {
    return body.Missing("summary_id");
  }
  out->summaries.push_back(std::move(summary));
  return Status::Ok();
}

Status EirParser::ParseDependency(EvidenceCase* out) {
  Advance();  // "dependency"
  Dependency dependency;
  StatusOr<Token> id = TakeIdentifier("a dependency identifier");
  if (!id.ok()) {
    return id.status();
  }
  dependency.id = id.value().text;

  AttributeReader body(this, "dependency");
  Status status = body.Open();
  if (!status.ok()) {
    return status;
  }
  bool have_kind = false;
  bool have_stable_id = false;
  for (;;) {
    StatusOr<Token> attribute = body.Next();
    if (!attribute.ok()) {
      return attribute.status();
    }
    const Token key = attribute.value();
    if (key.kind == TokenKind::kEnd) {
      break;
    }
    status = body.Bind(key);
    if (!status.ok()) {
      return status;
    }
    status = body.ExpectEqual(key);
    if (!status.ok()) {
      return status;
    }
    if (key.text == "kind") {
      StatusOr<DependencyKind> kind =
          TakeEnum("dependency kind", ParseDependencyKind);
      if (!kind.ok()) {
        return kind.status();
      }
      dependency.kind = kind.value();
      have_kind = true;
    } else if (key.text == "stable_id") {
      StatusOr<std::string> text = TakeString("the dependency stable ID");
      if (!text.ok()) {
        return text.status();
      }
      StatusOr<core::StableId> stable = core::ParseStableId(text.value());
      if (!stable.ok()) {
        return FailAt(key,
                      Joined("the dependency stable_id is not a stable ID of "
                             "the form <kind>:sha256:<digest>: ",
                             stable.status().message()));
      }
      dependency.stable_id = std::move(stable).value();
      have_stable_id = true;
    } else {
      return body.Unknown(key);
    }
    status = body.ExpectSemicolon(key);
    if (!status.ok()) {
      return status;
    }
  }
  if (!have_kind) {
    return body.Missing("kind");
  }
  if (!have_stable_id) {
    return body.Missing("stable_id");
  }
  out->dependencies.push_back(std::move(dependency));
  return Status::Ok();
}

Status EirParser::ParseOmission(EvidenceCase* out) {
  Advance();  // "omission"
  Omission omission;
  StatusOr<Token> id = TakeIdentifier("an omission identifier");
  if (!id.ok()) {
    return id.status();
  }
  omission.id = id.value().text;

  AttributeReader body(this, "omission");
  Status status = body.Open();
  if (!status.ok()) {
    return status;
  }
  bool have_kind = false;
  bool have_subject = false;
  bool have_reason = false;
  bool have_expandable = false;
  for (;;) {
    StatusOr<Token> attribute = body.Next();
    if (!attribute.ok()) {
      return attribute.status();
    }
    const Token key = attribute.value();
    if (key.kind == TokenKind::kEnd) {
      break;
    }
    status = body.Bind(key);
    if (!status.ok()) {
      return status;
    }
    status = body.ExpectEqual(key);
    if (!status.ok()) {
      return status;
    }
    if (key.text == "kind") {
      // `OmissionKind` is a `QualifiedId`, not a closed enumeration.
      StatusOr<Token> kind = TakeIdentifier("the omission kind");
      if (!kind.ok()) {
        return kind.status();
      }
      omission.kind = kind.value().text;
      have_kind = true;
    } else if (key.text == "subject") {
      StatusOr<std::string> subject = TakeReference("the omission subject");
      if (!subject.ok()) {
        return subject.status();
      }
      omission.subject = std::move(subject).value();
      have_subject = true;
    } else if (key.text == "reason") {
      StatusOr<std::string> reason = TakeString("the omission reason string");
      if (!reason.ok()) {
        return reason.status();
      }
      omission.reason = std::move(reason).value();
      have_reason = true;
    } else if (key.text == "expandable") {
      StatusOr<Expression> value = ParsePropertyValue();
      if (!value.ok()) {
        return value.status();
      }
      if (value.value().kind != Expression::Kind::kBool) {
        return FailAt(
            key, "the omission 'expandable' attribute takes a boolean literal");
      }
      omission.expandable = value.value().boolean;
      have_expandable = true;
    } else {
      return body.Unknown(key);
    }
    status = body.ExpectSemicolon(key);
    if (!status.ok()) {
      return status;
    }
  }
  if (!have_kind) {
    return body.Missing("kind");
  }
  if (!have_subject) {
    return body.Missing("subject");
  }
  if (!have_reason) {
    return body.Missing("reason");
  }
  if (!have_expandable) {
    return body.Missing("expandable");
  }
  out->omissions.push_back(std::move(omission));
  return Status::Ok();
}

// --- Predicates -------------------------------------------------------------

StatusOr<Expression> EirParser::ParsePredicate() {
  if (MatchKeyword("forall")) {
    return ParseQuantified(Expression::Kind::kForAll);
  }
  if (MatchKeyword("exists")) {
    return ParseQuantified(Expression::Kind::kExists);
  }
  return ParseImplication();
}

StatusOr<Expression> EirParser::ParseImplication() {
  StatusOr<Expression> left = ParseOr();
  if (!left.ok()) {
    return left.status();
  }
  if (!MatchKeyword("implies")) {
    return left;
  }
  // `ImplicationExpr ::= OrExpr [ "implies" ImplicationExpr ]` — right
  // associative, so the consequent is parsed at this level rather than at `or`'s.
  StatusOr<Expression> right = ParseImplication();
  if (!right.ok()) {
    return right.status();
  }
  Expression expression;
  expression.kind = Expression::Kind::kImplies;
  expression.operands.push_back(std::move(left).value());
  expression.operands.push_back(std::move(right).value());
  return expression;
}

StatusOr<Expression> EirParser::ParseOr() {
  StatusOr<Expression> first = ParseAnd();
  if (!first.ok()) {
    return first.status();
  }
  if (!MatchKeyword("or")) {
    return first;
  }
  std::vector<Expression> operands;
  operands.push_back(std::move(first).value());
  do {
    StatusOr<Expression> next = ParseAnd();
    if (!next.ok()) {
      return next.status();
    }
    operands.push_back(std::move(next).value());
  } while (MatchKeyword("or"));
  return MakeConnective(Expression::Kind::kOr, std::move(operands));
}

StatusOr<Expression> EirParser::ParseAnd() {
  StatusOr<Expression> first = ParseComparison();
  if (!first.ok()) {
    return first.status();
  }
  if (!MatchKeyword("and")) {
    return first;
  }
  std::vector<Expression> operands;
  operands.push_back(std::move(first).value());
  do {
    StatusOr<Expression> next = ParseComparison();
    if (!next.ok()) {
      return next.status();
    }
    operands.push_back(std::move(next).value());
  } while (MatchKeyword("and"));
  return MakeConnective(Expression::Kind::kAnd, std::move(operands));
}

StatusOr<Expression> EirParser::ParseComparison() {
  StatusOr<Expression> left = ParseUnary();
  if (!left.ok()) {
    return left.status();
  }
  const Token& token = Peek();
  const std::string_view op = ComparisonText(token.kind);
  if (op.empty()) {
    return left;
  }
  Advance();
  StatusOr<Expression> right = ParseUnary();
  if (!right.ok()) {
    return right.status();
  }
  // `ComparisonExpr ::= UnaryExpr [ ComparisonOp UnaryExpr ]` — the trailing
  // comparison is never repeated, so a comparison is not associative. A chained
  // one is refused rather than grouped, because neither grouping is what the
  // writer wrote and choosing one would give the chain an identity the grammar
  // never assigned it.
  const std::string_view chained = ComparisonText(Peek().kind);
  if (!chained.empty()) {
    return Fail(Joined("a comparison is not associative: ", Quoted(op)) +
                " and " + Quoted(chained) +
                " cannot be chained; write the two comparisons as an explicit "
                "conjunction");
  }
  Expression expression;
  expression.kind = Expression::Kind::kCompare;
  expression.text = std::string(op);
  expression.operands.push_back(std::move(left).value());
  expression.operands.push_back(std::move(right).value());
  return expression;
}

StatusOr<Expression> EirParser::ParseUnary() {
  if (MatchKeyword("not")) {
    StatusOr<Expression> operand = ParseUnary();
    if (!operand.ok()) {
      return operand.status();
    }
    Expression expression;
    expression.kind = Expression::Kind::kNot;
    expression.operands.push_back(std::move(operand).value());
    return expression;
  }
  return ParsePrimary();
}

StatusOr<Expression> EirParser::ParseQuantified(Expression::Kind kind) {
  const StatusOr<Token> variable = TakeIdentifier("a bound variable name");
  if (!variable.ok()) {
    return variable.status();
  }
  Status status = ExpectKeyword("in");
  if (!status.ok()) {
    return status;
  }

  // `Domain ::= Identifier "(" [ ArgumentList ] ")" | Reference`.
  Expression domain;
  if (Peek().kind == TokenKind::kAt) {
    StatusOr<std::string> reference = TakeReference("the quantifier domain");
    if (!reference.ok()) {
      return reference.status();
    }
    domain = MakeReference(std::move(reference).value());
  } else {
    StatusOr<Token> callee = TakeIdentifier("a quantifier domain");
    if (!callee.ok()) {
      return callee.status();
    }
    status =
        Expect(TokenKind::kLeftParen,
               Joined("'(' after the domain ", Quoted(callee.value().text)));
    if (!status.ok()) {
      return status;
    }
    StatusOr<std::vector<Expression>> arguments = ParsePropertyValueList();
    if (!arguments.ok()) {
      return arguments.status();
    }
    status =
        Expect(TokenKind::kRightParen, "')' closing the quantifier domain");
    if (!status.ok()) {
      return status;
    }
    domain = MakeCall(callee.value().text, std::move(arguments).value());
  }

  status = Expect(TokenKind::kColon, "':' after the quantifier domain");
  if (!status.ok()) {
    return status;
  }
  // A quantifier is the loosest level, so it owns everything after its colon.
  StatusOr<Expression> body = ParsePredicate();
  if (!body.ok()) {
    return body.status();
  }
  Expression expression;
  expression.kind = kind;
  expression.text = variable.value().text;
  expression.operands.push_back(std::move(domain));
  expression.operands.push_back(std::move(body).value());
  return expression;
}

StatusOr<Expression> EirParser::ParsePrimary() {
  const Token& token = Peek();
  if (token.kind == TokenKind::kLeftParen) {
    Advance();
    StatusOr<Expression> inner = ParsePredicate();
    if (!inner.ok()) {
      return inner.status();
    }
    const Status status =
        Expect(TokenKind::kRightParen, "')' closing a parenthesised predicate");
    if (!status.ok()) {
      return status;
    }
    return inner;
  }
  if (token.kind == TokenKind::kString) {
    Advance();
    return MakeString(token.text);
  }
  if (token.kind == TokenKind::kInteger) {
    Advance();
    return ParseIntegerToken(token);
  }
  if (token.kind == TokenKind::kAt) {
    StatusOr<std::string> reference =
        TakeReference("an expression reference");
    if (!reference.ok()) {
      return reference.status();
    }
    return MakeReference(std::move(reference).value());
  }
  if (token.kind == TokenKind::kIdentifier) {
    // `AtomicPredicate ::= Identifier "(" [ PredicateArgumentList ] ")" |
    // Reference | BooleanLiteral`.
    if (Peek(1).kind == TokenKind::kLeftParen) {
      Advance();
      Advance();
      StatusOr<std::vector<Expression>> arguments =
          ParsePredicateArgumentList();
      if (!arguments.ok()) {
        return arguments.status();
      }
      const Status status =
          Expect(TokenKind::kRightParen, "')' closing a predicate call");
      if (!status.ok()) {
        return status;
      }
      return MakeCall(token.text, std::move(arguments).value());
    }
    Advance();
    if (token.text == "true" || token.text == "false") {
      return MakeBoolean(token.text == "true");
    }
    // A bare identifier is not a listed `AtomicPredicate` alternative, yet the
    // model's `kSymbol` exists for exactly this value and §15's own example
    // compares a reference against one. It is accepted rather than refused; see
    // the file comment for why refusing it would break REP-001.
    return MakeSymbol(token.text);
  }
  return Fail(Joined("expected a predicate, found ", Describe(token)));
}

// --- Values -----------------------------------------------------------------

StatusOr<Expression> EirParser::ParsePropertyValue() {
  const Token& token = Peek();
  if (token.kind == TokenKind::kString) {
    Advance();
    return MakeString(token.text);
  }
  if (token.kind == TokenKind::kInteger) {
    Advance();
    return ParseIntegerToken(token);
  }
  if (token.kind == TokenKind::kAt) {
    StatusOr<std::string> reference = TakeReference("a reference value");
    if (!reference.ok()) {
      return reference.status();
    }
    return MakeReference(std::move(reference).value());
  }
  if (token.kind == TokenKind::kIdentifier) {
    // `PropertyValue ::= StringLiteral | IntegerLiteral | BooleanLiteral |
    // Reference | FunctionCall`. The bare-identifier widening is the same one
    // `ParsePrimary` documents and is made for the same reason.
    if (Peek(1).kind == TokenKind::kLeftParen) {
      Advance();
      Advance();
      StatusOr<std::vector<Expression>> arguments = ParsePropertyValueList();
      if (!arguments.ok()) {
        return arguments.status();
      }
      const Status status =
          Expect(TokenKind::kRightParen, "')' closing a function call");
      if (!status.ok()) {
        return status;
      }
      return MakeCall(token.text, std::move(arguments).value());
    }
    Advance();
    if (token.text == "true" || token.text == "false") {
      return MakeBoolean(token.text == "true");
    }
    return MakeSymbol(token.text);
  }
  return Fail(Joined("expected a property value, found ", Describe(token)));
}

StatusOr<std::string> EirParser::ParseCallOrQualifiedId(const Token& attribute,
                                                        std::string_view what) {
  // `AssumptionSource ::= FunctionCall | QualifiedId`, and the two
  // identifier-shaped alternatives of `Scope` share that pair. Both lower to a
  // string: the call to its canonical EIR-T spelling, which is a fixpoint of
  // this lowering, and the identifier to its own spelling.
  StatusOr<Expression> value = ParsePropertyValue();
  if (!value.ok()) {
    return value.status();
  }
  if (value.value().kind == Expression::Kind::kCall) {
    return RenderValue(value.value());
  }
  if (value.value().kind == Expression::Kind::kSymbol) {
    return value.value().text;
  }
  return FailAt(attribute,
                Joined("the ", what) +
                    " attribute takes a qualified identifier or a function "
                    "call");
}

StatusOr<std::vector<Expression>> EirParser::ParsePropertyValueList() {
  std::vector<Expression> arguments;
  if (Peek().kind == TokenKind::kRightParen) {
    return arguments;
  }
  for (;;) {
    StatusOr<Expression> argument = ParsePropertyValue();
    if (!argument.ok()) {
      return argument.status();
    }
    arguments.push_back(std::move(argument).value());
    if (!Match(TokenKind::kComma)) {
      return arguments;
    }
  }
}

StatusOr<std::vector<Expression>> EirParser::ParsePredicateArgumentList() {
  std::vector<Expression> arguments;
  if (Peek().kind == TokenKind::kRightParen) {
    return arguments;
  }
  for (;;) {
    // `PredicateArgument ::= Reference | IntegerLiteral | StringLiteral |
    // Identifier | Predicate`. Every alternative is reached through the
    // predicate chain, whose primary level admits the reference, the two
    // literals, and the bare identifier.
    StatusOr<Expression> argument = ParsePredicate();
    if (!argument.ok()) {
      return argument.status();
    }
    arguments.push_back(std::move(argument).value());
    if (!Match(TokenKind::kComma)) {
      return arguments;
    }
  }
}

StatusOr<Expression> EirParser::ParseResourceBudget() {
  if (Peek().kind == TokenKind::kInteger) {
    const Token token = Advance();
    return ParseIntegerToken(token);
  }
  if (Peek().kind == TokenKind::kIdentifier &&
      Peek(1).kind == TokenKind::kLeftParen) {
    const Token callee = Advance();
    Advance();
    StatusOr<std::vector<Expression>> arguments = ParsePropertyValueList();
    if (!arguments.ok()) {
      return arguments.status();
    }
    const Status status =
        Expect(TokenKind::kRightParen, "')' closing a resource budget call");
    if (!status.ok()) {
      return status;
    }
    return MakeCall(callee.text, std::move(arguments).value());
  }
  return Fail(Joined("expected a resource budget, found ", Describe(Peek())));
}

StatusOr<std::vector<std::string>> EirParser::ParseIdentifierList(
    std::string_view what) {
  Status status = Expect(TokenKind::kLeftBracket, "'[' opening the list");
  if (!status.ok()) {
    return status;
  }
  std::vector<std::string> values;
  if (Match(TokenKind::kRightBracket)) {
    return values;
  }
  for (;;) {
    StatusOr<Token> value = TakeIdentifier(what);
    if (!value.ok()) {
      return value.status();
    }
    values.push_back(value.value().text);
    if (!Match(TokenKind::kComma)) {
      break;
    }
  }
  status = Expect(TokenKind::kRightBracket, "']' closing the list");
  if (!status.ok()) {
    return status;
  }
  return values;
}

StatusOr<std::vector<std::string>> EirParser::ParseReferenceList(
    std::string_view what) {
  Status status = Expect(TokenKind::kLeftBracket, "'[' opening the list");
  if (!status.ok()) {
    return status;
  }
  std::vector<std::string> values;
  if (Match(TokenKind::kRightBracket)) {
    return values;
  }
  for (;;) {
    StatusOr<std::string> value = TakeReference(what);
    if (!value.ok()) {
      return value.status();
    }
    values.push_back(std::move(value).value());
    if (!Match(TokenKind::kComma)) {
      break;
    }
  }
  status = Expect(TokenKind::kRightBracket, "']' closing the list");
  if (!status.ok()) {
    return status;
  }
  return values;
}

StatusOr<std::vector<std::string>> EirParser::ParseFactReferenceList() {
  Status status = Expect(TokenKind::kLeftBracket, "'[' opening the list");
  if (!status.ok()) {
    return status;
  }
  std::vector<std::string> values;
  if (Match(TokenKind::kRightBracket)) {
    return values;
  }
  for (;;) {
    // `FactReference ::= "$" Identifier`. The sigil distinguishes a case-local
    // fact member from the `Reference` spelling used everywhere else, and like
    // the `@` it belongs to the syntax rather than to the model value.
    status = Expect(TokenKind::kDollar, "'$' introducing a fact reference");
    if (!status.ok()) {
      return status;
    }
    StatusOr<Token> value = TakeIdentifier("a fact reference after '$'");
    if (!value.ok()) {
      return value.status();
    }
    values.push_back(value.value().text);
    if (!Match(TokenKind::kComma)) {
      break;
    }
  }
  status = Expect(TokenKind::kRightBracket, "']' closing the list");
  if (!status.ok()) {
    return status;
  }
  return values;
}

StatusOr<Expression> EirParser::ParseIntegerToken(const Token& token) {
  std::int64_t value = 0;
  const char* const first = token.text.data();
  const char* const last = first + token.text.size();
  const std::from_chars_result result = std::from_chars(first, last, value);
  if (result.ec != std::errc() || result.ptr != last) {
    return FailAt(token,
                  Joined("the integer literal ", Quoted(token.text)) +
                      " is outside the range of a signed 64-bit integer");
  }
  Expression expression;
  // `text` stays empty. The per-kind contract carries an integer in `integer`
  // alone, and the assembly path writes it the same way, so one formula does not
  // depend on which boundary produced it.
  expression.kind = Expression::Kind::kInteger;
  expression.integer = value;
  return expression;
}

// --- Entry point ------------------------------------------------------------

StatusOr<EvidenceCase> EirParser::Parse() {
  EvidenceCase value;
  const Status status = ParseEvidenceCase(&value);
  if (!status.ok()) {
    return status;
  }
  if (!AtEnd()) {
    return Fail(Joined("expected the end of the document after the evidence "
                       "case, found ",
                       Describe(Peek())));
  }
  return value;
}

StatusOr<EvidenceCase> ParseEirText(std::string_view source,
                                    EirParseError* error) {
  EirLexer lexer(source, error);
  StatusOr<std::vector<Token>> tokens = lexer.Tokenize();
  if (!tokens.ok()) {
    return tokens.status();
  }

  EirParser parser(tokens.value(), error);
  StatusOr<EvidenceCase> value = parser.Parse();
  if (!value.ok()) {
    return value.status();
  }
  EvidenceCase result = std::move(value).value();

  // `FinalizeEvidenceIdentity` runs the `RequireValidEvidenceCase` gate first
  // and assigns the identity only after it succeeds, so the two stages cannot
  // disagree and a caller never receives a half-built case. A well-formedness
  // rejection is about the case as a whole rather than about a token, so it
  // reports the start of the document and names the offending member in its own
  // message.
  const Status finalized = FinalizeEvidenceIdentity(&result);
  if (!finalized.ok()) {
    if (error != nullptr) {
      error->offset = 0;
      error->line = 1;
      error->column = 1;
      error->message = std::string(finalized.message());
    }
    return finalized;
  }
  return result;
}

}  // namespace veritas::evidence
