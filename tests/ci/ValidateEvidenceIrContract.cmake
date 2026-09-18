# Copyright 2026 VERITAS Contributors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

if(NOT DEFINED VERITAS_SOURCE_DIR)
  message(FATAL_ERROR "VERITAS_SOURCE_DIR is required")
endif()

set(EIR_SPEC
    "${VERITAS_SOURCE_DIR}/docs/specs/veritas-evidence-ir-formal-specification.md")
file(READ "${EIR_SPEC}" EIR_SPEC_CONTENT)

foreach(REQUIRED_LITERAL IN ITEMS
    "SchemaDecl ::= \"schema\" \"=\" StringLiteral \";\""
    "EvidenceLevel ::= \"l0\" | \"l1\" | \"l2\""
    "EvidenceState ::="
    "DependencyDecl ::="
    "OmissionDecl ::="
    "ImplicationExpr ::="
    "OrExpr ::="
    "AndExpr ::="
    "ComparisonExpr ::="
    "UnaryExpr ::=")
  string(FIND "${EIR_SPEC_CONTENT}" "${REQUIRED_LITERAL}" FOUND_AT)
  if(FOUND_AT EQUAL -1)
    message(FATAL_ERROR "EIR formal spec is missing: ${REQUIRED_LITERAL}")
  endif()
endforeach()

# --- Section extraction ------------------------------------------------------

# Extract the body of one spec section, bounded by two anchors, and normalize it
# to a single line. Markdown hard-wraps prose and EBNF blocks alike, so a
# phrase-level or production-level check has to be insensitive to where an editor
# chose to break a line; unwrapping only stops a reflowed paragraph from reading
# as a deleted one.
#
# The slice is taken from, and must begin at, the start anchor, so a literal
# found here really lives in this section: a quotation of the same text elsewhere
# in the document, §24 Grammar Version History above all, cannot satisfy it.
function(veritas_spec_section CONTENT START_ANCHOR END_ANCHOR OUT_VAR)
  string(FIND "${CONTENT}" "${START_ANCHOR}" START_AT)
  if(START_AT EQUAL -1)
    message(FATAL_ERROR
        "EIR formal spec is missing the '${START_ANCHOR}' anchor")
  endif()
  string(FIND "${CONTENT}" "${END_ANCHOR}" END_AT)
  if(END_AT EQUAL -1)
    message(FATAL_ERROR
        "EIR formal spec is missing the '${END_ANCHOR}' anchor")
  endif()
  if(NOT END_AT GREATER START_AT)
    message(FATAL_ERROR
        "EIR formal spec has '${END_ANCHOR}' before '${START_ANCHOR}'")
  endif()

  math(EXPR LENGTH "${END_AT} - ${START_AT}")
  string(SUBSTRING "${CONTENT}" ${START_AT} ${LENGTH} RAW)

  # Post-condition on the offset arithmetic: the slice must begin exactly at the
  # start anchor, not merely be non-empty. Without this a wrong start offset
  # could yield a slice that still happens to contain the literals below.
  string(FIND "${RAW}" "${START_ANCHOR}" ANCHOR_AT)
  if(NOT ANCHOR_AT EQUAL 0)
    message(FATAL_ERROR
        "EIR formal spec section does not begin at '${START_ANCHOR}'")
  endif()

  string(REPLACE "\n" " " UNWRAPPED "${RAW}")
  string(REGEX REPLACE " +" " " SLICE "${UNWRAPPED}")
  set(${OUT_VAR} "${SLICE}" PARENT_SCOPE)
endfunction()

# Require each literal of one section to appear in that section's body. The
# literals are addressed by index, `${ARGV<n>}`, and never through a list: every
# production below ends in the grammar's `";"` terminator, so a literal routed
# through a semicolon-joined list is split at that delimiter into fragments that
# then match independently -- a pin that reads as one production but tests two
# substrings, and one that stays green when the terminator is dropped.
function(veritas_require_literals SECTION_TEXT SECTION_NAME)
  if(ARGC LESS 3)
    message(FATAL_ERROR
        "veritas_require_literals needs a literal for ${SECTION_NAME}")
  endif()
  math(EXPR LAST_INDEX "${ARGC} - 1")
  foreach(INDEX RANGE 2 ${LAST_INDEX})
    set(SECTION_LITERAL "${ARGV${INDEX}}")
    string(FIND "${SECTION_TEXT}" "${SECTION_LITERAL}" FOUND_AT)
    if(FOUND_AT EQUAL -1)
      message(FATAL_ERROR
          "EIR formal spec ${SECTION_NAME} is missing: ${SECTION_LITERAL}")
    endif()
  endforeach()
endfunction()

