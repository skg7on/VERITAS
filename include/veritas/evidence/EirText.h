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

// EirText.h — the public error contract of the EIR-T text front end.
//
// EIR-T is the surface syntax of the Evidence IR defined by
// `docs/specs/veritas-evidence-ir-formal-specification.md`. Reading it is a
// two-stage pipeline — a lexer, then a parser — and both report failure through
// the single type below. This header carries no token model and no lexer: those
// are private to `src/evidence/EirSyntax.h`, because nothing outside the reader
// and its writer may depend on how the text is cut into tokens.
//
// COORDINATES
//
// `offset` is a zero-based **byte** offset into the source. `line` is one-based.
// `column` is one-based and counts **bytes** as well: it is the number of bytes
// between the start of the reported line and the reported position, plus one.
// A line holding multi-byte UTF-8 therefore reports byte columns, not
// code-point or grapheme columns. That is deliberate — it is the same
// coordinate system `offset` already uses, so a caller can always recover the
// byte range of a diagnostic by adding `column - 1` to the line's start offset,
// and no caller has to know which of three "column" definitions applies.
//
// A line ends at `"\n"`. A bare `"\r"` is whitespace, not a line terminator, so
// a `"\r\n"` source counts each line break exactly once.

#ifndef VERITAS_EVIDENCE_EIR_TEXT_H_
#define VERITAS_EVIDENCE_EIR_TEXT_H_

#include <cstddef>
#include <string>

namespace veritas::evidence {

// Where an EIR-T lexical or syntactic error was found, and what it was.
//
// The four fields are filled together: a failing reader call reports the
// position of the construct it rejected (never the position of whatever
// followed it) and a message naming that construct. The defaults describe the
// start of an empty source, so a caller that ignores a failure sees a
// well-formed, if uninformative, position rather than garbage.
struct EirParseError {
  std::size_t offset = 0;
  std::size_t line = 1;
  std::size_t column = 1;
  std::string message;
};

}  // namespace veritas::evidence

#endif  // VERITAS_EVIDENCE_EIR_TEXT_H_
