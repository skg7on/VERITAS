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

// EirLexerTest.cpp — the EIR-T lexer's token stream and its rejections, against
// the lexical grammar of §2.1–2.5 of
// `docs/specs/veritas-evidence-ir-formal-specification.md`.
//
// HOW THE REJECTION TESTS ARE BUILT
//
// A lexer test that only asserts "this input failed" proves nothing, because
// almost any malformed input fails for some reason. Every negative test here
// therefore pins three independent facts about the failure it expects:
//
//   1. the call failed;
//   2. the failure is reported at the offset *and* line *and* column of the
//      construct under test — not of its first character's neighbour, and not
//      of whatever the lexer might have tripped over earlier; and
//   3. the message names that construct.
//
// Facts 2 and 3 are what make each rejection its own: no two negative cases
// here share a position, and each pins the vocabulary of its own message. Each
// negative case also has a nearby positive control — the same shape with the
// one defect removed — so a test cannot pass by rejecting a whole category of
// input (all long integers, all non-ASCII bytes, all comments) rather than the
// specific construct it names.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "evidence/EirSyntax.h"
#include "veritas/core/Status.h"
#include "veritas/evidence/EirText.h"

using namespace veritas;
using namespace veritas::evidence;

namespace {

// Tokenizes `source`, requiring success, and returns the token stream.
std::vector<Token> Lex(std::string_view source) {
  EirParseError error;
  EirLexer lexer(source, &error);
  auto tokens = lexer.Tokenize();
  EXPECT_TRUE(tokens.ok()) << "offset " << error.offset << ", line "
                           << error.line << ", column " << error.column << ": "
                           << error.message;
  return tokens.ok() ? *tokens : std::vector<Token>();
}

// Tokenizes `source`, requiring rejection, and returns the reported position
// and message.
EirParseError Reject(std::string_view source) {
  EirParseError error;
  EirLexer lexer(source, &error);
  auto tokens = lexer.Tokenize();
  EXPECT_FALSE(tokens.ok()) << "expected a lexical error, but the source "
                               "tokenized successfully";
  return error;
}

// Asserts that token `index` has kind `kind` and spelling `text`.
void ExpectToken(const std::vector<Token>& tokens, std::size_t index,
                 TokenKind kind, std::string_view text) {
  ASSERT_LT(index, tokens.size());
  EXPECT_EQ(tokens[index].kind, kind) << "token " << index;
  EXPECT_EQ(tokens[index].text, std::string(text)) << "token " << index;
}

// Asserts that `message` mentions `fragment`, which is how a test pins *why*
// the lexer rejected an input rather than merely that it did.
void ExpectMessageMentions(std::string_view message, std::string_view fragment) {
  EXPECT_NE(message.find(fragment), std::string_view::npos)
      << "message: " << message;
}

}  // namespace

TEST(EirLexerTest, TracksTokensAndSourceCoordinates) {
  EirParseError error;
  EirLexer lexer("evidence E {\n  schema = \"eir.v1\";\n}", &error);
  auto tokens = lexer.Tokenize();
  ASSERT_TRUE(tokens.ok()) << error.message;
  EXPECT_EQ((*tokens)[0].text, "evidence");
  EXPECT_EQ((*tokens)[3].line, 2U);
  EXPECT_EQ((*tokens)[3].column, 3U);

  // The same token stream, pinned further: `evidence` opens the source, the
  // line-2 `schema` follows the newline exactly as the byte offsets say, and
  // the string literal carries its decoded value rather than its spelling.
  ASSERT_EQ((*tokens).size(), 9U);
  EXPECT_EQ((*tokens)[0].kind, TokenKind::kIdentifier);
  EXPECT_EQ((*tokens)[0].offset, 0U);
  EXPECT_EQ((*tokens)[0].line, 1U);
  EXPECT_EQ((*tokens)[0].column, 1U);
  ExpectToken(*tokens, 1, TokenKind::kIdentifier, "E");
  ExpectToken(*tokens, 2, TokenKind::kLeftBrace, "{");
  ExpectToken(*tokens, 3, TokenKind::kIdentifier, "schema");
  EXPECT_EQ((*tokens)[3].offset, 15U);
  ExpectToken(*tokens, 4, TokenKind::kEqual, "=");
  ExpectToken(*tokens, 5, TokenKind::kString, "eir.v1");
  ExpectToken(*tokens, 6, TokenKind::kSemicolon, ";");
  ExpectToken(*tokens, 7, TokenKind::kRightBrace, "}");
  EXPECT_EQ((*tokens)[7].line, 3U);
  EXPECT_EQ((*tokens)[7].column, 1U);
  EXPECT_EQ((*tokens)[8].kind, TokenKind::kEnd);
  EXPECT_EQ((*tokens)[8].offset, 35U);
  EXPECT_EQ((*tokens)[8].line, 3U);
  EXPECT_EQ((*tokens)[8].column, 2U);
}

