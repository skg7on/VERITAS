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

#ifndef VERITAS_ANALYSIS_PROJECTANALYZERINTERNAL_H_
#define VERITAS_ANALYSIS_PROJECTANALYZERINTERNAL_H_

#include <memory>

#include "veritas/analysis/ProjectAnalyzer.h"

namespace veritas::analysis::svf {
class SvfAnalysisStage;
}  // namespace veritas::analysis::svf

namespace veritas::analysis::internal {

// ProjectAnalyzerTestFactory is the private test seam for injecting a recording
// or failing SVF stage. It is not installed as a public header.
class ProjectAnalyzerTestFactory {
 public:
  static ProjectAnalyzer Create(std::unique_ptr<svf::SvfAnalysisStage> stage);
};

}  // namespace veritas::analysis::internal

#endif  // VERITAS_ANALYSIS_PROJECTANALYZERINTERNAL_H_
