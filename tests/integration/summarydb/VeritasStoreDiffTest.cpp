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

// The exit-code contract of `veritas-store-diff` — 0 equivalent, 1 different,
// 2 could not compare — is the milestone's deliverable rather than a detail of
// it: the M14 scaling harness branches on the code, and the M13 acceptance
// record calls it "the deliverable". Before this file existed the contract was
// pinned only by a shell transcript pasted into that record, which CI cannot
// re-run — and the transcript covered two of the three codes.
//
// So every case below asserts a code observed on a real invocation of the
// built binary, not a code inferred from reading `main`. The store roots are
// built here with `MetadataStore`, so the case fails for the right reason when
// the instrument regresses.
//
// It lives beside `VeritasDiffTest`, the sibling CLI test for `veritas-diff`,
// and is registered with an explicit `add_test` plus a `TIMEOUT`:
// `gtest_discover_tests` registers the cases it discovers with the default
// timeout and offers no per-test one.

#include "veritas/summarydb/MetadataStore.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#ifndef VERITAS_STORE_DIFF_BINARY
#error "VERITAS_STORE_DIFF_BINARY must be defined by the build system"
#endif

using namespace veritas::summarydb;

namespace {

namespace fs = std::filesystem;

struct CliResult {
  int exit_code = -1;
  // The two streams are captured merged, by one `2>&1` pipe, so this holds
  // stderr as well. The name is kept to match the sibling CLI tests rather than
  // invented, but no case below depends on which stream a message came from.
  std::string stdout_text;
};

std::string ShellQuote(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('\'');
  for (const char c : value) {
    if (c == '\'') {
      out.append("'\\''");
    } else {
      out.push_back(c);
    }
  }
  out.push_back('\'');
  return out;
}

CliResult RunStoreDiff(const std::vector<std::string>& arguments) {
  std::string command = ShellQuote(VERITAS_STORE_DIFF_BINARY);
  for (const auto& argument : arguments) {
    command.push_back(' ');
    command.append(ShellQuote(argument));
  }
  command.append(" 2>&1");

  CliResult result;
  FILE* pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr) {
    return result;
  }
  std::array<char, 4096> buffer{};
  while (::fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
    result.stdout_text.append(buffer.data());
  }
  const int status = ::pclose(pipe);
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return result;
}

// This binary is built `-fno-exceptions`, so every filesystem call here uses
// the `std::error_code` overload: the throwing one is a `std::terminate` on
// failure, which aborts the binary and takes the other cases' results with it
// instead of reporting one failure.
fs::path TempDirectory() {
  std::error_code error;
  const auto path = fs::temp_directory_path(error);
  if (error) {
    ADD_FAILURE() << "cannot resolve the temporary directory: "
                  << error.message();
  }
  return path;
}

void RemoveAll(const fs::path& path) {
  std::error_code error;
  fs::remove_all(path, error);
  EXPECT_FALSE(error) << "cannot remove " << path.string() << ": "
                      << error.message();
}

// A per-process token: a test name alone does not distinguish two suites, and
// this repository runs several worktrees at once, so two processes running this
// binary would otherwise share one set of store roots — and `SetUp` clears the
// roots before use, which makes that not a stale file but one process deleting
// another's store mid-test.
std::string ProcessToken() {
  return std::to_string(static_cast<unsigned long long>(::getpid()));
}

// Creates a one-table store at `root`. Deliberately schema-free and free of any
// wall-clock column: this file tests the tool's exit-code contract, not the
// projection, and a store with a moving column in it would make the equivalence
// case depend on the projection instead of on the code path under test.
veritas::Status CreateStore(const fs::path& root, std::string_view payload) {
  // `sqlite3_open` creates the database file but not the directory above it.
  std::error_code error;
  fs::create_directories(root, error);
  if (error) {
    return veritas::Status::Internal("cannot create " + root.string() + ": " +
                                     error.message());
  }

  auto store = MetadataStore::Open(root / "metadata.db");
  if (!store.ok()) return store.status();
  auto created =
      store->Execute("CREATE TABLE alpha (id TEXT, payload TEXT)", {});
  if (!created.ok()) return created;
  return store->Execute("INSERT INTO alpha (id, payload) VALUES ('a1', ?)",
                        {std::string(payload)});
}

// Changes the single cell, so the two stores differ in content and in nothing
// else — same tables, same row count, same schema.
veritas::Status ChangePayload(const fs::path& root, std::string_view payload) {
  auto store = MetadataStore::Open(root / "metadata.db");
  if (!store.ok()) return store.status();
  return store->Execute("UPDATE alpha SET payload = ? WHERE id = 'a1'",
                        {std::string(payload)});
}

class VeritasStoreDiffTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    root_ = TempDirectory() /
            ("veritas_store_diff_" + ProcessToken() + "_" + info->name());
    RemoveAll(root_);
  }

  void TearDown() override { RemoveAll(root_); }

  fs::path Left() const { return root_ / "left"; }
  fs::path Right() const { return root_ / "right"; }

  fs::path root_;
};

