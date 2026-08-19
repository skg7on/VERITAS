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

# src/tools/CMakeLists.txt
#
# Four VERITAS CLI binaries. At M0 each one only implements `--version`; the
# real ingest / query / diff / explain flows arrive with later milestones.
# Every CLI links veritas_core and picks up the shared warning contract.

foreach(_veritas_cli veritas-build veritas-query veritas-diff veritas-explain)
  add_executable(${_veritas_cli} "${_veritas_cli}.cpp")
  target_link_libraries(${_veritas_cli} PRIVATE veritas_core)
  veritas_add_warnings(${_veritas_cli})
endforeach()

# veritas-build is the project-analysis entry point: it loads the M1 manifest,
# writes the diagnostic manifest, then runs the analyzer.
target_link_libraries(veritas-build PRIVATE veritas_analysis veritas_build)

# veritas-query queries published CPG projections.
target_link_libraries(veritas-query PRIVATE veritas_cpg veritas_summarydb)
