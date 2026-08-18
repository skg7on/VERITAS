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

#include <algorithm>
#include <cstddef>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>

namespace veritas::summarydb {

namespace {

// In-memory ObjectStore implementation for testing.
class InMemoryObjectStore : public ObjectStore {
 public:
  veritas::Status PutIfAbsent(std::string_view key,
                              std::span<const std::byte> bytes) override {
    std::string key_str(key);
    auto it = store_.find(key_str);

    if (it != store_.end()) {
      // Key exists, verify content matches
      if (it->second.size() != bytes.size() ||
          !std::equal(it->second.begin(), it->second.end(), bytes.begin())) {
        return veritas::Status::Internal(
            "ObjectStore: key exists with different content");
      }
      return veritas::Status::Ok();
    }

    // Insert new object
    store_[key_str] =
        std::vector<std::byte>(bytes.begin(), bytes.end());
    return veritas::Status::Ok();
  }

  veritas::StatusOr<std::vector<std::byte>> Get(
      std::string_view key) const override {
    auto it = store_.find(std::string(key));
    if (it == store_.end()) {
      return veritas::Status::NotFound("ObjectStore: key not found");
    }
    return it->second;
  }

  bool Exists(std::string_view key) const override {
    return store_.find(std::string(key)) != store_.end();
  }

  size_t ObjectCount() const { return store_.size(); }

 private:
  std::map<std::string, std::vector<std::byte>> store_;
};

}  // namespace

// RocksDB-backed ObjectStore implementation.
class ObjectStoreRocksDb : public ObjectStore {
 public:
  static veritas::StatusOr<std::unique_ptr<ObjectStore>> Open(
      const std::string& db_path) {
    rocksdb::Options options;
    options.create_if_missing = true;

    std::unique_ptr<rocksdb::DB> db;
    rocksdb::Status status = rocksdb::DB::Open(options, db_path, &db);
    if (!status.ok()) {
      return veritas::Status::Internal("Failed to open RocksDB: " +
                                       status.ToString());
    }

    return std::unique_ptr<ObjectStore>(
        new ObjectStoreRocksDb(std::move(db)));
  }

  ~ObjectStoreRocksDb() override = default;

  veritas::Status PutIfAbsent(std::string_view key,
                              std::span<const std::byte> bytes) override {
    std::string key_str(key);
    std::string value_to_write(reinterpret_cast<const char*>(bytes.data()),
                               bytes.size());

    // Check if key exists
    std::string existing_value;
    rocksdb::Status status = db_->Get(rocksdb::ReadOptions(), key_str,
                                      &existing_value);

    if (status.ok()) {
      // Key exists, verify content matches
      if (existing_value != value_to_write) {
        return veritas::Status::Internal(
            "ObjectStore: key exists with different content");
      }
      return veritas::Status::Ok();
    }

    if (!status.IsNotFound()) {
      return veritas::Status::Internal("RocksDB Get failed: " +
                                       status.ToString());
    }

    // Key doesn't exist, insert it
    status = db_->Put(rocksdb::WriteOptions(), key_str, value_to_write);
    if (!status.ok()) {
      return veritas::Status::Internal("RocksDB Put failed: " +
                                       status.ToString());
    }

    return veritas::Status::Ok();
  }

  veritas::StatusOr<std::vector<std::byte>> Get(
      std::string_view key) const override {
    std::string value;
    rocksdb::Status status = db_->Get(rocksdb::ReadOptions(),
                                      std::string(key), &value);

    if (status.IsNotFound()) {
      return veritas::Status::NotFound("ObjectStore: key not found");
    }

    if (!status.ok()) {
      return veritas::Status::Internal("RocksDB Get failed: " +
                                       status.ToString());
    }

    std::vector<std::byte> bytes(value.size());
    std::memcpy(bytes.data(), value.data(), value.size());
    return bytes;
  }

  bool Exists(std::string_view key) const override {
    std::string value;
    rocksdb::Status status = db_->Get(rocksdb::ReadOptions(),
                                      std::string(key), &value);
    return status.ok();
  }

 private:
  explicit ObjectStoreRocksDb(std::unique_ptr<rocksdb::DB> db)
      : db_(std::move(db)) {}

  std::unique_ptr<rocksdb::DB> db_;
};

// Factory function for creating in-memory stores (used in tests).
std::unique_ptr<ObjectStore> CreateInMemoryObjectStore() {
  return std::make_unique<InMemoryObjectStore>();
}

// Factory function for creating RocksDB stores.
veritas::StatusOr<std::unique_ptr<ObjectStore>> CreateObjectStore(
    const std::string& db_path) {
  return ObjectStoreRocksDb::Open(db_path);
}

}  // namespace veritas::summarydb
