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

add_library(veritas_wpa
  CallGraph.cpp
  CppConformanceExecutor.cpp
  CppRuleEvaluator.cpp
  RelationIo.cpp
  SccStateRepository.cpp
  SccGraph.cpp
  SouffleWpaExecutor.cpp
  WpaCoordinator.cpp
  WpaInputMaterializer.cpp
  WpaOrchestrator.cpp
  WpaRunRepository.cpp
)

target_include_directories(veritas_wpa PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(veritas_wpa PUBLIC
  veritas::core
  veritas::facts
  veritas::runtime
  veritas::summary
  veritas::summarydb
)

# The production Souffle engine is linked in-process through the runner's C ABI
# (SouffleRunner.h); the runner target is defined by VeritasSouffle.cmake under
# the souffle engine and resolves at generate time.
if(VERITAS_WPA_ENGINE STREQUAL "souffle")
  target_link_libraries(veritas_wpa PRIVATE veritas_souffle_runner)
endif()

target_compile_features(veritas_wpa PUBLIC cxx_std_20)
veritas_add_warnings(veritas_wpa)

add_library(veritas::wpa ALIAS veritas_wpa)
