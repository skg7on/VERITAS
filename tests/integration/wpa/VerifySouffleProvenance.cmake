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

foreach(_required IN ITEMS MANIFEST SOUFFLE WORKER FUNCTOR REACH_BUNDLE
                           MAYWRITE_BUNDLE EXPECTED_REVISION)
  if(NOT DEFINED ${_required})
    message(FATAL_ERROR "VerifySouffleProvenance: missing -D${_required}")
  endif()
endforeach()

file(READ "${MANIFEST}" _json)

function(read_field FIELD OUTPUT)
  string(JSON _value GET "${_json}" "${FIELD}")
  set(${OUTPUT} "${_value}" PARENT_SCOPE)
endfunction()

function(assert_digest FIELD PATH OUTPUT)
  file(SHA256 "${PATH}" _expected)
  read_field("${FIELD}" _actual)
  if(NOT _actual STREQUAL _expected)
    message(FATAL_ERROR
      "${FIELD} does not match ${PATH}: expected ${_expected}, got ${_actual}")
  endif()
  set(${OUTPUT} "${_actual}" PARENT_SCOPE)
endfunction()

read_field("source_revision" _source_revision)
if(NOT _source_revision STREQUAL EXPECTED_REVISION)
  message(FATAL_ERROR
    "source_revision: expected ${EXPECTED_REVISION}, got ${_source_revision}")
endif()

assert_digest("souffle_executable_sha256" "${SOUFFLE}" _souffle_digest)
assert_digest("worker_executable_sha256" "${WORKER}" _worker_digest)
assert_digest("functor_library_sha256" "${FUNCTOR}" _functor_digest)
assert_digest("reachability_bundle_sha256" "${REACH_BUNDLE}" _reach_digest)
assert_digest("may_write_bundle_sha256" "${MAYWRITE_BUNDLE}" _maywrite_digest)

read_field("executable_sha256" _compatibility_digest)
if(NOT _compatibility_digest STREQUAL _souffle_digest)
  message(FATAL_ERROR "executable_sha256 compatibility field is inconsistent")
endif()

foreach(_metadata IN ITEMS compiler_id compiler_version compiler_path
                           system_name system_processor cmake_generator
                           build_type cxx_standard)
  read_field("${_metadata}" _value)
  if(_value STREQUAL "")
    message(FATAL_ERROR "${_metadata} must not be empty")
  endif()
endforeach()
read_field("compiler_id" _compiler_id)
read_field("compiler_version" _compiler_version)
read_field("compiler_path" _compiler_path)
read_field("system_name" _system_name)
read_field("system_processor" _system_processor)
read_field("cmake_generator" _cmake_generator)
read_field("build_type" _build_type)
read_field("cxx_standard" _cxx_standard)
read_field("cxx_flags" _cxx_flags)
read_field("executable_linker_flags" _linker_flags)

string(CONCAT _canonical
  "source_revision=${_source_revision}\n"
  "souffle_executable_sha256=${_souffle_digest}\n"
  "worker_executable_sha256=${_worker_digest}\n"
  "functor_library_sha256=${_functor_digest}\n"
  "reachability_bundle_sha256=${_reach_digest}\n"
  "may_write_bundle_sha256=${_maywrite_digest}\n"
  "compiler_id=${_compiler_id}\n"
  "compiler_version=${_compiler_version}\n"
  "compiler_path=${_compiler_path}\n"
  "system_name=${_system_name}\n"
  "system_processor=${_system_processor}\n"
  "cmake_generator=${_cmake_generator}\n"
  "build_type=${_build_type}\n"
  "cxx_standard=${_cxx_standard}\n"
  "cxx_flags=${_cxx_flags}\n"
  "executable_linker_flags=${_linker_flags}\n")
string(SHA256 _expected_canonical_digest "${_canonical}")
read_field("canonical_provenance_sha256" _actual_canonical_digest)
if(NOT _actual_canonical_digest STREQUAL _expected_canonical_digest)
  message(FATAL_ERROR
    "canonical provenance digest: expected ${_expected_canonical_digest}, "
    "got ${_actual_canonical_digest}")
endif()
