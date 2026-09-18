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

// EirSyntax.h — the internal token model and lexer for EIR-T, the Evidence IR
// text format specified by `docs/specs/veritas-evidence-ir-formal-specification.md`.
//
// This header is private to `src/evidence`. Its public half is
// `veritas/evidence/EirText.h`, which carries `EirParseError` alone: the token
// model below is shared only by the lexer's unit test and by the EIR-T parser
// and writer, so it is deliberately not installed under `include/`.
//
// A LEXER THAT DECIDES NOTHING
//
// The lexer cuts the source into tokens and reports the first malformed byte
// sequence. It does not know the keyword table, it does not check that `{`
// closes a `}`, and it does not require any token to follow any other. Every
// EIR-T keyword is a plain `kIdentifier` whose spelling the parser matches —
// `evidence E {` and `evidence {` produce the same kinds, the second merely
// carrying fewer tokens, because the case label of §3.1 is optional and
// non-semantic. A lexer that "helpfully" claimed the label would reject a legal
// case, so the label is left for the parser to accept or refuse.
//
// THE TOKEN KIND SET IS THE GRAMMAR'S TERMINALS, NOT A CHARACTER TABLE
//
// `TokenKind` has 23 members and none of them is a minus sign, a dot, or an
// assignment operator, for three reasons that the grammar settles (§2.1–2.5):
//
//   * `IntegerLiteral ::= [ "-" ] Digit { Digit }` puts the sign inside the
//     literal, so `-5` is one `kInteger` token whose `text` is `"-5"`. There is
//     no bare-minus token to confuse it with, and `-` is therefore only ever
//     the start of an integer or of `PathOp ::= "->"` — which the lexer decides
//     in a single character of lookahead. A `-` followed by neither a digit nor
//     `>` is a lexical error, not a token.
//   * `QualifiedId ::= Identifier { "." Identifier }` is used by real
//     productions (the omission `kind`, the path `subject`, and the `location`
//     values among them) yet the grammar defines no `"."` terminal. A dotted
//     qualified id is therefore one `kIdentifier` token that carries its dots:
//     `@a.b.c` lexes as `kAt`, `kIdentifier("a.b.c")`. A dot continues the
//     identifier only when a `Letter` immediately follows it — no interior
//     whitespace — so `a. b` is `a`, then a stray `.` and a lexical error.
//   * `AssignmentOp ::= "=" | ":="` is defined and referenced by no
//     production, exactly as `LogicalOp` is not. `:=` needs no token: it lexes
//     as `kColon` then `kEqual`, and no parser has to recognize the pair.
//
// TWO READINGS OF `{ Character }`
//
// `LineComment ::= "//" { Character } "\n"` cannot be read strictly, because a
// `Character` that may itself be `"\n"` lets one comment swallow the file. The
// repetition is therefore read as excluding `"\n"`, and the comment ends at the
// first newline — or at end of input, which is a deliberate leniency: a
// trailing `// note` with no final newline is accepted. `BlockComment`, by
// contrast, is terminated by `"*/"` or not at all, so an unclosed `/*` is
// unambiguously an error and is reported at the `/*` that opened it.
//
// UTF-8
//
// The source is UTF-8. Every byte that is not ASCII is validated as part of a
// well-formed UTF-8 sequence wherever the lexer consumes it — inside comments,
// inside string literals, and in the token dispatch — and a malformed sequence
// is rejected at its first byte. Well-formed non-ASCII text is legal inside a
// string literal and inside a comment; outside those two it begins no token,
// because `Letter ::= "a".."z" | "A".."Z"` and no other production admits a
// non-ASCII character.
//
// FAILURE MODES
//
// `Tokenize` fails only with `Status::InvalidArgument`, whose message is the
// same text stored in the `EirParseError` the lexer was constructed with (when
// one was supplied). The first malformed construct in source order wins: the
// lexer emits nothing for it and stops, so a caller never sees a partial token
// stream described as a success.

#ifndef VERITAS_EVIDENCE_EIR_SYNTAX_H_
#define VERITAS_EVIDENCE_EIR_SYNTAX_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "veritas/core/Status.h"
#include "veritas/evidence/EirText.h"
#include "veritas/evidence/EvidenceCase.h"

