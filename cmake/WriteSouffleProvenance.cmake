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

# WriteSouffleProvenance.cmake — writes the generated Souffle provenance manifest.
#
# Invoked by VeritasSouffle.cmake after the complete production toolchain is
# built. The canonical digest binds every runtime/generation artifact and the
# compiler, linker, and platform metadata that can affect their bytes.

foreach(_required IN ITEMS
    VERITAS_SOUFFLE_EXECUTABLE
    VERITAS_SOUFFLE_RUNNER
    VERITAS_SOUFFLE_FUNCTOR_LIBRARY
    VERITAS_REACHABILITY_BUNDLE
    VERITAS_MAY_WRITE_BUNDLE
    VERITAS_SOUFFLE_REVISION
    VERITAS_COMPILER_ID
    VERITAS_COMPILER_VERSION
    VERITAS_COMPILER_PATH
    VERITAS_SYSTEM_NAME
    VERITAS_SYSTEM_PROCESSOR
    VERITAS_CMAKE_GENERATOR
    VERITAS_BUILD_TYPE
    VERITAS_CXX_STANDARD
    VERITAS_CXX_FLAGS
    VERITAS_EXECUTABLE_LINKER_FLAGS
    VERITAS_SOUFFLE_PROVENANCE_OUTPUT)
  if(NOT DEFINED ${_required})
    message(FATAL_ERROR "WriteSouffleProvenance: missing -D${_required}")
  endif()
endforeach()

function(veritas_json_escape INPUT OUTPUT)
  string(REPLACE "\\" "\\\\" _escaped "${INPUT}")
  string(REPLACE "\"" "\\\"" _escaped "${_escaped}")
  string(REPLACE "\n" "\\n" _escaped "${_escaped}")
  string(REPLACE "\r" "\\r" _escaped "${_escaped}")
  string(REPLACE "\t" "\\t" _escaped "${_escaped}")
  set(${OUTPUT} "${_escaped}" PARENT_SCOPE)
endfunction()

file(SHA256 "${VERITAS_SOUFFLE_EXECUTABLE}" _souffle_digest)
file(SHA256 "${VERITAS_SOUFFLE_RUNNER}" _runner_digest)
file(SHA256 "${VERITAS_SOUFFLE_FUNCTOR_LIBRARY}" _functor_digest)
file(SHA256 "${VERITAS_REACHABILITY_BUNDLE}" _reach_digest)
file(SHA256 "${VERITAS_MAY_WRITE_BUNDLE}" _maywrite_digest)

string(CONCAT _canonical
  "source_revision=${VERITAS_SOUFFLE_REVISION}\n"
  "souffle_executable_sha256=${_souffle_digest}\n"
  "runner_library_sha256=${_runner_digest}\n"
  "functor_library_sha256=${_functor_digest}\n"
  "reachability_bundle_sha256=${_reach_digest}\n"
  "may_write_bundle_sha256=${_maywrite_digest}\n"
  "compiler_id=${VERITAS_COMPILER_ID}\n"
  "compiler_version=${VERITAS_COMPILER_VERSION}\n"
  "compiler_path=${VERITAS_COMPILER_PATH}\n"
  "system_name=${VERITAS_SYSTEM_NAME}\n"
  "system_processor=${VERITAS_SYSTEM_PROCESSOR}\n"
  "cmake_generator=${VERITAS_CMAKE_GENERATOR}\n"
  "build_type=${VERITAS_BUILD_TYPE}\n"
  "cxx_standard=${VERITAS_CXX_STANDARD}\n"
  "cxx_flags=${VERITAS_CXX_FLAGS}\n"
  "executable_linker_flags=${VERITAS_EXECUTABLE_LINKER_FLAGS}\n")
string(SHA256 _canonical_digest "${_canonical}")

foreach(_metadata IN ITEMS COMPILER_ID COMPILER_VERSION COMPILER_PATH
                           SYSTEM_NAME SYSTEM_PROCESSOR CMAKE_GENERATOR
                           BUILD_TYPE CXX_STANDARD CXX_FLAGS
                           EXECUTABLE_LINKER_FLAGS)
  veritas_json_escape("${VERITAS_${_metadata}}" _${_metadata})
endforeach()
veritas_json_escape("${VERITAS_SOUFFLE_REVISION}" _source_revision)

file(WRITE "${VERITAS_SOUFFLE_PROVENANCE_OUTPUT}"
  "{\n"
  "  \"source_revision\": \"${_source_revision}\",\n"
  "  \"executable_sha256\": \"${_souffle_digest}\",\n"
  "  \"souffle_executable_sha256\": \"${_souffle_digest}\",\n"
  "  \"runner_library_sha256\": \"${_runner_digest}\",\n"
  "  \"functor_library_sha256\": \"${_functor_digest}\",\n"
  "  \"reachability_bundle_sha256\": \"${_reach_digest}\",\n"
  "  \"may_write_bundle_sha256\": \"${_maywrite_digest}\",\n"
  "  \"compiler_id\": \"${_COMPILER_ID}\",\n"
  "  \"compiler_version\": \"${_COMPILER_VERSION}\",\n"
  "  \"compiler_path\": \"${_COMPILER_PATH}\",\n"
  "  \"system_name\": \"${_SYSTEM_NAME}\",\n"
  "  \"system_processor\": \"${_SYSTEM_PROCESSOR}\",\n"
  "  \"cmake_generator\": \"${_CMAKE_GENERATOR}\",\n"
  "  \"build_type\": \"${_BUILD_TYPE}\",\n"
  "  \"cxx_standard\": \"${_CXX_STANDARD}\",\n"
  "  \"cxx_flags\": \"${_CXX_FLAGS}\",\n"
  "  \"executable_linker_flags\": \"${_EXECUTABLE_LINKER_FLAGS}\",\n"
  "  \"canonical_provenance_sha256\": \"${_canonical_digest}\"\n"
  "}\n")
