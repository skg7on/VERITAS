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
