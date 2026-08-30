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

add_executable(SouffleRunnerTest SouffleRunnerTest.cpp)
target_link_libraries(SouffleRunnerTest PRIVATE
  veritas::facts
  veritas::wpa
  GTest::gtest_main
)
target_compile_definitions(SouffleRunnerTest PRIVATE
  VERITAS_HAS_SOUFFLE=$<BOOL:${VERITAS_HAS_SOUFFLE}>
  VERITAS_SOUFFLE_EXECUTABLE="${VERITAS_SOUFFLE_EXECUTABLE}"
  VERITAS_FACT_RULE_DIR="${PROJECT_SOURCE_DIR}/src/facts/rules"
)
veritas_add_warnings(SouffleRunnerTest)
gtest_discover_tests(SouffleRunnerTest DISCOVERY_TIMEOUT 60)

# Proves the six semantic-key functors produce byte-identical keys to
# SemanticKeyCodec. Links veritas-souffle-functors and libsouffle, so it is
# built with RTTI/exceptions and only under the production Souffle engine.
if(VERITAS_WPA_ENGINE STREQUAL "souffle")
  add_executable(SemanticKeyFunctorTest SemanticKeyFunctorTest.cpp)
  target_include_directories(SemanticKeyFunctorTest PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${VERITAS_SOUFFLE_SOURCE_DIR}/src/include
  )
  target_link_libraries(SemanticKeyFunctorTest PRIVATE
    veritas_souffle_functors
    libsouffle
    GTest::gtest_main
  )
  target_compile_options(SemanticKeyFunctorTest PRIVATE -frtti -fexceptions)
  gtest_discover_tests(SemanticKeyFunctorTest DISCOVERY_TIMEOUT 60)
endif()
