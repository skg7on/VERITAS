// Copyright 2026 VERITAS Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "veritas/summarydb/ObjectStore.h"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace veritas::summarydb {
namespace {

std::vector<std::byte> MakeBytes(const std::string& str) {
  std::vector<std::byte> bytes(str.size());
  std::memcpy(bytes.data(), str.data(), str.size());
  return bytes;
}

TEST(ObjectStoreTest, PutIfAbsentDeduplicatesByKey) {
  auto store = CreateInMemoryObjectStore();
  auto bytes = MakeBytes("content");

  EXPECT_TRUE(store->PutIfAbsent("summary:sha256:abc", bytes).ok());
  EXPECT_TRUE(store->PutIfAbsent("summary:sha256:abc", bytes).ok());

  // Verify only one object was stored
  EXPECT_TRUE(store->Exists("summary:sha256:abc"));
}

TEST(ObjectStoreTest, PutIfAbsentRejectsDifferentContent) {
  auto store = CreateInMemoryObjectStore();
  auto bytes1 = MakeBytes("content1");
  auto bytes2 = MakeBytes("content2");

  EXPECT_TRUE(store->PutIfAbsent("summary:sha256:abc", bytes1).ok());

  auto status = store->PutIfAbsent("summary:sha256:abc", bytes2);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kInternal);
}

TEST(ObjectStoreTest, GetReturnsStoredContent) {
  auto store = CreateInMemoryObjectStore();
  auto bytes = MakeBytes("test content");

  EXPECT_TRUE(store->PutIfAbsent("summary:sha256:abc", bytes).ok());

  auto result = store->Get("summary:sha256:abc");
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(*result, bytes);
}

TEST(ObjectStoreTest, GetReturnsNotFoundForMissingKey) {
  auto store = CreateInMemoryObjectStore();

  auto result = store->Get("summary:sha256:missing");
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), StatusCode::kNotFound);
}

TEST(ObjectStoreTest, ExistsReturnsTrueForStoredKey) {
  auto store = CreateInMemoryObjectStore();
  auto bytes = MakeBytes("content");

  EXPECT_FALSE(store->Exists("summary:sha256:abc"));
  EXPECT_TRUE(store->PutIfAbsent("summary:sha256:abc", bytes).ok());
  EXPECT_TRUE(store->Exists("summary:sha256:abc"));
}

TEST(ObjectStoreTest, RocksDbPutAndGet) {
  std::filesystem::path temp_dir = std::filesystem::temp_directory_path() /
                                   "veritas_test_rocksdb";
  std::filesystem::remove_all(temp_dir);

  auto store_result = CreateObjectStore(temp_dir.string());
  ASSERT_TRUE(store_result.ok()) << store_result.status().message();
  auto store = std::move(*store_result);

  auto bytes = MakeBytes("rocksdb content");
  EXPECT_TRUE(store->PutIfAbsent("summary:sha256:rocks", bytes).ok());

  auto result = store->Get("summary:sha256:rocks");
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(*result, bytes);

  std::filesystem::remove_all(temp_dir);
}

TEST(ObjectStoreTest, RocksDbIdempotentPut) {
  std::filesystem::path temp_dir = std::filesystem::temp_directory_path() /
                                   "veritas_test_rocksdb_idempotent";
  std::filesystem::remove_all(temp_dir);

  auto store_result = CreateObjectStore(temp_dir.string());
  ASSERT_TRUE(store_result.ok());
  auto store = std::move(*store_result);

  auto bytes = MakeBytes("content");
  EXPECT_TRUE(store->PutIfAbsent("summary:sha256:xyz", bytes).ok());
  EXPECT_TRUE(store->PutIfAbsent("summary:sha256:xyz", bytes).ok());

  std::filesystem::remove_all(temp_dir);
}

}  // namespace
}  // namespace veritas::summarydb
