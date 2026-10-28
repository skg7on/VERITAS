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

#ifndef VERITAS_REVIEW_SNAPSHOT_CAPTURE_H_
#define VERITAS_REVIEW_SNAPSHOT_CAPTURE_H_
#include "veritas/review/ProgramScope.h"
#include <filesystem>
#include <memory>
namespace veritas::review {
// Trusted native invocation API. The public capture wire request accepts only
// operator selection keys; host adapters must resolve those before calling
// this.
struct SnapshotRequest {
  std::filesystem::path project_root;
  std::string repository_id;
  std::string base_ref = "HEAD";
  // Empty means every tracked path in the index and base commit.
  std::vector<std::string> selected_tracked, selected_untracked;
  // Explicit trusted file/directory selections, including generated headers,
  // build configuration, response files, tests and documentation.
  std::vector<std::string> selected_artifacts;
  std::string compile_commands_path, program_manifest_path, program_target_key;
  std::string membership_source = "EXPLICIT_DECLARATION";
  std::vector<ProviderVersion> provider_versions;
  uint64_t max_files = 10000, max_bytes = 64 * 1024 * 1024;
};
struct CapturedCompileCommand {
  std::string directory, file;
  std::vector<std::string> arguments;
};
struct SnapshotData;
class ReviewSnapshot {
public:
  const ReviewBinding &binding() const;
  const std::vector<CapturedCompileCommand> &compiler_commands() const;
  std::vector<std::string> captured_paths() const;
  // Lexical resolution against the captured root; never consults live files.
  // Root itself is returned as "." for include-directory operands.
  StatusOr<std::string> CompilerPath(std::string_view directory,
                                    std::string_view path) const;
  const ProgramScope &program_scope() const;
  const Coverage &compiler_coverage() const;
  const std::vector<std::string> &excluded() const;
  const std::vector<std::string> &unavailable() const;
  // Reads only captured immutable bytes. Missing/deleted/excluded inputs yield
  // NotFound, unsafe paths yield InvalidArgument. No live filesystem reads.
  StatusOr<std::string> Read(std::string_view path) const;
  StatusOr<std::string> ReadBase(std::string_view path) const;
  // Context identity excludes binding (snapshot identity is already a premise).
  // Invalid UTF-8, unsupported kinds or invalid byte boundaries are refused.
  StatusOr<ContextArtifact> Context(std::string_view path,
                                    std::string_view kind, uint64_t start,
                                    uint64_t end) const;
  // Creates a NEW private worker tree of captured inputs, read-only. Existing
  // destinations or symlink components are refused; no live source is copied.
  Status Materialize(const std::filesystem::path &destination) const;
  StatusOr<bool> LiveTreeAdvanced() const;

private:
  explicit ReviewSnapshot(std::shared_ptr<const SnapshotData> data)
      : data_(std::move(data)) {}
  std::shared_ptr<const SnapshotData> data_;
  friend StatusOr<ReviewSnapshot>
  CaptureReviewSnapshot(const SnapshotRequest &);
};
// Failure-atomic in-memory publication. Capture is bounded and refuses with
// FailedPrecondition (retryable) if index/base/selected bytes or directory
// membership change during capture. Symlinks and traversal are InvalidArgument.
StatusOr<ReviewSnapshot> CaptureReviewSnapshot(const SnapshotRequest &);
} // namespace veritas::review
#endif
