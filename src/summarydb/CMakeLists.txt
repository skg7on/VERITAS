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

# src/summarydb/CMakeLists.txt
#
# Builds `veritas_summarydb`, the metadata and summary storage subsystem.

find_package(SQLite3 REQUIRED)
find_package(RocksDB REQUIRED CONFIG)

add_library(veritas_summarydb
  MetadataStore.cpp
  ObjectStoreRocksDb.cpp
  SummaryRepository.cpp
  SummaryDelta.cpp
  DependencyIndex.cpp
)

# Embed schema/v1.sql and schema/v2.sql as compile-time strings so ApplySchema
# has a single source of truth and no runtime file-path dependency.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/schema/v1.sql" VERITAS_SCHEMA_SQL)
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/schema/schema_v1.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/schema_v1.h"
  @ONLY
)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/schema/v2.sql" VERITAS_SCHEMA_V2_SQL)
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/schema/schema_v2.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/schema_v2.h"
  @ONLY
)

target_include_directories(veritas_summarydb PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
  PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(veritas_summarydb PUBLIC
  veritas::core
  veritas::build
  veritas::summary
  SQLite::SQLite3
  RocksDB::rocksdb
)

target_compile_features(veritas_summarydb PUBLIC cxx_std_20)

veritas_add_warnings(veritas_summarydb)

add_library(veritas::summarydb ALIAS veritas_summarydb)