namespace veritas::evidence {

// Every token the EIR-T lexical grammar defines, plus `kEnd`.
//
// There is no keyword kind: `evidence`, `claim`, `must`, and the rest of §2.4
// arrive as `kIdentifier` and are matched by text in the parser, which is the
// only component that can tell a keyword from a member name that happens to
// spell one.
enum class TokenKind {
  kIdentifier,
  kString,
  kInteger,
  kAt,
  kDollar,
  kLeftBrace,
  kRightBrace,
  kLeftParen,
  kRightParen,
  kLeftBracket,
  kRightBracket,
  kColon,
  kSemicolon,
  kComma,
  kEqual,
  kEqualEqual,
  kNotEqual,
  kLess,
  kLessEqual,
  kGreater,
  kGreaterEqual,
  kArrow,
  kEnd,
};

// One token, with the position of its first byte.
//
// `text` is the token's source spelling, with two exceptions: it is the
// **decoded** value of a `kString` (escape sequences resolved, surrounding
// quotes omitted), and it is empty for `kEnd`, which occupies no source.
// A `kInteger`'s spelling includes its minus sign and always parses into a
// `std::int64_t`, because the lexer range-checks it before emitting the token.
//
// `offset` is a zero-based byte offset; `line` and `column` are one-based and
// both count bytes, as `EirParseError` documents.
struct Token {
  TokenKind kind = TokenKind::kEnd;
  std::string text;
  std::size_t offset = 0;
  std::size_t line = 1;
  std::size_t column = 1;
};

// Turns EIR-T source into the token sequence the parser consumes.
//
// The lexer borrows the source and the error sink; neither may be destroyed or
// moved while it is in use.
class EirLexer {
 public:
  // `error` receives the position and message of the first rejection, and may
  // be null when the caller only needs the returned `Status`.
  EirLexer(std::string_view source, EirParseError* error);

  // Tokenizes the whole source, ending with exactly one `kEnd` token positioned
  // one byte past the last byte of input.
  //
  // Returns `InvalidArgument` for the first construct the lexical grammar does
  // not admit — an unterminated string or block comment, a malformed UTF-8
  // sequence, an invalid string escape, a `-` that begins neither an integer
  // nor `->`, an integer literal outside `std::int64_t`, or a character that
  // begins no token. An empty source succeeds and yields `kEnd` alone.
  //
  // May be called more than once; each call restarts from the beginning of the
  // source.
  StatusOr<std::vector<Token>> Tokenize();

 private:
  // The character `lookahead` bytes past the cursor, or `'\0'` when that is
  // past the end. Callers must test `AtEnd` rather than this sentinel, because
  // a source may legitimately contain a NUL byte.
  char Peek(std::size_t lookahead = 0) const;

  // Consumes and returns one byte, updating the line coordinates when it is a
  // newline. Advances nothing and returns `'\0'` at end of input.
  char Advance();

  // Consumes whitespace, line comments, and block comments, leaving the cursor
  // on the first byte that begins a token or on end of input. A block comment
  // with no closing `"*/"` is rejected here, at its opening `"/*"`.
  Status SkipWhitespaceAndComments();

  // Cuts one token beginning at the cursor and appends it to `*out`.
  Status LexToken(std::vector<Token>* out);

  // The cursor methods of the lexical grammar.
  Status LexIdentifier(std::vector<Token>* out);
  Status LexString(std::vector<Token>* out);
  Status LexInteger(std::vector<Token>* out);

  // Appends a one- and a two-byte token whose text is its own spelling.
  Status Emit(TokenKind kind, std::vector<Token>* out);
  Status EmitPair(TokenKind kind, std::vector<Token>* out);

  // Records a rejection at `offset`/`line`/`column` in the error sink, when one
  // was supplied, and returns it as the `Status` to propagate.
  Status Fail(std::size_t offset, std::size_t line, std::size_t column,
              std::string message);

  bool AtEnd() const { return cursor_ >= source_.size(); }

  // The one-based byte column of the cursor on the line it is on.
  std::size_t Column() const { return cursor_ - line_start_ + 1; }

  std::string_view source_;
  EirParseError* error_;

