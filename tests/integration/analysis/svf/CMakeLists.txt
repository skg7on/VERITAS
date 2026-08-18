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

add_executable(svf_session_integration_test
  SvfSessionTest.cpp
)

target_link_libraries(svf_session_integration_test
  PRIVATE
    veritas_svf_analysis
    veritas_analysis_pipeline
    veritas_unit_test_support
    LLVMCore
    LLVMSupport
)

target_include_directories(svf_session_integration_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src"
)

add_test(NAME SvfSessionTest COMMAND svf_session_integration_test)
set_tests_properties(SvfSessionTest PROPERTIES
  TIMEOUT 30
  LABELS "integration;svf"
)

add_executable(svf_fact_mapper_integration_test
  SvfFactMapperTest.cpp
)

target_link_libraries(svf_fact_mapper_integration_test
  PRIVATE
    veritas_svf_analysis
    veritas_analysis_pipeline
    veritas_unit_test_support
    LLVMCore
    LLVMSupport
)

target_include_directories(svf_fact_mapper_integration_test
  PRIVATE "${PROJECT_SOURCE_DIR}/src"
)

add_test(NAME SvfFactMapperTest COMMAND svf_fact_mapper_integration_test)
set_tests_properties(SvfFactMapperTest PROPERTIES
  TIMEOUT 30
  LABELS "integration;svf"
)
