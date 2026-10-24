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

# tests/integration/wpa/CMakeLists.txt
#
# The two worker programs stay separate executables: they are subprocess
# entry points, not tests. Everything else shares one executable.

if(VERITAS_WPA_ENGINE STREQUAL "souffle")
  add_executable(WpaEmptyOutputWorker WpaEmptyOutputWorker.cpp)
  veritas_add_warnings(WpaEmptyOutputWorker)

  add_executable(WpaLimitProbeWorker WpaLimitProbeWorker.cpp)
  target_compile_definitions(WpaLimitProbeWorker PRIVATE
    VERITAS_EXPECTED_MEMORY_BYTES=17179869184ULL
  )
  veritas_add_warnings(WpaLimitProbeWorker)
endif()

set(veritas_integration_wpa_sources WpaEndToEndTest.cpp)
if(VERITAS_WPA_ENGINE STREQUAL "souffle")
  list(APPEND veritas_integration_wpa_sources
    WpaExecutorConformanceTest.cpp
    SouffleWpaExecutorTest.cpp
  )
endif()

add_executable(veritas_integration_wpa_tests ${veritas_integration_wpa_sources})
target_link_libraries(veritas_integration_wpa_tests PRIVATE
  veritas::summarydb
  veritas::wpa
  GTest::gtest_main
)

if(VERITAS_WPA_ENGINE STREQUAL "souffle")
  target_compile_definitions(veritas_integration_wpa_tests PRIVATE
    VERITAS_SOUFFLE_WORKER="$<TARGET_FILE:veritas_souffle_worker>"
    VERITAS_EMPTY_OUTPUT_WORKER="$<TARGET_FILE:WpaEmptyOutputWorker>"
    VERITAS_LIMIT_PROBE_WORKER="$<TARGET_FILE:WpaLimitProbeWorker>"
  )
  add_dependencies(veritas_integration_wpa_tests
    veritas_souffle_worker
    veritas_souffle_provenance
    WpaEmptyOutputWorker
    WpaLimitProbeWorker
  )
endif()

veritas_add_warnings(veritas_integration_wpa_tests)
gtest_discover_tests(veritas_integration_wpa_tests DISCOVERY_TIMEOUT 60)

if(VERITAS_WPA_ENGINE STREQUAL "souffle")
  veritas_m9_entry_test(WpaExecutorConformanceTest engine-conformance
    veritas_integration_wpa_tests)
  veritas_m9_entry_test(SouffleWpaExecutorTest souffle-production
    veritas_integration_wpa_tests)

  add_test(NAME SouffleProvenanceTest
    COMMAND ${CMAKE_COMMAND}
      "-DMANIFEST=${CMAKE_BINARY_DIR}/souffle-provenance.json"
      "-DSOUFFLE=$<TARGET_FILE:souffle>"
      "-DRUNNER=$<TARGET_FILE:veritas_souffle_runner>"
      "-DFUNCTOR=$<TARGET_FILE:veritas_souffle_functors>"
      "-DREACH_BUNDLE=${VERITAS_SOUFFLE_GEN_DIR}/v2_reach.cpp"
      "-DMEMORY_EFFECTS_BUNDLE=${VERITAS_SOUFFLE_GEN_DIR}/v2_memory_effects.cpp"
      "-DFLOW_BUNDLE=${VERITAS_SOUFFLE_GEN_DIR}/v2_global_flow.cpp"
      "-DEFFECTS_BUNDLE=${VERITAS_SOUFFLE_GEN_DIR}/v2_effects.cpp"
      "-DEXPECTED_REVISION=${VERITAS_SOUFFLE_PINNED_REVISION}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifySouffleProvenance.cmake"
  )
endif()
