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

#ifndef VERITAS_SUMMARYDB_OBJECT_STORE_H_
#define VERITAS_SUMMARYDB_OBJECT_STORE_H_

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "veritas/core/Status.h"

namespace veritas::summarydb {

// ObjectStore is a content-addressed storage interface for immutable summary
// objects. All summaries are stored by their content hash and never modified
// after insertion.
class ObjectStore {
 public:
  virtual ~ObjectStore() = default;

  // Store an object if it doesn't already exist. Idempotent: if an object with
  // the same key already exists, this is a no-op and returns Ok.
  // Returns Internal if the existing object has different content.
  virtual veritas::Status PutIfAbsent(std::string_view key,
                                      std::span<const std::byte> bytes) = 0;

  // Retrieve an object by key. Returns NotFound if the key doesn't exist.
  virtual veritas::StatusOr<std::vector<std::byte>> Get(
      std::string_view key) const = 0;

  // Check if an object exists.
  virtual bool Exists(std::string_view key) const = 0;
};

// Factory function for creating in-memory stores (used in tests).
std::unique_ptr<ObjectStore> CreateInMemoryObjectStore();

// Factory function for creating RocksDB stores.
veritas::StatusOr<std::unique_ptr<ObjectStore>> CreateObjectStore(
    const std::string& db_path);

}  // namespace veritas::summarydb

#endif  // VERITAS_SUMMARYDB_OBJECT_STORE_H_