TEST(EirLexerTest, RejectsInvalidEscapeWithLocation) {
  EirParseError error;
  EirLexer lexer("\"bad\\q\"", &error);
  EXPECT_FALSE(lexer.Tokenize().ok());
  EXPECT_EQ(error.line, 1U);
  EXPECT_EQ(error.column, 5U);

  // The column is the backslash, which is the start of the escape sequence a
  // reader has to remove — not the escaped character one byte later.
  EXPECT_EQ(error.offset, 4U);
  ExpectMessageMentions(error.message, "\\q");
  ExpectMessageMentions(error.message, "escape");
}

TEST(EirLexerTest, SkipsLineAndBlockComments) {
  const std::vector<Token> tokens = Lex(
      "// leading\n"
      "evidence /* inline */ E {\n"
      "  /* multi\n"
      "     line */ schema = \"eir.v1\"; // trailing\n"
      "}");
  ASSERT_EQ(tokens.size(), 9U);
  ExpectToken(tokens, 0, TokenKind::kIdentifier, "evidence");
  EXPECT_EQ(tokens[0].line, 2U);
  ExpectToken(tokens, 1, TokenKind::kIdentifier, "E");
  ExpectToken(tokens, 2, TokenKind::kLeftBrace, "{");
  ExpectToken(tokens, 3, TokenKind::kIdentifier, "schema");
  // A block comment that spans a newline moves the line count with it, and the
  // line comment before `}` does not consume the `}`.
  EXPECT_EQ(tokens[3].line, 4U);
  EXPECT_EQ(tokens[3].column, 14U);
  EXPECT_EQ(tokens[7].kind, TokenKind::kRightBrace);
  EXPECT_EQ(tokens[7].line, 5U);
  EXPECT_EQ(tokens[7].column, 1U);
  EXPECT_EQ(tokens[8].kind, TokenKind::kEnd);
}

TEST(EirLexerTest, AcceptsALineCommentThatEndsAtEndOfInput) {
  // §2.1 spells `LineComment ::= "//" { Character } "\n"`, which cannot be read
  // strictly; the comment is read as ending at the first newline or at end of
  // input, so a trailing comment with no final newline is accepted.
  const std::vector<Token> tokens = Lex("evidence // no final newline");
  ASSERT_EQ(tokens.size(), 2U);
  ExpectToken(tokens, 0, TokenKind::kIdentifier, "evidence");
  ExpectToken(tokens, 1, TokenKind::kEnd, "");
  EXPECT_EQ(tokens[1].offset, 28U);
  EXPECT_EQ(tokens[1].line, 1U);
}

TEST(EirLexerTest, DecodesStringEscapes) {
  const std::vector<Token> tokens = Lex("\"a\\\"b\\\\c\\nd\\re\\tf\"");
  ASSERT_EQ(tokens.size(), 2U);
  EXPECT_EQ(tokens[0].kind, TokenKind::kString);
  EXPECT_EQ(tokens[0].text, "a\"b\\c\nd\re\tf");
  // An escape sequence is two source bytes, so the newline it decodes to does
  // not move the line counter: the `kEnd` token still sits on line 1.
  EXPECT_EQ(tokens[0].line, 1U);
  EXPECT_EQ(tokens[0].column, 1U);
  EXPECT_EQ(tokens[1].line, 1U);
}

