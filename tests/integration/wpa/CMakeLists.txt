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

add_executable(WpaEndToEndTest WpaEndToEndTest.cpp)
target_link_libraries(WpaEndToEndTest PRIVATE
  veritas::summarydb
  veritas::wpa
  GTest::gtest_main
)
veritas_add_warnings(WpaEndToEndTest)
gtest_discover_tests(WpaEndToEndTest DISCOVERY_TIMEOUT 60)

if(VERITAS_WPA_ENGINE STREQUAL "souffle")
  add_executable(WpaEmptyOutputWorker WpaEmptyOutputWorker.cpp)
  veritas_add_warnings(WpaEmptyOutputWorker)

  add_executable(WpaLimitProbeWorker WpaLimitProbeWorker.cpp)
  target_compile_definitions(WpaLimitProbeWorker PRIVATE
    VERITAS_EXPECTED_MEMORY_BYTES=17179869184ULL
  )
  veritas_add_warnings(WpaLimitProbeWorker)

  add_executable(WpaExecutorConformanceTest WpaExecutorConformanceTest.cpp)
  target_link_libraries(WpaExecutorConformanceTest PRIVATE
    veritas::wpa
    GTest::gtest_main
  )
  target_compile_definitions(WpaExecutorConformanceTest PRIVATE
    VERITAS_SOUFFLE_WORKER="$<TARGET_FILE:veritas_souffle_worker>"
  )
  add_dependencies(WpaExecutorConformanceTest
    veritas_souffle_worker
    veritas_souffle_provenance
  )
  veritas_add_warnings(WpaExecutorConformanceTest)
  gtest_discover_tests(WpaExecutorConformanceTest DISCOVERY_TIMEOUT 60)
veritas_m9_entry_test(WpaExecutorConformanceTest engine-conformance)

  add_executable(SouffleWpaExecutorTest SouffleWpaExecutorTest.cpp)
  target_link_libraries(SouffleWpaExecutorTest PRIVATE
    veritas::wpa
    GTest::gtest_main
  )
  target_compile_definitions(SouffleWpaExecutorTest PRIVATE
    VERITAS_SOUFFLE_WORKER="$<TARGET_FILE:veritas_souffle_worker>"
    VERITAS_EMPTY_OUTPUT_WORKER="$<TARGET_FILE:WpaEmptyOutputWorker>"
    VERITAS_LIMIT_PROBE_WORKER="$<TARGET_FILE:WpaLimitProbeWorker>"
  )
  add_dependencies(SouffleWpaExecutorTest
    veritas_souffle_worker
    veritas_souffle_provenance
    WpaEmptyOutputWorker
    WpaLimitProbeWorker
  )
  veritas_add_warnings(SouffleWpaExecutorTest)
  gtest_discover_tests(SouffleWpaExecutorTest DISCOVERY_TIMEOUT 60)
veritas_m9_entry_test(SouffleWpaExecutorTest souffle-production)

  add_test(NAME SouffleProvenanceTest
    COMMAND ${CMAKE_COMMAND}
      "-DMANIFEST=${CMAKE_BINARY_DIR}/souffle-provenance.json"
      "-DSOUFFLE=$<TARGET_FILE:souffle>"
      "-DRUNNER=$<TARGET_FILE:veritas_souffle_runner>"
      "-DFUNCTOR=$<TARGET_FILE:veritas_souffle_functors>"
      "-DREACH_BUNDLE=${VERITAS_SOUFFLE_GEN_DIR}/v2_reach.cpp"
      "-DMAYWRITE_BUNDLE=${VERITAS_SOUFFLE_GEN_DIR}/v2_maywrite.cpp"
      "-DEXPECTED_REVISION=${VERITAS_SOUFFLE_PINNED_REVISION}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifySouffleProvenance.cmake"
  )
endif()
