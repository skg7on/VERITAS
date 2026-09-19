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

// Which of the two EIR-T text layouts `WriteEirText` produces.
//
// The two modes differ in whitespace only — indentation and line breaks — and
// never in a token, a value, or an order. Both parse back to the same
// `eir.v1` case, so both encode to the same canonical bytes and therefore to
// the same `EvidenceID`; a caller may choose either without affecting
// identity.
//
//   * `kCanonical` — the tight, content-addressable layout: one declaration
//     per line, four spaces of indentation per nesting level, no blank lines,
//     no comments. This is the layout a caller pins a golden against.
//   * `kPretty` — the same declarations and the same indentation, with a
//     blank line between the case's top-level items so the structure is
//     visible when read.
//
// Neither mode assumes one token per line, and neither ever emits a raw
// newline or carriage return inside a string literal: `\n`, `\r`, `\t`, `\"`,
// and `\\` are escaped, and every other byte is emitted as itself.
enum class EirTextStyle {
  kCanonical,
  kPretty,
};

// Writes one `eir.v1` case as EIR-T text, in `style`.
//
// This is the inverse of `ParseEirText`: for a case `c` that this function
// accepts, `ParseEirText(WriteEirText(c))` is a case with the same canonical
// bytes and the same `EvidenceID`, and writing that result again reproduces
// the same text byte for byte. That write/parse/write fixpoint is `REP-001`.
//
// The writer emits **no top-level case identifier**. §3.1 makes the case label
// a display label that carries no semantic content and that the `eir.v1` model
// has no member for, so canonical output is `evidence { … }` — the token after
// `evidence` is always `{`. Nothing in this file, and nothing anywhere else,
// derives a label from `EvidenceID`.
//
// ORDER
//
// Top-level properties are emitted `schema`, `level`, `state`, `context`, then
// the members in the order
// `entity, claim, fact, assumption, hypothesis, unknown, edge, path,
// constraint, provenance, verify, summary, dependency, omission`. Within a
// member category the records are emitted in the canonicalizer's order — its
// `(kind, stable-id, local-id)` key, **including the components that key leaves
// empty** — so the text's order is a function of the case's meaning rather than
// of the order the case happened to be assembled in. For `summaries` and
// `dependencies` the canonicalizer's middle component is empty, so those two
// families sort by kind and local handle alone; a writer that sorted them by
// `summary_id`/`stable_id` would print a different order from the one their
// identity is built on. `ProgramBinding::analyzer_versions` and the four
// reference lists (`Unknown::blocking_ids`, `Provenance::input_fact_ids`,
// `SummaryReference::components`, `ProofObligation::verifier_kinds`) are
// sorted, which §19.1 requires.
//
// Two sequences are emitted exactly as the model declares them, because the
// model's order is what the reader preserves and neither is reordered for
// identity: `Path::entity_ids` (the segment sequence *is* the path) and the
// operands of `kAnd`/`kOr` (written flattened, never re-parenthesised). A case
// whose commutative operands or path conditions were permuted therefore has
// one `EvidenceID` — the canonicalizer orders both — but is written with the
// order it declares. That asymmetry is deliberate and is not a `REP-001`
// failure: the writer's fixpoint holds for each case, and identity never
// depended on the text.
//
// REFUSALS
//
// The writer validates before it emits and returns a typed error rather than
// producing text its own parser would refuse, or text that would read back as
// a different case. In particular it refuses, rather than dropping or
// coercing, the four values §4.1 records as having no EIR-T spelling at all:
//
//   * a `scope` outside `"global" | "function" | "path" | "basic_block" |
//     "callsite" | "entity" | FunctionCall` — `Scope` is deliberately not
//     widened, so the string has no spelling;
//   * a producer string the `QualifiedId` alphabet cannot spell — empty, or
//     carrying a character outside letters, digits, underscores, and interior
//     dots. `Producer` has five carriers: `AnalyzerVersion::producer`,
//     `Fact::producer`, `Hypothesis::producer`, `Provenance::producer`, and
//     `ProofObligation::verification_producer`;
//   * a property-bag entry named `stable_id` on an entity that declares no
//     identity — the leading attribute binds the identity, so emitting such an
//     entry would have it re-read as one;
//   * a property-bag **key** that is not an `Identifier` — §4.1 fixes
//     `PropertyKey ::= Identifier` and nothing upstream of the writer checks
//     it, so the alphabet is enforced here. The key is emitted verbatim
//     otherwise, which for `"not an identifier"` is text the parser rejects and
//     for `"x = 1; y"` is text that reparses into a *different* case, silently.
//
// `REP-001` cannot detect any of these: a value the writer dropped would be
// absent from both passes and the round trip would stay green. Silent omission
// is therefore the defect, and a typed error is the fix.
//
// The same obligation governs the remaining refusals: an expression kind or a
// value shape with no spelling in the position it occupies, an identifier that
// is not an EIR-T identifier, a producer-shaped attribute that is not one, and
// a call-shaped carrier that is not in the canonical spelling `RenderValue`
// produces or that carries a raw line break. Each is a `Status`, never a
// dropped value.
//
// FAILURE MODES
//
// Returns `InvalidArgument` when `value` is not well-formed, when it carries
// no `evidence_id`, when its `evidence_id` is not its recomputed content
// address, or when it holds one of the values above. A case that is not
// finalized (`FinalizeEvidenceIdentity` has not run) is refused rather than
// written, so a caller cannot serialize a case whose identity is not yet its
// own. Every failure leaves `value` unchanged — the writer never mutates it.
StatusOr<std::string> WriteEirText(const EvidenceCase& value, EirTextStyle style);

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
// four value shapes it admits beyond the letter of §5.1 and §4.1 are listed in
// that section's prose; each exists because the model can carry the value and
// the writer must reproduce it. The
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