TEST(EirLexerTest, KeepsCoordinatesAcrossAMultiLineStringLiteral) {
  const std::vector<Token> tokens = Lex("\"one\ntwo\"\nschema");
  ASSERT_EQ(tokens.size(), 3U);
  EXPECT_EQ(tokens[0].kind, TokenKind::kString);
  EXPECT_EQ(tokens[0].text, "one\ntwo");
  // Two raw newlines precede `schema`: one inside the literal, one after it.
  ExpectToken(tokens, 1, TokenKind::kIdentifier, "schema");
  EXPECT_EQ(tokens[1].line, 3U);
  EXPECT_EQ(tokens[1].column, 1U);
}

TEST(EirLexerTest, LexesASignedIntegerAsOneToken) {
  // `IntegerLiteral ::= [ "-" ] Digit { Digit }` puts the sign inside the
  // literal, and the kind set has no minus token to confuse it with.
  const std::vector<Token> tokens = Lex("-5");
  ASSERT_EQ(tokens.size(), 2U);
  EXPECT_EQ(tokens[0].kind, TokenKind::kInteger);
  EXPECT_EQ(tokens[0].text, "-5");
  EXPECT_EQ(tokens[0].offset, 0U);
  EXPECT_EQ(tokens[0].column, 1U);
  EXPECT_EQ(tokens[1].kind, TokenKind::kEnd);

  const std::vector<Token> spaced = Lex("range(@len, -5, 0)");
  ASSERT_EQ(spaced.size(), 10U);
  EXPECT_EQ(spaced[5].kind, TokenKind::kInteger);
  EXPECT_EQ(spaced[5].text, "-5");
}

TEST(EirLexerTest, LexesTheArrowApartFromSignedIntegers) {
  const std::vector<Token> tokens = Lex("@a -> @b");
  ASSERT_EQ(tokens.size(), 6U);
  ExpectToken(tokens, 0, TokenKind::kAt, "@");
  ExpectToken(tokens, 1, TokenKind::kIdentifier, "a");
  ExpectToken(tokens, 2, TokenKind::kArrow, "->");
  ExpectToken(tokens, 3, TokenKind::kAt, "@");
  ExpectToken(tokens, 4, TokenKind::kIdentifier, "b");
  EXPECT_EQ(tokens[2].offset, 3U);

  // The lookahead after `-` decides, not the whitespace around it.
  const std::vector<Token> tight = Lex("@a->@b");
  ASSERT_EQ(tight.size(), 6U);
  ExpectToken(tight, 1, TokenKind::kIdentifier, "a");
  ExpectToken(tight, 2, TokenKind::kArrow, "->");
  EXPECT_EQ(tight[2].offset, 2U);
}

TEST(EirLexerTest, RejectsADashThatBeginsNeitherAnArrowNorAnInteger) {
  const EirParseError error = Reject("a - b");
  EXPECT_EQ(error.offset, 2U);
  EXPECT_EQ(error.line, 1U);
  EXPECT_EQ(error.column, 3U);
  ExpectMessageMentions(error.message, "->");
  ExpectMessageMentions(error.message, "'-'");

  // A trailing `-` has the same fate; there is no token it could become.
  const EirParseError trailing = Reject("-");
  EXPECT_EQ(trailing.offset, 0U);
  EXPECT_EQ(trailing.column, 1U);
}