  std::size_t cursor_ = 0;
  std::size_t line_ = 1;
  std::size_t line_start_ = 0;
};

// Recursive descent over the token stream `EirLexer` produced.
//
// One method per production of the stabilized grammar
// (`docs/specs/veritas-evidence-ir-formal-specification.md` §3–§13), each
// lowering directly into the `eir.v1` records of
// `veritas/evidence/EvidenceCase.h`. No intermediate syntax tree is built and
// no Protobuf message is constructed: the domain records are the only product,
// which is what keeps the parser and the Protobuf codec agreeing on one model.
//
// The parser owns three structural rules the grammar states in prose rather
// than in a production, and each is a rejection rather than a repair:
//
//   * the three case declarations and `ContextDecl` appear once each, in §3.1's
//     order, before any `EvidenceMember`;
//   * exactly one `claim` member is declared;
//   * every attribute is declared at most once, and an attribute the production
//     does not list is refused rather than ignored.
//
// The third rule is what makes the widening of the language surface safe: a
// construct the model cannot carry is refused with a typed diagnostic instead of
// being dropped or lowered onto a neighbouring field, so no two documents that
// differ only in such an attribute can share an `EvidenceID`.
class EirParser {
 public:
  // `tokens` must be the lexer's output and must still be alive, and must end
  // with `kEnd`. `error` receives the position and message of the first
  // rejection and may be null.
  EirParser(const std::vector<Token>& tokens, EirParseError* error);

  // Parses one `EvidenceCase`. Does not validate and does not compute an
  // identity: `ParseEirText` owns both, so a parser test can inspect a case the
  // validator would refuse.
  //
  // Returns `InvalidArgument` for the first construct the grammar does not
  // admit, and for the first construct it admits but the model cannot carry.
  StatusOr<EvidenceCase> Parse();

 private:
  // --- Cursor ---------------------------------------------------------------

  const Token& Peek(std::size_t lookahead = 0) const;
  const Token& Advance();
  bool AtEnd() const { return Peek().kind == TokenKind::kEnd; }

  // Consumes the token when it has `kind` (or is an identifier spelling
  // `keyword`) and reports whether it did.
  bool Match(TokenKind kind);
  bool MatchKeyword(std::string_view keyword);

  // Requires the next token, consuming it on success.
  Status Expect(TokenKind kind, std::string_view what);
  Status ExpectKeyword(std::string_view keyword);
  // `ExpectKeyword`, with the rejection explaining where the keyword belongs.
  // The case header's four keywords are single-valued and ordered; `position`
  // distinguishes the four sites so a reader sees which one was expected.
  Status ExpectPositionedKeyword(std::string_view keyword,
                                 std::string_view position);

  // Requires an identifier and consumes it, returning its spelling.
  StatusOr<Token> TakeIdentifier(std::string_view what);
  // Requires a string literal and consumes it, returning its decoded value.
  StatusOr<std::string> TakeString(std::string_view what);
  // Requires `"@" Identifier` and consumes both, returning the bare identifier.
  StatusOr<std::string> TakeReference(std::string_view what);

  // Requires an identifier naming one terminal of a textual enum family and
  // consumes it. Rejection covers two cases: the token is not an identifier,
  // and the spelling is outside the family.
  //
  // The model's `kUnspecified` enumerators have no spelling any of the M10B
  // `Parse*` helpers admits, so no identifier reaches this point carrying one
  // and there is no third case to refuse here. Absence is expressed by the
  // attribute being absent — which the enclosing declaration decides — never by
  // a spelling.
  template <typename T>
  StatusOr<T> TakeEnum(std::string_view what,
                       StatusOr<T> (*parse)(std::string_view)) {
    const Token& token = Peek();
    if (token.kind != TokenKind::kIdentifier) {
      std::string message = "expected ";
      message.append(what);
      message += ", found ";
      message += Describe(token);
      return FailAt(token, std::move(message));
    }
    StatusOr<T> parsed = parse(token.text);
    std::string message = "expected ";
    message.append(what);
    message += ", found \"";
    message.append(token.text);
    message += '\"';
    if (!parsed.ok()) {
      message += ", which is not a valid ";
      message.append(what);
      return FailAt(token, std::move(message));
    }
    Advance();
    return parsed;
  }

  // The spelling of `token` as a diagnostic names it. Defined in the
  // implementation with the rest of the message vocabulary.
  static std::string Describe(const Token& token);

  // --- Diagnostics ----------------------------------------------------------

  // Records a rejection at `token` and returns it as the `Status` to propagate.
  Status FailAt(const Token& token, std::string message);
  // `FailAt(Peek(), message)`.
  Status Fail(std::string message);

  // --- Productions ----------------------------------------------------------

