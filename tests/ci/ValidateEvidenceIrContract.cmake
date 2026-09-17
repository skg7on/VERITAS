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

# §4.1 -- the entity stable ID is a declared attribute, ahead of the open bag.
veritas_spec_section("${EIR_SPEC_CONTENT}"
    "EntityDecl ::=" "EntityProperty ::=" EIR_SECTION_ENTITY)
veritas_require_literals("${EIR_SECTION_ENTITY}" "§4.1"
    "[ \"stable_id\" \"=\" StringLiteral \";\" ] { EntityProperty }")

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

# §11.1 -- the provenance source anchor and its explicit analysis run.
veritas_spec_section("${EIR_SPEC_CONTENT}"
    "ProvenanceDecl ::=" "FactReferenceList ::=" EIR_SECTION_PROVENANCE)
veritas_require_literals("${EIR_SECTION_PROVENANCE}" "§11.1"
    "[ \"configuration\" \"=\" StringLiteral \";\" ] [ \"source_anchor\" \"=\" StringLiteral \";\" ]"
    "[ \"source_anchor\" \"=\" StringLiteral \";\" ] [ \"analysis_run\" \"=\" StringLiteral \";\" ]")

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