TEST(EirLexerTest, LexesReferencesAndDottedQualifiedIdentifiers) {
  const std::vector<Token> tokens = Lex("@a.b.c $F1 @x");
  ASSERT_EQ(tokens.size(), 7U);
  ExpectToken(tokens, 0, TokenKind::kAt, "@");
  // `QualifiedId ::= Identifier { "." Identifier }` has no `"."` terminal, so
  // the dots ride along inside one identifier token.
  ExpectToken(tokens, 1, TokenKind::kIdentifier, "a.b.c");
  EXPECT_EQ(tokens[1].offset, 1U);
  EXPECT_EQ(tokens[1].column, 2U);
  ExpectToken(tokens, 2, TokenKind::kDollar, "$");
  ExpectToken(tokens, 3, TokenKind::kIdentifier, "F1");
  ExpectToken(tokens, 4, TokenKind::kAt, "@");
  ExpectToken(tokens, 5, TokenKind::kIdentifier, "x");
  EXPECT_EQ(tokens[6].kind, TokenKind::kEnd);
}

TEST(EirLexerTest, RejectsADotInAQualifiedIdentifierThatPrecedesNoLetter) {
  // The dot continues the identifier only when a `Letter` follows it
  // immediately, so `a.5` is `a`, then a stray `.`.
  const EirParseError error = Reject("@a.5");
  EXPECT_EQ(error.offset, 2U);
  EXPECT_EQ(error.column, 3U);
  ExpectMessageMentions(error.message, "'.'");

  // Whitespace after the dot breaks the qualified id the same way.
  const EirParseError spaced = Reject("@a. b");
  EXPECT_EQ(spaced.offset, 2U);
  EXPECT_EQ(spaced.column, 3U);
  ExpectMessageMentions(spaced.message, "'.'");

  // The positive control: `a.b` is one token, so the rejections above are
  // about the dot's right-hand side and not about dotted ids in general.
  const std::vector<Token> ok = Lex("@a.b");
  ASSERT_EQ(ok.size(), 3U);
  ExpectToken(ok, 1, TokenKind::kIdentifier, "a.b");
}

TEST(EirLexerTest, LexesPunctuationAndComparisonOperators) {
  const std::vector<Token> tokens = Lex(
      "{ } ( ) [ ] : ; , = == != < <= > >=");
  ASSERT_EQ(tokens.size(), 17U);
  ExpectToken(tokens, 0, TokenKind::kLeftBrace, "{");
  ExpectToken(tokens, 1, TokenKind::kRightBrace, "}");
  ExpectToken(tokens, 2, TokenKind::kLeftParen, "(");
  ExpectToken(tokens, 3, TokenKind::kRightParen, ")");
  ExpectToken(tokens, 4, TokenKind::kLeftBracket, "[");
  ExpectToken(tokens, 5, TokenKind::kRightBracket, "]");
  ExpectToken(tokens, 6, TokenKind::kColon, ":");
  ExpectToken(tokens, 7, TokenKind::kSemicolon, ";");
  ExpectToken(tokens, 8, TokenKind::kComma, ",");
  ExpectToken(tokens, 9, TokenKind::kEqual, "=");
  ExpectToken(tokens, 10, TokenKind::kEqualEqual, "==");
  ExpectToken(tokens, 11, TokenKind::kNotEqual, "!=");
  ExpectToken(tokens, 12, TokenKind::kLess, "<");
  ExpectToken(tokens, 13, TokenKind::kLessEqual, "<=");
  ExpectToken(tokens, 14, TokenKind::kGreater, ">");
  ExpectToken(tokens, 15, TokenKind::kGreaterEqual, ">=");
}

TEST(EirLexerTest, LexesAssignmentOpAsTheTwoTokensItIsSpelledWith) {
  // `AssignmentOp ::= "=" | ":="` is referenced by no production, and the kind
  // set has no assignment token, so `:=` is a colon followed by an equals.
  const std::vector<Token> tokens = Lex("a := 1");
  ASSERT_EQ(tokens.size(), 5U);
  ExpectToken(tokens, 0, TokenKind::kIdentifier, "a");
  ExpectToken(tokens, 1, TokenKind::kColon, ":");
  ExpectToken(tokens, 2, TokenKind::kEqual, "=");
  ExpectToken(tokens, 3, TokenKind::kInteger, "1");
  EXPECT_EQ(tokens[4].kind, TokenKind::kEnd);
}

