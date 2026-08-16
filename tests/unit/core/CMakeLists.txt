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

# tests/unit/core/CMakeLists.txt
#
# Unit tests for veritas_core. GoogleTest was included from Dependencies.cmake
# (which pulls in the GoogleTest CMake helper module), so gtest_discover_tests
# is available here.

add_executable(StatusTest StatusTest.cpp)
target_link_libraries(StatusTest PRIVATE
  veritas_core
  GTest::gtest
  GTest::gtest_main
)
veritas_add_warnings(StatusTest)
gtest_discover_tests(StatusTest)

add_executable(VersionTest VersionTest.cpp)
target_link_libraries(VersionTest PRIVATE
  veritas_core
  GTest::gtest
  GTest::gtest_main
)
veritas_add_warnings(VersionTest)
gtest_discover_tests(VersionTest)
