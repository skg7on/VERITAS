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


add_executable(veritas_unit_review_tests ReviewTypesTest.cpp)
target_link_libraries(veritas_unit_review_tests PRIVATE veritas_review GTest::gtest_main)
veritas_add_warnings(veritas_unit_review_tests)
gtest_discover_tests(veritas_unit_review_tests DISCOVERY_TIMEOUT 60)
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
add_test(NAME ReviewTypesPythonTest COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${PROJECT_SOURCE_DIR}/plugins/veritas-evidence-review/tests" -p test_review_types.py)