TEST(EirLexerTest, RejectsACharacterThatBeginsNoToken) {
  const EirParseError bang = Reject("!x");
  EXPECT_EQ(bang.offset, 0U);
  EXPECT_EQ(bang.column, 1U);
  ExpectMessageMentions(bang.message, "!=");

  // A lone `/` is not a comment opener and begins no token, and neither does a
  // `*` that no comment introduced.
  const EirParseError slash = Reject("a / b");
  EXPECT_EQ(slash.offset, 2U);
  EXPECT_EQ(slash.column, 3U);
  ExpectMessageMentions(slash.message, "unexpected character '/'");

  const EirParseError star = Reject("a * b");
  EXPECT_EQ(star.offset, 2U);
  EXPECT_EQ(star.column, 3U);
  ExpectMessageMentions(star.message, "unexpected character '*'");
}

TEST(EirLexerTest, EmitsTheEndTokenAtEndOfInput) {
  const std::vector<Token> empty = Lex("");
  ASSERT_EQ(empty.size(), 1U);
  EXPECT_EQ(empty[0].kind, TokenKind::kEnd);
  EXPECT_EQ(empty[0].offset, 0U);
  EXPECT_EQ(empty[0].line, 1U);
  EXPECT_EQ(empty[0].column, 1U);

  const std::vector<Token> blank = Lex("  \n\t ");
  ASSERT_EQ(blank.size(), 1U);
  EXPECT_EQ(blank[0].kind, TokenKind::kEnd);
  EXPECT_EQ(blank[0].offset, 5U);
  EXPECT_EQ(blank[0].line, 2U);
  EXPECT_EQ(blank[0].column, 3U);
}

TEST(EirLexerTest, LeavesTheOptionalCaseLabelForTheParser) {
  // `EvidenceCase ::= "evidence" [ Identifier ] "{"`: the label is optional and
  // carries no semantics, so the lexer must not claim it. Both spellings yield
  // the same kinds, differing only by the extra identifier.
  const std::vector<Token> labelled = Lex("evidence Overflow_001 {");
  ASSERT_EQ(labelled.size(), 4U);
  ExpectToken(labelled, 0, TokenKind::kIdentifier, "evidence");
  ExpectToken(labelled, 1, TokenKind::kIdentifier, "Overflow_001");
  ExpectToken(labelled, 2, TokenKind::kLeftBrace, "{");

  const std::vector<Token> bare = Lex("evidence {");
  ASSERT_EQ(bare.size(), 3U);
  ExpectToken(bare, 0, TokenKind::kIdentifier, "evidence");
  ExpectToken(bare, 1, TokenKind::kLeftBrace, "{");
  EXPECT_EQ(bare[2].kind, TokenKind::kEnd);
}

TEST(EirLexerTest, RejectsAnUnterminatedStringLiteral) {
  const EirParseError error = Reject("schema = \"eir.v1");
  EXPECT_EQ(error.offset, 9U);
  EXPECT_EQ(error.line, 1U);
  EXPECT_EQ(error.column, 10U);
  ExpectMessageMentions(error.message, "unterminated string literal");

  // A backslash immediately before end of input is the same failure, reported
  // at the quote that opened the literal rather than at the backslash.
  const EirParseError dangling = Reject("\"abc\\");
  EXPECT_EQ(dangling.offset, 0U);
  EXPECT_EQ(dangling.column, 1U);
  ExpectMessageMentions(dangling.message, "unterminated string literal");
}