# The case-label contract is checked against §3.1 Evidence Case alone, not the
# whole document. §24 Grammar Version History must stay free to quote the
# superseded production verbatim -- a version record forbidden from showing what
# changed is a trap for the next editor -- and a whole-document search would
# make that legitimate quotation a test failure. The contextual properties the
# semantic model requires are pinned the same way, against the section that owns
# the production.
veritas_spec_section("${EIR_SPEC_CONTENT}"
    "### 3.1 Evidence Case" "## 4. Entity Grammar" EIR_SECTION_S31)

veritas_require_literals("${EIR_SECTION_S31}" "§3.1"
    # The production: the case label is optional.
    "\"evidence\" [ Identifier ] \"{\""
    # The normative paragraph in full -- what Tasks 7 and 8 are written against.
    # Pinned whole rather than by sampled sentences: pinning the first sentence
    # and the last clause left the middle sentence, which carries the
    # anti-over-rejection rule and "preserves no label in the semantic model",
    # deletable with the test still green. The slice is already unwrapped, so
    # this survives a reflow but not a lost or reworded clause.
    "The case `Identifier` is a display label and carries no semantic content: it is not an input to `EvidenceID`, and the semantic model has no member for it. A parser must accept any well-formed label, and must not reject a case for carrying one, but it preserves no label in the semantic model. The canonical writer emits no label, so canonical text is a function of the case's semantics alone."
    # The remaining program identity: the type layout, the analysis run, and the
    # repeatable analyzer list with its own production.
    "| \"type_layout\" \"=\" StringLiteral \";\""
    "| \"analysis_run\" \"=\" StringLiteral \";\""
    "| \"analyzer\" \"=\" AnalyzerVersion \";\""
    "AnalyzerVersion ::= Producer \"(\" [ StringLiteral [ \",\" StringLiteral ] ] \")\" ;")

# §4.1 -- the entity stable ID is a declared attribute, ahead of the open bag,
# and the section that records what the grammar cannot write. The slice runs to
# the next section heading, not to the end of the EBNF block, so it covers the
# normative prose as well as the production: the `Producer` limit is prose, and
# a slice that stopped at `EntityProperty ::=` would pin nothing but syntax.
veritas_spec_section("${EIR_SPEC_CONTENT}"
    "EntityDecl ::=" "## 5. Predicate Language" EIR_SECTION_ENTITY)
veritas_require_literals("${EIR_SECTION_ENTITY}" "§4.1"
    "[ \"stable_id\" \"=\" StringLiteral \";\" ] { EntityProperty }"
    # The `Producer` narrowing, pinned whole rather than by sampled sentences.
    # Every claim in it is load-bearing -- what the nonterminal cannot write,
    # that the unwritable value is one the model holds and the canonical form
    # hashes, that the limit is systematic rather than local to this amendment,
    # and that leaving it is a decision rather than an oversight -- and a
    # sampled pin lets any unsampled sentence go silently. The slice is already
    # unwrapped, so this survives a reflow but not a reworded clause.
    #
    # The `Scope` closing paragraph is part of the same literal. It records the
    # cost of the narrow `Scope` production §5.1 states as a widening not taken:
    # `Constraint::scope` and `Assumption::scope` are plain strings the canonical
    # form hashes verbatim, so a scope outside the six keywords plus
    # `FunctionCall` is carriable and unspellable. Without it here the two
    # sections disagree by silence -- §5.1 asserts the narrowing and §4.1 never
    # states what it costs -- and the sentence is deletable with the suite green.
    "A second limit is systematic rather than local. `Producer` is `QualifiedId`, which admits letters, digits, underscores, and interior dots and nothing else, so a producer value that is empty or that carries a character outside that alphabet has no spelling at all — although every field it feeds holds a plain string. Such a value is reachable: `AnalyzerVersion`'s producer and `ProofObligation`'s verification producer are `std::string` fields the canonical form hashes verbatim, and no validator constrains their shape — the analyzer list is not examined at all, and a decided result's producer is required only to be non-empty. This is a known limitation of EIR-T 1.0's identifier alphabet for string-valued fields, not of any one declaration. `Producer` has five carriers — `AnalyzerVersion`'s producer and `VerificationDecl`'s, `FactDecl`'s `source`, `HypothesisDecl`'s `producer`, and `ProvenanceDecl`'s `producer` — and the limit reaches every one of them. `AssumptionDecl`'s `source` is not a `Producer` carrier, and the limit reaches it only in part: `AssumptionSource` admits `FunctionCall`, so a call-shaped source is spellable, while its other branch is the identifier-shaped `QualifiedId`, and a source that is neither — a plain `src/main.c` — has no spelling at all. It is deliberately not widened here. Relaxing `Producer` would change a production this amendment did not introduce and would need a canonical-spelling rule for the one-value-two-spellings case, with no writer yet to test that rule against; the repair belongs to a coherent change to string-valued fields as a class, not to ad hoc widening of a single production. `Scope` is a third limit, and like `Producer`'s it is an alphabet limit rather than a positional one. It is `\"global\" | \"function\" | \"path\" | \"basic_block\" | \"callsite\" | \"entity\" | FunctionCall`, and `ConstraintDecl` (§10.1) and `AssumptionDecl` both hold it in a plain string the canonical form hashes verbatim, so a scope that is neither one of the six keywords nor a call — say `everywhere` — has no spelling at all: a writer handed one could not emit text its own parser would read back, and `REP-001` would fail for any case holding it. The narrow production is kept rather than widened. The six keywords plus `FunctionCall` are what the grammar says, §10.1 defines no seventh scope, and admitting an arbitrary identifier there would make the written language wider than the declaration it lowers into — the same reasoning §5.1 records for `QualifiedId`, `Scope`, and `Producer` together. No corpus case exercises the gap today, because the scenarios that carry a scope write `global` or `path`; it is recorded here so the narrowing is a decision on the record rather than an unstated cost.")

