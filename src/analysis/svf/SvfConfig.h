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

#ifndef VERITAS_ANALYSIS_SVF_SVFCONFIG_H_
#define VERITAS_ANALYSIS_SVF_SVFCONFIG_H_

#include <chrono>
#include <cstddef>
#include <string>

namespace veritas::analysis::svf {

enum class PointerAnalysisKind {
  kAndersenWaveDiff
};

struct SvfConfig {
  PointerAnalysisKind pointer_analysis;
  std::chrono::seconds soft_analysis_budget;
  std::size_t max_graph_nodes;
  std::size_t max_emitted_facts;
  // Upper bound on the number of pointer pairs the alias cross-product will
  // examine. The cross-product is O(N^2) in the number of admitted alias
  // pointers; this cap bounds that computation independently of the fact
  // emission budget so a large program cannot stall the mapping pass.
  std::size_t max_alias_pairs;
  bool field_sensitive;

  static SvfConfig Default();
  std::string CanonicalAnalyzerConfig() const;
};

}  // namespace veritas::analysis::svf

#endif  // VERITAS_ANALYSIS_SVF_SVFCONFIG_H_