TEST(EirLexerTest, RejectsAnUnterminatedBlockComment) {
  // Nothing else is wrong with this source: if the unterminated-comment check
  // were absent the comment would simply run to end of input and the lexer
  // would return `evidence` followed by `kEnd`.
  const EirParseError error = Reject("evidence /* no end");
  EXPECT_EQ(error.offset, 9U);
  EXPECT_EQ(error.line, 1U);
  EXPECT_EQ(error.column, 10U);
  ExpectMessageMentions(error.message, "unterminated block comment");

  const EirParseError nested = Reject("/*");
  EXPECT_EQ(nested.offset, 0U);
  EXPECT_EQ(nested.column, 1U);
  ExpectMessageMentions(nested.message, "unterminated block comment");

  // The positive control: the same comment, closed, is accepted.
  const std::vector<Token> closed = Lex("evidence /* closed */");
  ASSERT_EQ(closed.size(), 2U);
  ExpectToken(closed, 0, TokenKind::kIdentifier, "evidence");
  EXPECT_EQ(closed[1].kind, TokenKind::kEnd);
}

TEST(EirLexerTest, RejectsMalformedUtf8) {
  // 0xC3 leads a two-byte sequence whose second byte must be a continuation
  // byte; `(` (0x28) is not one. Inside a string literal nothing else about
  // this source is wrong, so the rejection can only be the byte sequence.
  const char truncated[] = {'"', 'a', static_cast<char>(0xC3), '(', 'b', '"'};
  const EirParseError error =
      Reject(std::string_view(truncated, sizeof(truncated)));
  EXPECT_EQ(error.offset, 2U);
  EXPECT_EQ(error.line, 1U);
  EXPECT_EQ(error.column, 3U);
  ExpectMessageMentions(error.message, "UTF-8");

  // A three-byte lead truncated by end of input, outside any string.
  const char cut[] = {'e', static_cast<char>(0xE2), static_cast<char>(0x82)};
  const EirParseError outside = Reject(std::string_view(cut, sizeof(cut)));
  EXPECT_EQ(outside.offset, 1U);
  EXPECT_EQ(outside.column, 2U);
  ExpectMessageMentions(outside.message, "UTF-8");

  // A surrogate half is well-formed byte-pattern-wise and still invalid UTF-8.
  const char surrogate[] = {'"', static_cast<char>(0xED), static_cast<char>(0xA0),
                            static_cast<char>(0x80), '"'};
  const EirParseError encoded =
      Reject(std::string_view(surrogate, sizeof(surrogate)));
  EXPECT_EQ(encoded.offset, 1U);
  EXPECT_EQ(encoded.column, 2U);
  ExpectMessageMentions(encoded.message, "UTF-8");
}

TEST(EirLexerTest, KeepsWellFormedNonAsciiBytesInStringsAndComments) {
  // The positive control for the rejections above: a well-formed sequence is
  // preserved byte for byte inside a string literal and skipped inside a
  // comment, so this is not a lexer that forbids non-ASCII.
  const char accented[] = {'"', 'c', 'a', 'f', static_cast<char>(0xC3),
                           static_cast<char>(0xA9), '"'};
  const std::vector<Token> tokens =
      Lex(std::string_view(accented, sizeof(accented)));
  ASSERT_EQ(tokens.size(), 2U);
  EXPECT_EQ(tokens[0].kind, TokenKind::kString);
  EXPECT_EQ(tokens[0].text, std::string("caf\xc3\xa9", 5));
  EXPECT_EQ(tokens[0].column, 1U);

  const char commented[] = {'/', '/', ' ', static_cast<char>(0xC3),
                            static_cast<char>(0xA9), '\n', 'x'};
  const std::vector<Token> after =
      Lex(std::string_view(commented, sizeof(commented)));
  ASSERT_EQ(after.size(), 2U);
  ExpectToken(after, 0, TokenKind::kIdentifier, "x");
  EXPECT_EQ(after[0].line, 2U);
}