# §5.1 -- the four value shapes a parser admits beyond the letter of the
# production, which the `eir.v1` model forces. The slice runs to the next
# subsection heading, not to the end of the EBNF block, so it covers the prose
# that states the widenings as well as the grammar they widen: the widenings are
# prose, and a slice that stopped at `Domain ::=` would pin nothing but syntax.
# The paragraph is pinned whole for the same reason as §3.1's and §4.1's --
# dropping the property-value half, or the clause that `QualifiedId`, `Scope`,
# and `Producer` are deliberately not widened with it, would otherwise let an
# over-rejection ship with the suite green.
veritas_spec_section("${EIR_SPEC_CONTENT}"
    "### 5.1 Predicate Expression" "### 5.2 Precedence and Associativity"
    EIR_SECTION_S51)
veritas_require_literals("${EIR_SECTION_S51}" "§5.1"
    "PrimaryExpr ::= AtomicPredicate | \"(\" Predicate \")\" ;"
    "PredicateArgument ::="
    "`PrimaryExpr` as written above is `AtomicPredicate | \"(\" Predicate \")\"`, and four value shapes a parser must accept are in neither that production nor `PropertyValue` (§4.1). Each is admitted here, and each is admitted because refusing it would make a value the `eir.v1` model already carries unrepresentable — a writer that emitted one could not read its own output back, so `REP-001` would fail for any case holding it. This is the same trade §4.1 records for the entity property bag, and it is a widening of the language, not a relaxation of the model. - **A bare identifier, in both the predicate and the property-value positions.** `Expression::Kind::kSymbol` is \"a bare name such as a domain value or a constant symbol\", and a bare identifier is its only spelling. §15's normative example already depends on the predicate half — it writes `@packet.type == EXTENSION`, comparing a reference against a bare identifier. The property-value half is not shown by that example, which puts `EXTENSION` in a predicate; it is admitted because `Entity::properties` (§4.1) is an open bag of `Expression` and the bag's values are not a narrower type. Any identifier that is neither a lexical keyword of the document nor followed by `\"(\"` lowers to `kSymbol`; `true` and `false` lower to `BooleanLiteral` as before, and an identifier followed by `\"(\"` is a `FunctionCall`. - **`StringLiteral` and `IntegerLiteral` at the primary level.** `PredicateArgument` lists `StringLiteral` and `IntegerLiteral`, and every argument of a `FunctionCall` is parsed as a full `Predicate`, so the only path by which `f(\"text\")` and `f(1)` reach the argument grammar is through `PrimaryExpr`. Without these two the second and third alternatives of `PredicateArgument` would be unreachable and `f(\"text\")` would have no spelling at all. `QualifiedId` is deliberately **not** widened, and neither is `Scope` (`\"global\" | \"function\" | \"path\" | \"basic_block\" | \"callsite\" | \"entity\" | FunctionCall`) or `Producer` (§4.1): those are closed enumerations of their own, and admitting an arbitrary identifier into one would admit documents the declaration does not define. A parser that did so would be accepting a language the specification does not describe.")

# §6.1 -- the fact stable ID and the derived marker.
veritas_spec_section("${EIR_SPEC_CONTENT}"
    "FactDecl ::=" "EpistemicState ::=" EIR_SECTION_FACT)
veritas_require_literals("${EIR_SECTION_FACT}" "§6.1"
    "[ \"provenance\" \"=\" Reference \";\" ] [ \"stable_id\" \"=\" StringLiteral \";\" ] [ \"derived\" \"=\" BooleanLiteral \";\" ]")

# §7.3 -- the free-text detail carried beside the closed reason code.
veritas_spec_section("${EIR_SPEC_CONTENT}"
    "UnknownDecl ::=" "UnknownReason ::=" EIR_SECTION_UNKNOWN)
