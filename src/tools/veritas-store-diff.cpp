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

// veritas-store-diff — compare two VERITAS store roots table by table.
//
// This CLI is the user-facing surface of the M13 store-equivalence
// instrument. Its exit code is the deliverable: the M14 scaling harness and
// Task 7's projection determination both branch on it, so the contract is
// 0 = equivalent, 1 = different, 2 = could not compare. Every store is read
// through `MetadataStore::OpenReadOnly`, so this tool never creates,
// migrates, or writes anything.

#include <cstring>
#include <filesystem>
#include <iostream>
#include <string_view>

#include "veritas/core/Status.h"
#include "veritas/core/Version.h"
#include "veritas/summarydb/StoreEquivalence.h"

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kUsage =
    "usage:\n"
    "  veritas-store-diff --version\n"
    "  veritas-store-diff <left-store-root> <right-store-root>\n"
    "\n"
    "Compares the metadata.db of two store roots table by table, using the\n"
    "determined projection recorded in\n"
    "docs/specs/milestones/m13-scale-profile-acceptance-record.md:\n"
    "run-scoped columns are excluded, rows are ordered deterministically, and\n"
    "each table is digested. Exits 0 when the stores are equivalent, 1 when\n"
    "they differ, and 2 on error.\n";

int ReportError(const veritas::Status& status) {
  std::cerr << "veritas-store-diff: " << status.message() << '\n';
  return 2;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    std::cout << veritas::FormatVersion(veritas::GetVersion()) << '\n';
    return 0;
  }
  if (argc != 3) {
    std::cerr << kUsage;
    return 2;
  }

  // The arguments are store *roots* — the directories handed to `--output` —
  // not metadata.db paths, because that is how a user has them.
  const fs::path left = fs::path(argv[1]) / "metadata.db";
  const fs::path right = fs::path(argv[2]) / "metadata.db";

  // The library function, not a re-composition of it. Its documented ordering
  // is preserved: it dumps the left store and reports a failure there before it
  // ever opens the right one, so an unreadable left store is named as such
  // rather than reported as a comparison of nothing.
  const auto comparison = veritas::summarydb::CompareStoreFiles(left, right);
  if (!comparison.ok()) return ReportError(comparison.status());

  if (comparison->equal) {
    // Keep the count: two stores that share no compared table compare equal,
    // and the number is the only thing that makes that vacuous pass visible.
    // Removing it as noise would restore a false green.
    std::cout << "stores are equivalent (" << comparison->tables_compared
              << " tables compared)\n";
    return 0;
  }

  std::cout << "stores differ\n";
  for (const auto& table : comparison->differing) {
    std::cout << "  differs: " << table.table << " (left " << table.left_rows
              << " rows, right " << table.right_rows << " rows)\n";
  }
  for (const auto& table : comparison->left_only) {
    std::cout << "  left only: " << table << '\n';
  }
  for (const auto& table : comparison->right_only) {
    std::cout << "  right only: " << table << '\n';
  }
  return 1;
}
