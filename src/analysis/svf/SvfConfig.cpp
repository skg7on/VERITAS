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

#include "SvfConfig.h"

namespace veritas::analysis::svf {

SvfConfig SvfConfig::Default() {
  return SvfConfig{
      .pointer_analysis = PointerAnalysisKind::kAndersenWaveDiff,
      .soft_analysis_budget = std::chrono::seconds(300),
      .max_graph_nodes = 2'000'000,
      .max_emitted_facts = 5'000'000,
      .max_alias_pairs = 5'000'000,
      .field_sensitive = true,
  };
}

std::string SvfConfig::CanonicalAnalyzerConfig() const {
  return "pointer_analysis=andersen_wave_diff\n"
         "soft_analysis_budget_seconds=" +
         std::to_string(soft_analysis_budget.count()) + "\n" +
         "max_graph_nodes=" + std::to_string(max_graph_nodes) + "\n" +
         "max_emitted_facts=" + std::to_string(max_emitted_facts) + "\n" +
         "max_alias_pairs=" + std::to_string(max_alias_pairs) + "\n" +
         "field_sensitive=" + (field_sensitive ? "true\n" : "false\n");
}

}  // namespace veritas::analysis::svf
