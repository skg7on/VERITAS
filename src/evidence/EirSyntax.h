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
#include <string>
#include <string_view>
#include <vector>

#include "veritas/core/Status.h"
#include "veritas/evidence/EirText.h"

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

}  // namespace veritas::evidence

#endif  // VERITAS_EVIDENCE_EIR_SYNTAX_H_