// The primary use case: two stores of identical content compare equivalent.
// This is the code the M14 harness reads, and 0 is the only value it treats as
// a pass.
TEST_F(VeritasStoreDiffTest, IdenticalStoresExitZero) {
  ASSERT_TRUE(CreateStore(Left(), "p1").ok());
  ASSERT_TRUE(CreateStore(Right(), "p1").ok());

  const auto result = RunStoreDiff({Left().string(), Right().string()});
  EXPECT_EQ(result.exit_code, 0) << result.stdout_text;
  EXPECT_NE(result.stdout_text.find("stores are equivalent"),
            std::string::npos)
      << result.stdout_text;
  // The count is asserted with the code, because it is what makes the pass
  // falsifiable: a tool that compared nothing at all also prints "equivalent".
  // The fixture has exactly one table (`alpha`), so the expected count is not a
  // guess about the store — an extra compared table would fail here.
  EXPECT_NE(result.stdout_text.find("(1 tables compared)"), std::string::npos)
      << result.stdout_text;
}

// One changed cell is the smallest real difference, and it must show up as 1
// rather than as an error or as equivalence.
TEST_F(VeritasStoreDiffTest, AChangedCellExitsOne) {
  ASSERT_TRUE(CreateStore(Left(), "p1").ok());
  ASSERT_TRUE(CreateStore(Right(), "p1").ok());
  ASSERT_TRUE(ChangePayload(Right(), "p2").ok());

  const auto result = RunStoreDiff({Left().string(), Right().string()});
  EXPECT_EQ(result.exit_code, 1) << result.stdout_text;
  EXPECT_NE(result.stdout_text.find("stores differ"), std::string::npos)
      << result.stdout_text;
  // Named, not merely counted: the name is what a reader takes back to the
  // projection table.
  EXPECT_NE(result.stdout_text.find("differs: alpha"), std::string::npos)
      << result.stdout_text;
}

// A store root that holds no store is "could not compare", not "different" and
// certainly not "equivalent". The distinction matters because a typo in a path
// otherwise reads as a clean pass.
TEST_F(VeritasStoreDiffTest, AMissingStoreRootExitsTwo) {
  ASSERT_TRUE(CreateStore(Left(), "p1").ok());
  // `Right()` is never created.

  const auto result = RunStoreDiff({Left().string(), Right().string()});
  EXPECT_EQ(result.exit_code, 2) << result.stdout_text;
  EXPECT_EQ(result.stdout_text.find("stores are equivalent"),
            std::string::npos)
      << result.stdout_text;
  EXPECT_NE(result.stdout_text.find("no metadata store at"), std::string::npos)
      << result.stdout_text;
}

// The documented ordering, pinned because it is a property a caller can depend
// on and one a refactor can quietly lose: when both stores are unreadable, the
// failure reported is the left one, because the left store is dumped first and
// the right is never opened. Both paths are missing here and are different
// strings, so a tool that had swapped the order would name the other one.
TEST_F(VeritasStoreDiffTest, AnUnreadableLeftStoreIsReportedFirst) {
  const fs::path missing_left = root_ / "missing-left";
  const fs::path missing_right = root_ / "missing-right";

  const auto result =
      RunStoreDiff({missing_left.string(), missing_right.string()});
  EXPECT_EQ(result.exit_code, 2) << result.stdout_text;
  EXPECT_NE(result.stdout_text.find(missing_left.string()), std::string::npos)
      << result.stdout_text;
  EXPECT_EQ(result.stdout_text.find(missing_right.string()), std::string::npos)
      << result.stdout_text;
}

// Argument-count errors are 2 as well, so a caller cannot mistake "you called
// me wrong" for "the stores are different".
TEST_F(VeritasStoreDiffTest, NoArgumentsExitsTwo) {
  const auto result = RunStoreDiff({});
  EXPECT_EQ(result.exit_code, 2) << result.stdout_text;
  EXPECT_NE(result.stdout_text.find("usage:"), std::string::npos)
      << result.stdout_text;
}

// Every sibling CLI answers `--version` and advertises it in its usage text,
// and a differential conformance harness is the most likely thing in this
// repository to record a tool's version. Asserting it here is what keeps the
// fifth CLI from being the one that does not.
TEST_F(VeritasStoreDiffTest, VersionFlagExitsZero) {
  const auto result = RunStoreDiff({"--version"});
  EXPECT_EQ(result.exit_code, 0) << result.stdout_text;
  EXPECT_NE(result.stdout_text.find("VERITAS "), std::string::npos)
      << result.stdout_text;
  EXPECT_EQ(result.stdout_text.find("usage:"), std::string::npos)
      << result.stdout_text;
}

}  // namespace
