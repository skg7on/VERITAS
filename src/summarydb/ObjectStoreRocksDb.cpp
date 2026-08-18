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
#include <rocksdb/merge_operator.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/status.h>

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

// Custom merge operator that implements put-if-absent with content verification.
// For content-addressed storage, identical keys MUST have identical content.
class PutIfAbsentMergeOperator : public rocksdb::MergeOperator {
 public:
  bool FullMergeV2(const MergeOperationInput& merge_in,
                   MergeOperationOutput* merge_out) const override {
    // If key doesn't exist (no existing value), use the first operand as the value
    if (!merge_in.existing_value) {
      merge_out->new_value = merge_in.operand_list[0].ToString();
      return true;
    }

    // Key exists - verify all operands match the existing value
    for (const auto& operand : merge_in.operand_list) {
      if (operand != *merge_in.existing_value) {
        // Content mismatch - this is a CAS invariant violation
        // Return false to fail the merge, which will fail the Merge() call
        return false;
      }
    }

    // All operands match existing value - keep existing value unchanged
    merge_out->existing_operand = merge_out->existing_value;
    return true;
  }

  bool PartialMerge(const rocksdb::Slice& /*key*/,
                    const rocksdb::Slice& left_operand,
                    const rocksdb::Slice& right_operand,
                    std::string* new_value,
                    rocksdb::Logger* /*logger*/) const override {
    // Partial merge: verify operands are identical
    if (left_operand != right_operand) {
      return false;  // Content mismatch
    }
    *new_value = left_operand.ToString();
    return true;
  }

  const char* Name() const override { return "PutIfAbsentMergeOperator"; }
};

}  // namespace

// RocksDB-backed ObjectStore implementation.
class ObjectStoreRocksDb : public ObjectStore {
 public:
  static veritas::StatusOr<std::unique_ptr<ObjectStore>> Open(
      const std::string& db_path) {
    rocksdb::Options options;
    options.create_if_missing = true;
    options.merge_operator = std::make_shared<PutIfAbsentMergeOperator>();

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

    // Use Merge with custom operator to atomically enforce put-if-absent with
    // content verification. RocksDB guarantees atomicity of the merge operation.
    rocksdb::Status status = db_->Merge(rocksdb::WriteOptions(), key_str,
                                        value_to_write);
    if (!status.ok()) {
      return veritas::Status::Internal("RocksDB Merge failed: " +
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