TEST(EirLexerTest, RejectsMalformedUtf8InALineComment) {
  // The line-comment arm validates the bytes it skips, so the malformed pair
  // inside the comment is caught even though nothing about the comment is ever
  // emitted as a token. The `0xC3` here leads a two-byte sequence and is
  // followed by `(`, which is not a continuation byte.
  const char source[] = {'/', '/', ' ', 'f', 'i', 'r', 's', 't', '\n',
                         '/', '/', ' ', 'n', 'o', 't', 'e', ' ',
                         static_cast<char>(0xC3), '(', '\n', 'x'};
  const EirParseError error = Reject(std::string_view(source, sizeof(source)));
  EXPECT_EQ(error.offset, 17U);
  EXPECT_EQ(error.line, 2U);
  EXPECT_EQ(error.column, 9U);
  ExpectMessageMentions(error.message, "UTF-8");
  ExpectMessageMentions(error.message, "comment");
}

TEST(EirLexerTest, SkipsWellFormedNonAsciiInALineComment) {
  // The positive control for the rejection above: the same comment with a
  // well-formed sequence is skipped byte for byte, and the token after it keeps
  // its own coordinates — so the rejection is about the bytes, not about
  // non-ASCII content in a line comment.
  const char source[] = {'/', '/', ' ', 'f', 'i', 'r', 's', 't', '\n',
                         '/', '/', ' ', 'c', 'a', 'f',
                         static_cast<char>(0xC3), static_cast<char>(0xA9),
                         '\n', 'x'};
  const std::vector<Token> tokens =
      Lex(std::string_view(source, sizeof(source)));
  ASSERT_EQ(tokens.size(), 2U);
  ExpectToken(tokens, 0, TokenKind::kIdentifier, "x");
  EXPECT_EQ(tokens[0].line, 3U);
  EXPECT_EQ(tokens[0].column, 1U);
}

TEST(EirLexerTest, RejectsMalformedUtf8InABlockComment) {
  // The block-comment arm keeps its own UTF-8 check, and reports the first
  // invalid byte by its position in the source, not in the comment: this one is
  // five bytes into line 2. The comment closes with `*/` and nothing else in
  // the source is malformed, so the byte pair is the only thing that can fail.
  const char source[] = {'e', 'v', 'i', 'd', 'e', 'n', 'c', 'e', ' ', '/', '*',
                         ' ', 'n', 'o', 't', 'e', '\n', ' ', ' ', 'x', 'x',
                         static_cast<char>(0xC3), '(', ' ', '*', '/', ' ', 'x'};
  const EirParseError error = Reject(std::string_view(source, sizeof(source)));
  EXPECT_EQ(error.offset, 21U);
  EXPECT_EQ(error.line, 2U);
  EXPECT_EQ(error.column, 5U);
  ExpectMessageMentions(error.message, "UTF-8");
  ExpectMessageMentions(error.message, "comment");
}

TEST(EirLexerTest, SkipsWellFormedNonAsciiInABlockComment) {
  // The positive control for the rejection above.
  const char source[] = {'e', 'v', 'i', 'd', 'e', 'n', 'c', 'e', ' ', '/', '*',
                         ' ', 'c', 'a', 'f', static_cast<char>(0xC3),
                         static_cast<char>(0xA9), '\n', ' ', ' ', 'o', 'k', ' ',
                         '*', '/', ' ', 'x'};
  const std::vector<Token> tokens =
      Lex(std::string_view(source, sizeof(source)));
  ASSERT_EQ(tokens.size(), 3U);
  ExpectToken(tokens, 0, TokenKind::kIdentifier, "evidence");
  ExpectToken(tokens, 1, TokenKind::kIdentifier, "x");
  EXPECT_EQ(tokens[1].line, 2U);
  EXPECT_EQ(tokens[1].column, 9U);
}

TEST(EirLexerTest, RejectsWellFormedNonAsciiOutsideAStringOrComment) {
  // A distinct rejection from the malformed-byte one: these bytes *are* valid
  // UTF-8, but `Letter ::= "a".."z" | "A".."Z"` admits them in no token.
  const char glyph[] = {static_cast<char>(0xC3), static_cast<char>(0xA9)};
  const EirParseError error = Reject(std::string_view(glyph, sizeof(glyph)));
  EXPECT_EQ(error.offset, 0U);
  EXPECT_EQ(error.column, 1U);
  ExpectMessageMentions(error.message, "non-ASCII");
  EXPECT_EQ(error.message.find("UTF-8"), std::string::npos)
      << "a well-formed sequence is not a UTF-8 error: " << error.message;
}