veritas_require_literals("${EIR_SECTION_UNKNOWN}" "§7.3"
    "\"reason\" \"=\" UnknownReason \";\" [ \"detail\" \"=\" StringLiteral \";\" ]")

# §11.1 -- the provenance source anchor, its explicit analysis run, and the
# rejection rule for `location`, which the production still admits but the
# V0.1 model cannot represent. The slice runs to the next section heading, not
# to the end of the EBNF block, so it covers the normative prose as well as the
# production: the `location` rule is prose, and a slice that stopped at
# `FactReferenceList ::=` would pin nothing but syntax.
veritas_spec_section("${EIR_SPEC_CONTENT}"
    "ProvenanceDecl ::=" "## 12. Verification Grammar" EIR_SECTION_PROVENANCE)
veritas_require_literals("${EIR_SECTION_PROVENANCE}" "§11.1"
    "[ \"configuration\" \"=\" StringLiteral \";\" ] [ \"source_anchor\" \"=\" StringLiteral \";\" ]"
    "[ \"source_anchor\" \"=\" StringLiteral \";\" ] [ \"analysis_run\" \"=\" StringLiteral \";\" ]"
    # The whole normative paragraph, pinned as one literal for the same reason
    # as §4.1's below: pinning the operative sentence and the distinction
    # separately left "is grammar-valid but model-unrepresentable, because" and
    # the closing REP-001 clause each silently deletable. This is the paragraph
    # whose absence lets a parser lower `location` onto `source_anchor_id`, so
    # every sentence of it is load-bearing. The slice is already unwrapped, so
    # this survives a reflow but not a reworded clause.
    "`location` and `source_anchor` are distinct, and only `source_anchor` has a home in the EIR V0.1 model: `ProvenanceDecl`'s `location` is grammar-valid but model-unrepresentable, because the `eir.v1` provenance record carries no location member and no member that could hold one. A parser must reject a declaration that carries `location` with a typed \"unsupported in EIR V0.1\" diagnostic; it must never lower the value onto `source_anchor_id`, and must never silently drop it. Either substitution would change `EvidenceID` for such an input and break REP-001 losslessness, which is the whole reason the EIR-T 1.0 language surface is wider than the V0.1 model it is parsed into.")

# §12.1 -- the verification producer travels with the obligation's result.
veritas_spec_section("${EIR_SPEC_CONTENT}"
    "VerificationDecl ::=" "VerificationGoal ::=" EIR_SECTION_VERIFY)
veritas_require_literals("${EIR_SECTION_VERIFY}" "§12.1"
    "[ \"result\" \"=\" Reference \";\" ] [ \"producer\" \"=\" Producer \";\" ]")

# Falsifying half of the case-label contract: the mandatory form must not
# survive in §3.1. The required literals above cannot see a spec that keeps the
# optional production and also restores the mandatory form beside it.
foreach(S31_FORBIDDEN_LITERAL IN ITEMS
    "\"evidence\" Identifier \"{\"")
  string(FIND "${EIR_SECTION_S31}" "${S31_FORBIDDEN_LITERAL}" FOUND_AT)
  if(NOT FOUND_AT EQUAL -1)
    message(FATAL_ERROR
        "EIR formal spec §3.1 must not contain: ${S31_FORBIDDEN_LITERAL}")
  endif()
endforeach()

# Falsifying half of §24's revision record. The 1.0 row must not claim the
# losslessness gap was closed, and must not describe the residue in the
# singular: §4.1 records four things the amendment leaves unwritable -- the
# entity `stable_id` corner, the `Producer` alphabet, the `Scope` alphabet, and
# a property-bag key outside `Identifier` -- so a row asserting closure, or
# counting one, contradicts the section it describes.
#
# Only those two wrong claims are forbidden, not the row's wording and not its
# arithmetic -- a version record has to stay rewritable, so these are negative
# pins rather than a quotation of the replacement sentence. §4.1's list is
# allowed to grow, so pinning any particular number here would be wrong the
# moment a fifth limit is recorded, and no pin short of quoting the count could
# detect a stale one: a row reading "the two values that stay unwritable" would
# pass these pins today and would have to be corrected against §4.1 itself.
veritas_spec_section("${EIR_SPEC_CONTENT}"
    "## 24. Grammar Version History" "**End of Formal Specification**"
    EIR_SECTION_S24)

foreach(S24_FORBIDDEN_LITERAL IN ITEMS
    "Closed the losslessness gap"
    "the single value that stays unwritable")
  string(FIND "${EIR_SECTION_S24}" "${S24_FORBIDDEN_LITERAL}" FOUND_AT)
  if(NOT FOUND_AT EQUAL -1)
    message(FATAL_ERROR
        "EIR formal spec §24 must not claim: ${S24_FORBIDDEN_LITERAL}")
  endif()
endforeach()
