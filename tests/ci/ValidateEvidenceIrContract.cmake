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

# The case-label contract is checked against §3.1 Evidence Case alone, not the
# whole document. §24 Grammar Version History must stay free to quote the
# superseded production verbatim -- a version record forbidden from showing what
# changed is a trap for the next editor -- and a whole-document search would
# make that legitimate quotation a test failure.
string(FIND "${EIR_SPEC_CONTENT}" "### 3.1 Evidence Case" S31_START)
string(FIND "${EIR_SPEC_CONTENT}" "## 4. Entity Grammar" S31_END)

# A missing sentinel is -1, and a -1 start would make the length arithmetic
# below nonsense: the slice would be empty or backwards, and every check run
# against it would pass for the wrong reason. Refuse to search anything that is
# not a real, correctly ordered slice.
if(S31_START EQUAL -1)
  message(FATAL_ERROR
      "EIR formal spec is missing the '### 3.1 Evidence Case' heading")
endif()
if(S31_END EQUAL -1)
  message(FATAL_ERROR
      "EIR formal spec is missing the '## 4. Entity Grammar' heading")
endif()
if(NOT S31_END GREATER S31_START)
  message(FATAL_ERROR
      "EIR formal spec has '## 4. Entity Grammar' before '### 3.1 Evidence Case'")
endif()

math(EXPR S31_LENGTH "${S31_END} - ${S31_START}")
string(SUBSTRING "${EIR_SPEC_CONTENT}" ${S31_START} ${S31_LENGTH} EIR_S31_RAW)

# Post-condition on the offset arithmetic: the slice must begin exactly at the
# §3.1 heading, not merely be non-empty. Without this a wrong start offset could
# yield a slice that still happens to contain the literals below.
string(FIND "${EIR_S31_RAW}" "### 3.1 Evidence Case" S31_ANCHOR)
if(NOT S31_ANCHOR EQUAL 0)
  message(FATAL_ERROR
      "EIR formal spec §3.1 slice does not begin at the §3.1 heading")
endif()

# Markdown hard-wraps prose, so a phrase-level check has to be insensitive to
# where an editor chose to break a line. Unwrapping changes nothing for the
# literals below, each of which is written on a single line; it only stops a
# reflowed paragraph from reading as a deleted one. (A mandatory production
# split across two lines now also matches the forbidden literal below, which is
# the outcome we want.)
string(REPLACE "\n" " " EIR_S31_UNWRAPPED "${EIR_S31_RAW}")
string(REGEX REPLACE " +" " " EIR_SPEC_S31 "${EIR_S31_UNWRAPPED}")

foreach(S31_REQUIRED_LITERAL IN ITEMS
    # The production: the case label is optional.
    "\"evidence\" [ Identifier ] \"{\""
    # The normative paragraph, which is what Tasks 7 and 8 are written against.
    "The case `Identifier` is a display label and carries no semantic content"
    "The canonical writer emits no label")
  string(FIND "${EIR_SPEC_S31}" "${S31_REQUIRED_LITERAL}" FOUND_AT)
  if(FOUND_AT EQUAL -1)
    message(FATAL_ERROR
        "EIR formal spec §3.1 is missing: ${S31_REQUIRED_LITERAL}")
  endif()
endforeach()

# Falsifying half of the case-label contract: the mandatory form must not
# survive in §3.1. The required literals above cannot see a spec that keeps the
# optional production and also restores the mandatory form beside it.
foreach(S31_FORBIDDEN_LITERAL IN ITEMS
    "\"evidence\" Identifier \"{\"")
  string(FIND "${EIR_SPEC_S31}" "${S31_FORBIDDEN_LITERAL}" FOUND_AT)
  if(NOT FOUND_AT EQUAL -1)
    message(FATAL_ERROR
        "EIR formal spec §3.1 must not contain: ${S31_FORBIDDEN_LITERAL}")
  endif()
endforeach()