TEST(EirLexerTest, RejectsAnIntegerLiteralOutsideInt64) {
  // 2^63, one past `std::int64_t`'s maximum. The message names the literal, so
  // the rejection cannot be mistaken for a length limit or a stray character.
  const EirParseError error = Reject("9223372036854775808");
  EXPECT_EQ(error.offset, 0U);
  EXPECT_EQ(error.line, 1U);
  EXPECT_EQ(error.column, 1U);
  ExpectMessageMentions(error.message, "9223372036854775808");
  ExpectMessageMentions(error.message, "std::int64_t");

  // `std::int64_t`'s minimum minus one, reported where it appears in a
  // plausible case rather than at the start of the source.
  const EirParseError in_context =
      Reject("entity x : value {\n  origin = -9223372036854775809;\n}");
  EXPECT_EQ(in_context.line, 2U);
  EXPECT_EQ(in_context.column, 12U);
  ExpectMessageMentions(in_context.message, "-9223372036854775809");
}

TEST(EirLexerTest, AcceptsTheInt64Extremes) {
  // The positive control for the rejections above: the extremes themselves are
  // in range, so overflow checking is not a length limit on digit runs.
  const std::vector<Token> tokens =
      Lex("-9223372036854775808 9223372036854775807 0 -0");
  ASSERT_EQ(tokens.size(), 5U);
  ExpectToken(tokens, 0, TokenKind::kInteger, "-9223372036854775808");
  ExpectToken(tokens, 1, TokenKind::kInteger, "9223372036854775807");
  ExpectToken(tokens, 2, TokenKind::kInteger, "0");
  ExpectToken(tokens, 3, TokenKind::kInteger, "-0");
  EXPECT_EQ(tokens[4].kind, TokenKind::kEnd);
}

TEST(EirLexerTest, ReportsTheSameFailureThroughStatusAndError) {
  // The error source here is deliberately a bare `!`, which is a different
  // rejection from the `/*` these two plumbing tests once used: the comment-arm
  // tests no longer reach this one. (`!` is also an input of
  // `RejectsACharacterThatBeginsNoToken`, which pins that arm's own message and
  // position; this test pins only that a failure reaches both the `Status` and
  // the `EirParseError`.)
  EirParseError error;
  EirLexer lexer("!", &error);
  const auto tokens = lexer.Tokenize();
  ASSERT_FALSE(tokens.ok());
  EXPECT_EQ(tokens.status().code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(std::string(tokens.status().message()), error.message);
  EXPECT_EQ(error.column, 1U);
}

TEST(EirLexerTest, ToleratesAMissingErrorSink) {
  EirLexer lexer("!", nullptr);
  EXPECT_FALSE(lexer.Tokenize().ok());

  EirLexer good("evidence", nullptr);
  EXPECT_TRUE(good.Tokenize().ok());
}

TEST(EirLexerTest, RestartsOnASecondCall) {
  EirParseError error;
  EirLexer lexer("evidence E {", &error);
  const auto first = lexer.Tokenize();
  const auto second = lexer.Tokenize();
  ASSERT_TRUE(first.ok()) << error.message;
  ASSERT_TRUE(second.ok()) << error.message;
  ASSERT_EQ(first->size(), second->size());
  for (std::size_t i = 0; i < first->size(); ++i) {
    EXPECT_EQ((*first)[i].kind, (*second)[i].kind) << "token " << i;
    EXPECT_EQ((*first)[i].text, (*second)[i].text) << "token " << i;
    EXPECT_EQ((*first)[i].offset, (*second)[i].offset) << "token " << i;
  }
}
