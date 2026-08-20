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

#ifndef VERITAS_ANALYSIS_PROJECTPUBLICATIONCOORDINATOR_H_
#define VERITAS_ANALYSIS_PROJECTPUBLICATIONCOORDINATOR_H_

#include <memory>
#include <vector>

#include "veritas/build/AnalysisManifest.h"
#include "veritas/core/Status.h"
#include "veritas/cpg/ThinCpg.h"
#include "veritas/summary/v1/summary.pb.h"

namespace veritas::summarydb {
class SummaryRepository;
}  // namespace veritas::summarydb

namespace veritas::cpg {
class CpgRepository;
}  // namespace veritas::cpg

namespace veritas::analysis {

// CompletedProjectAnalysis is the validated summary + CPG pair handed to the
// publication coordinator.
struct CompletedProjectAnalysis {
  std::vector<summary::v1::FunctionSummary> summaries;
  ::veritas::cpg::ThinCpg graph;
};

// ProjectPublicationCoordinator publishes summaries and the CPG projection in
// one SQLite transaction so neither current binding advances without the other.
class ProjectPublicationCoordinator {
 public:
  ~ProjectPublicationCoordinator();

  static StatusOr<std::unique_ptr<ProjectPublicationCoordinator>> Open(
      const std::string& db_path);

  // Persist the M1 program context so summary/CPG bindings resolve their
  // revision/build foreign keys.
  Status PersistManifestContext(const build::AnalysisManifest& manifest);

  // Publish summaries and the CPG projection atomically. Returns the published
  // FunctionSummaryIDs in input order.
  StatusOr<std::vector<core::StableId>> Publish(CompletedProjectAnalysis completed);

 private:
  explicit ProjectPublicationCoordinator(
      std::unique_ptr<summarydb::SummaryRepository> summaries);

  std::unique_ptr<summarydb::SummaryRepository> summaries_;
  std::unique_ptr<::veritas::cpg::CpgRepository> cpg_;
};

}  // namespace veritas::analysis

#endif  // VERITAS_ANALYSIS_PROJECTPUBLICATIONCOORDINATOR_H_
