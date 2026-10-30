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

#ifndef VERITAS_REVIEW_REVIEW_BACKEND_H_
#define VERITAS_REVIEW_REVIEW_BACKEND_H_
#include "veritas/review/LocalEvidence.h"
#include <set>
namespace veritas::review {
struct ContextSelection {
  std::string path, kind;
};
struct BackendProject {
  SnapshotRequest snapshot;
  std::map<std::string, ContextSelection> context_selections;
  std::set<std::string> allowed_untracked;
  // Each key selects both a manifest and a target; no model path is accepted.
  std::map<std::string, std::pair<std::string, std::string>> programs;
};
struct BackendConfig {
  std::map<std::string, BackendProject> projects;
  StructuralBudget structural;
  uint64_t max_request_bytes = 1024 * 1024, max_output_bytes = 1024 * 1024,
           max_context_bytes = 32768, max_examined = 10000, max_requests = 256,
           wall_ms = 120000;
};
// Trusted operator configuration, supplied in argv before any model
// interaction.
StatusOr<BackendConfig> DecodeBackendConfig(std::string_view);
class ReviewBackend {
public:
  explicit ReviewBackend(BackendConfig);
  ~ReviewBackend();
  StatusOr<std::string> Call(std::string_view request);
  size_t published_snapshots() const;

private:
  struct State;
  BackendConfig config_;
  std::unique_ptr<State> state_;
  std::set<std::string> request_ids_;
  bool negotiated_ = false;
  uint64_t charged_wall_ms_ = 0;
};
} // namespace veritas::review
#endif
