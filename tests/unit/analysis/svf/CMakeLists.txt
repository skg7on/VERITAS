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

add_executable(svf_config_test
  SvfConfigTest.cpp
)

target_link_libraries(svf_config_test
  PRIVATE
    veritas_svf_analysis
    veritas_unit_test_support
)

target_include_directories(svf_config_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src"
)

add_test(NAME SvfConfigTest COMMAND svf_config_test)
set_tests_properties(SvfConfigTest PROPERTIES TIMEOUT 10)
