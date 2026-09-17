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

// EirLexer.cpp — EIR-T tokenization. See `EirSyntax.h` for the token model, the
// three grammar rulings it encodes, and the failure contract.

#include "evidence/EirSyntax.h"

#include <charconv>
#include <cstdint>
#include <system_error>
#include <utility>

namespace veritas::evidence {
namespace {

bool IsLetter(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

bool IsContinuationByte(unsigned char byte) {
  return byte >= 0x80 && byte <= 0xBF;
}

// The length in bytes of the well-formed UTF-8 sequence beginning at
// `source[pos]`, or 0 when the bytes there are not well-formed UTF-8. ASCII is
// length 1. The rejection rules beyond well-formedness are RFC 3629's: no
// overlong encoding, no surrogate half (`U+D800`..`U+DFFF`), and nothing above
// `U+10FFFF`.
//
// `pos` must be a valid index into `source`.
std::size_t Utf8SequenceLengthAt(std::string_view source, std::size_t pos) {
  const auto lead = static_cast<unsigned char>(source[pos]);
  if (lead < 0x80) {
    return 1;
  }
  if (lead >= 0xC2 && lead <= 0xDF) {
    // 0xC0 and 0xC1 would only ever lead an overlong two-byte sequence.
    if (pos + 1 >= source.size()) {
      return 0;
    }
    return IsContinuationByte(static_cast<unsigned char>(source[pos + 1])) ? 2
                                                                          : 0;
  }
  if (lead >= 0xE0 && lead <= 0xEF) {
    if (pos + 2 >= source.size()) {
      return 0;
    }
    const auto second = static_cast<unsigned char>(source[pos + 1]);
    const auto third = static_cast<unsigned char>(source[pos + 2]);
    if (!IsContinuationByte(second) || !IsContinuationByte(third)) {
      return 0;
    }
    if (lead == 0xE0 && second < 0xA0) {
      return 0;  // overlong: U+0800 and below
    }
    if (lead == 0xED && second >= 0xA0) {
      return 0;  // surrogate half: U+D800..U+DFFF
    }
    return 3;
  }
  if (lead >= 0xF0 && lead <= 0xF4) {
    if (pos + 3 >= source.size()) {
      return 0;
    }
    const auto second = static_cast<unsigned char>(source[pos + 1]);
    const auto third = static_cast<unsigned char>(source[pos + 2]);
    const auto fourth = static_cast<unsigned char>(source[pos + 3]);
    if (!IsContinuationByte(second) || !IsContinuationByte(third) ||
        !IsContinuationByte(fourth)) {
      return 0;
    }
    if (lead == 0xF0 && second < 0x90) {
      return 0;  // overlong: U+10000 and below
    }
    if (lead == 0xF4 && second > 0x8F) {
      return 0;  // above U+10FFFF
    }
    return 4;
  }
  // 0x80..0xC1 is a stray continuation byte or an overlong lead, and
  // 0xF5..0xFF leads nothing that UTF-8 defines.
  return 0;
}

}  // namespace

EirLexer::EirLexer(std::string_view source, EirParseError* error)
    : source_(source), error_(error) {}

char EirLexer::Peek(std::size_t lookahead) const {
  const std::size_t index = cursor_ + lookahead;
  return index < source_.size() ? source_[index] : '\0';
}

char EirLexer::Advance() {
  if (AtEnd()) {
    return '\0';
  }
  const char c = source_[cursor_++];
  if (c == '\n') {
    ++line_;
    line_start_ = cursor_;
  }
  return c;
}

Status EirLexer::Fail(std::size_t offset, std::size_t line, std::size_t column,
                      std::string message) {
  if (error_ != nullptr) {
    error_->offset = offset;
    error_->line = line;
    error_->column = column;
    error_->message = message;
  }
  return Status::InvalidArgument(std::move(message));
}

Status EirLexer::Emit(TokenKind kind, std::vector<Token>* out) {
  const std::size_t start = cursor_;
  const std::size_t start_line = line_;
  const std::size_t start_column = Column();
  std::string text(1, Advance());
  out->push_back(Token{kind, std::move(text), start, start_line, start_column});
  return Status::Ok();
}

Status EirLexer::EmitPair(TokenKind kind, std::vector<Token>* out) {
  const std::size_t start = cursor_;
  const std::size_t start_line = line_;
  const std::size_t start_column = Column();
  std::string text(1, Advance());
  text.push_back(Advance());
  out->push_back(Token{kind, std::move(text), start, start_line, start_column});
  return Status::Ok();
}

Status EirLexer::SkipWhitespaceAndComments() {
  for (;;) {
    if (AtEnd()) {
      return Status::Ok();
    }
    const char c = Peek();
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      Advance();
      continue;
    }
    if (c != '/') {
      return Status::Ok();
    }
    if (Peek(1) == '/') {
      Advance();
      Advance();
      // A line comment ends at the first newline (which the whitespace arm
      // above then consumes, keeping the line count in step) or, leniently, at
      // end of input.
      while (!AtEnd() && Peek() != '\n') {
        const std::size_t length = Utf8SequenceLengthAt(source_, cursor_);
        if (length == 0) {
          return Fail(cursor_, line_, Column(),
                      "invalid UTF-8 sequence in comment");
        }
        for (std::size_t i = 0; i < length; ++i) {
          Advance();
        }
      }
      continue;
    }
    if (Peek(1) == '*') {
      const std::size_t start = cursor_;
      const std::size_t start_line = line_;
      const std::size_t start_column = Column();
      Advance();
      Advance();
      for (;;) {
        if (AtEnd()) {
          return Fail(start, start_line, start_column,
                      "unterminated block comment");
        }
        if (Peek() == '*' && Peek(1) == '/') {
          Advance();
          Advance();
          break;
        }
        const std::size_t length = Utf8SequenceLengthAt(source_, cursor_);
        if (length == 0) {
          return Fail(cursor_, line_, Column(),
                      "invalid UTF-8 sequence in comment");
        }
        for (std::size_t i = 0; i < length; ++i) {
          Advance();
        }
      }
      continue;
    }
    // A lone '/' begins no token; the dispatch below rejects it with the same
    // message it gives every other character that starts nothing.
    return Status::Ok();
  }
}

Status EirLexer::LexIdentifier(std::vector<Token>* out) {
  const std::size_t start = cursor_;
  const std::size_t start_line = line_;
  const std::size_t start_column = Column();
  while (!AtEnd() && (IsLetter(Peek()) || IsDigit(Peek()) || Peek() == '_')) {
    Advance();
  }
  // `QualifiedId ::= Identifier { "." Identifier }`: a dot extends the same
  // token, but only when a letter follows it immediately. `a.b.` is therefore
  // `a.b` followed by a stray dot, not an identifier with a trailing dot.
  while (Peek() == '.' && IsLetter(Peek(1))) {
    Advance();
    while (!AtEnd() &&
           (IsLetter(Peek()) || IsDigit(Peek()) || Peek() == '_')) {
      Advance();
    }
  }
  const std::size_t length = cursor_ - start;
  out->push_back(Token{TokenKind::kIdentifier,
                       std::string(source_.substr(start, length)), start,
                       start_line, start_column});
  return Status::Ok();
}

Status EirLexer::LexString(std::vector<Token>* out) {
  const std::size_t start = cursor_;
  const std::size_t start_line = line_;
  const std::size_t start_column = Column();
  Advance();  // the opening quote

  std::string decoded;
  for (;;) {
    if (AtEnd()) {
      return Fail(start, start_line, start_column,
                  "unterminated string literal");
    }
    const char c = Peek();
    if (c == '"') {
      Advance();
      break;
    }
    if (c == '\\') {
      const std::size_t escape_offset = cursor_;
      const std::size_t escape_line = line_;
      const std::size_t escape_column = Column();
      Advance();  // the backslash
      if (AtEnd()) {
        return Fail(start, start_line, start_column,
                    "unterminated string literal");
      }
      const char escaped = Advance();
      switch (escaped) {
        case '"':
          decoded.push_back('"');
          break;
        case '\\':
          decoded.push_back('\\');
          break;
        case 'n':
          decoded.push_back('\n');
          break;
        case 'r':
          decoded.push_back('\r');
          break;
        case 't':
          decoded.push_back('\t');
          break;
        default:
          // The escape is rejected at its backslash, which is where a reader
          // has to start deleting.
          return Fail(escape_offset, escape_line, escape_column,
                      std::string("invalid escape sequence \"\\") + escaped +
                          "\" in string literal");
      }
      continue;
    }
    const std::size_t length = Utf8SequenceLengthAt(source_, cursor_);
    if (length == 0) {
      return Fail(cursor_, line_, Column(),
                  "invalid UTF-8 sequence in string literal");
    }
    for (std::size_t i = 0; i < length; ++i) {
      decoded.push_back(Advance());
    }
  }

  out->push_back(Token{TokenKind::kString, std::move(decoded), start,
                       start_line, start_column});
  return Status::Ok();
}

Status EirLexer::LexInteger(std::vector<Token>* out) {
  const std::size_t start = cursor_;
  const std::size_t start_line = line_;
  const std::size_t start_column = Column();
  if (Peek() == '-') {
    Advance();
  }
  while (IsDigit(Peek())) {
    Advance();
  }

  const std::string_view spelling = source_.substr(start, cursor_ - start);
  const char* const first = spelling.data();
  const char* const last = first + spelling.size();
  std::int64_t value = 0;
  const std::from_chars_result result = std::from_chars(first, last, value);
  if (result.ec != std::errc{} || result.ptr != last) {
    return Fail(start, start_line, start_column,
                "integer literal \"" + std::string(spelling) +
                    "\" is not representable as a std::int64_t");
  }

  out->push_back(Token{TokenKind::kInteger, std::string(spelling), start,
                       start_line, start_column});
  return Status::Ok();
}

Status EirLexer::LexToken(std::vector<Token>* out) {
  const std::size_t start = cursor_;
  const std::size_t start_line = line_;
  const std::size_t start_column = Column();
  const char c = Peek();

  if (IsLetter(c)) {
    return LexIdentifier(out);
  }
  if (IsDigit(c)) {
    return LexInteger(out);
  }
  if (static_cast<unsigned char>(c) >= 0x80) {
    if (Utf8SequenceLengthAt(source_, cursor_) == 0) {
      return Fail(start, start_line, start_column,
                  "invalid UTF-8 sequence in source text");
    }
    return Fail(start, start_line, start_column,
                "unexpected non-ASCII character outside a string literal or "
                "comment");
  }

  switch (c) {
    case '"':
      return LexString(out);
    case '@':
      return Emit(TokenKind::kAt, out);
    case '$':
      return Emit(TokenKind::kDollar, out);
    case '{':
      return Emit(TokenKind::kLeftBrace, out);
    case '}':
      return Emit(TokenKind::kRightBrace, out);
    case '(':
      return Emit(TokenKind::kLeftParen, out);
    case ')':
      return Emit(TokenKind::kRightParen, out);
    case '[':
      return Emit(TokenKind::kLeftBracket, out);
    case ']':
      return Emit(TokenKind::kRightBracket, out);
    case ':':
      // `AssignmentOp ::= "=" | ":="` is referenced by no production, so `:=`
      // is left as the two tokens it is spelled with.
      return Emit(TokenKind::kColon, out);
    case ';':
      return Emit(TokenKind::kSemicolon, out);
    case ',':
      return Emit(TokenKind::kComma, out);
    case '=':
      return Peek(1) == '=' ? EmitPair(TokenKind::kEqualEqual, out)
                            : Emit(TokenKind::kEqual, out);
    case '!':
      if (Peek(1) == '=') {
        return EmitPair(TokenKind::kNotEqual, out);
      }
      return Fail(start, start_line, start_column,
                  "unexpected character '!': expected \"!=\"");
    case '<':
      return Peek(1) == '=' ? EmitPair(TokenKind::kLessEqual, out)
                            : Emit(TokenKind::kLess, out);
    case '>':
      return Peek(1) == '=' ? EmitPair(TokenKind::kGreaterEqual, out)
                            : Emit(TokenKind::kGreater, out);
    case '-':
      // One character of lookahead separates the only two things '-'
      // introduces: `PathOp ::= "->"` and a signed `IntegerLiteral`.
      if (Peek(1) == '>') {
        return EmitPair(TokenKind::kArrow, out);
      }
      if (IsDigit(Peek(1))) {
        return LexInteger(out);
      }
      return Fail(start, start_line, start_column,
                  "unexpected character '-': expected \"->\" or a signed "
                  "integer literal");
    default:
      break;
  }
  return Fail(start, start_line, start_column,
              std::string("unexpected character '") + c + "'");
}

StatusOr<std::vector<Token>> EirLexer::Tokenize() {
  cursor_ = 0;
  line_ = 1;
  line_start_ = 0;

  std::vector<Token> tokens;
  for (;;) {
    const Status skipped = SkipWhitespaceAndComments();
    if (!skipped.ok()) {
      return skipped;
    }
    if (AtEnd()) {
      tokens.push_back(Token{TokenKind::kEnd, std::string(), cursor_, line_,
                             Column()});
      return tokens;
    }
    const Status lexed = LexToken(&tokens);
    if (!lexed.ok()) {
      return lexed;
    }
  }
}

}  // namespace veritas::evidence
