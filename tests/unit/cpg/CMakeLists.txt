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

add_executable(ThinCpgTest
  ThinCpgTest.cpp
)
target_link_libraries(ThinCpgTest PRIVATE
  veritas::cpg
  GTest::gtest
  GTest::gtest_main
)
gtest_discover_tests(ThinCpgTest)

add_executable(CpgRepositoryTest
  CpgRepositoryTest.cpp
)
target_include_directories(CpgRepositoryTest PRIVATE
  "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(CpgRepositoryTest PRIVATE
  veritas::cpg
  veritas_summarydb
  veritas::core
  GTest::gtest
  GTest::gtest_main
)
gtest_discover_tests(CpgRepositoryTest)

add_executable(CpgCanonicalizerTest
  CpgCanonicalizerTest.cpp
)
target_include_directories(CpgCanonicalizerTest PRIVATE
  "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(CpgCanonicalizerTest PRIVATE
  veritas::cpg
  veritas::core
  GTest::gtest
  GTest::gtest_main
)
gtest_discover_tests(CpgCanonicalizerTest)