  Status ParseEvidenceCase(EvidenceCase* out);
  Status ParseContext(ProgramBinding* out);
  Status ParseAnalyzerVersion(AnalyzerVersion* out);
  Status ParseEvidenceMember(EvidenceCase* out);

  Status ParseClaim(EvidenceCase* out);
  Status ParseEntity(EvidenceCase* out);
  Status ParseFact(EvidenceCase* out);
  Status ParseAssumption(EvidenceCase* out);
  Status ParseHypothesis(EvidenceCase* out);
  Status ParseUnknown(EvidenceCase* out);
  Status ParseEdge(EvidenceCase* out);
  Status ParsePath(EvidenceCase* out);
  Status ParseConstraint(EvidenceCase* out);
  Status ParseProvenance(EvidenceCase* out);
  Status ParseVerification(EvidenceCase* out);
  Status ParseSummary(EvidenceCase* out);
  Status ParseDependency(EvidenceCase* out);
  Status ParseOmission(EvidenceCase* out);

  // `Predicate ::= QuantifiedPredicate | ImplicationExpr`, and the precedence
  // chain the grammar factors out of it (§5.1).
  StatusOr<Expression> ParsePredicate();
  StatusOr<Expression> ParseImplication();
  StatusOr<Expression> ParseOr();
  StatusOr<Expression> ParseAnd();
  StatusOr<Expression> ParseComparison();
  StatusOr<Expression> ParseUnary();
  StatusOr<Expression> ParsePrimary();
  StatusOr<Expression> ParseQuantified(Expression::Kind kind);

  // `PropertyValue`, the value production of the open entity-property bag and
  // of every `FunctionCall` argument list.
  StatusOr<Expression> ParsePropertyValue();
  StatusOr<std::vector<Expression>> ParsePropertyValueList();
  StatusOr<std::vector<Expression>> ParsePredicateArgumentList();

  // `ResourceBudget ::= IntegerLiteral | FunctionCall`.
  StatusOr<Expression> ParseResourceBudget();

  // Re-reads an `IntegerLiteral` token under the `std::int64_t` bound the lexer
  // already applied, so the parser never depends on a property of its input
  // beyond the token contract, and lowers it into a `kInteger` expression.
  StatusOr<Expression> ParseIntegerToken(const Token& token);

  // `"[" Identifier { "," Identifier } "]"` for `using`, `components`, and the
  // backend lists.
  StatusOr<std::vector<std::string>> ParseIdentifierList(std::string_view what);

  // `ReferenceList ::= "[" [ Reference { "," Reference } ] "]"`, for the open
  // question's blocking facts. The `@` belongs to the syntax, so each entry
  // lowers to its bare case-local handle.
  StatusOr<std::vector<std::string>> ParseReferenceList(std::string_view what);

  // `FactReferenceList ::= "[" [ FactReference { "," FactReference } ] "]"`,
  // for a provenance record's inputs. `FactReference ::= "$" Identifier` is a
  // distinct spelling from `Reference`: a `$` names a case-local fact member,
  // and it lowers to the same bare handle.
  StatusOr<std::vector<std::string>> ParseFactReferenceList();

  // `FunctionCall | QualifiedId`, the shape `AssumptionSource` is. Both lower
  // into a `std::string` carrier — the call as its canonical EIR-T spelling,
  // the identifier as itself — and `attribute` is the attribute being read, for
  // the diagnostic.
  StatusOr<std::string> ParseCallOrQualifiedId(const Token& attribute,
                                               std::string_view what);

  // `Scope ::= "global" | "function" | "path" | "basic_block" | "callsite" |
  // "entity" | FunctionCall`. It lowers like `ParseCallOrQualifiedId`, but its
  // identifier-shaped half is that closed set rather than every `QualifiedId`.
  StatusOr<std::string> ParseScope(const Token& attribute, std::string_view what);

  // Reads one `{ attribute = value ";" ... }` body, refusing an attribute named
  // twice and one the production does not list. It owns the attribute names and
  // the value grammar; a caller dispatches on the name it returns. Defined in
  // the implementation.
  class AttributeReader;

  std::vector<Token> tokens_;
  EirParseError* error_;
  std::size_t cursor_ = 0;
  bool claim_declared_ = false;
};

}  // namespace veritas::evidence

#endif  // VERITAS_EVIDENCE_EIR_SYNTAX_H_
