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
# Invoked by VeritasSouffle.cmake after the vendored souffle executable is built.
# Records the pinned source revision and the built executable's SHA-256, so the
# run manifest can carry exact engine/toolchain identity without re-hashing at
# runtime. The revision is the pinned checkout's constant; the digest is derived
# from the built binary, so provenance is reproducible by construction.

foreach(_required IN ITEMS VERITAS_SOUFFLE_EXECUTABLE VERITAS_SOUFFLE_REVISION
                    VERITAS_SOUFFLE_PROVENANCE_OUTPUT)
  if(NOT DEFINED ${_required})
    message(FATAL_ERROR "WriteSouffleProvenance: missing -D${_required}")
  endif()
endforeach()

file(SHA256 "${VERITAS_SOUFFLE_EXECUTABLE}" _veritas_souffle_digest)
file(WRITE "${VERITAS_SOUFFLE_PROVENANCE_OUTPUT}"
  "{\"source_revision\":\"${VERITAS_SOUFFLE_REVISION}\",\"executable_sha256\":\"${_veritas_souffle_digest}\"}\n")
