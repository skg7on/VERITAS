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
#include <string_view>

#include "veritas/core/Status.h"
#include "veritas/evidence/EvidenceCase.h"

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

// Reads one EIR-T document and lowers it to a validated, identity-bearing
// `eir.v1` case.
//
// The three stages run in order and the later ones never run on an earlier
// one's failure: tokenize, parse, then validate and finalize. A document that
// tokenizes and parses but is not well-formed is refused here rather than
// returned half-built — `RequireValidEvidenceCase` and `FinalizeEvidenceIdentity`
// are the same gates every other M10C representation boundary runs through, so
// a caller never receives a case it would have to re-check.
//
// On success `evidence_id` is the computed content address. The case-level
// display label is accepted and discarded: it is not part of the model and
// never reaches the returned value, so it cannot reach `EvidenceID` either.
//
// REJECTIONS
//
// The parser rejects everything the grammar does not admit, and — because the
// EIR-T 1.0 language surface is deliberately wider than the V0.1 model it is
// parsed into — everything the grammar admits but the model cannot carry. The
// second class is refused with a typed `"unsupported in EIR V0.1"` diagnostic,
// never dropped and never lowered onto a neighbouring field: silently accepting
// one would change `EvidenceID` for that input. `ProvenanceDecl`'s `location`
// is the carrier this matters most for (`location` and `source_anchor` are
// distinct, and only the latter exists in the model), and `EdgeDecl`'s
// `condition`, `transfer`, and `summary` join it for the same reason.
//
// FAILURE MODES
//
// Returns `Status::InvalidArgument` for every rejection. When `error` is
// non-null it receives the position and message of the construct that was
// refused: the failing token's offset, one-based line, and one-based byte
// column for a lexical or syntactic rejection, and the start of the document
// for a well-formedness rejection, which is about the case as a whole and names
// its own member rather than a token. `error` may be null when only the
// `Status` is needed.
StatusOr<EvidenceCase> ParseEirText(std::string_view source, EirParseError* error);

}  // namespace veritas::evidence

#endif  // VERITAS_EVIDENCE_EIR_TEXT_H_
