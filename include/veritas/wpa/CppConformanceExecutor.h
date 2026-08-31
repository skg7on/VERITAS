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

// CppConformanceExecutor.h — the C++ conformance/emergency engine adapter.
//
// Delegates to CppRuleEvaluator over the same immutable logical input a Souffle
// run consumes. It exposes only kCppConformance or kCppEmergency and can never
// stand in for the production Souffle engine: Create rejects kSouffle, so a
// C++ executor is never constructible under a production identity.

#ifndef VERITAS_WPA_CPP_CONFORMANCE_EXECUTOR_H_
#define VERITAS_WPA_CPP_CONFORMANCE_EXECUTOR_H_

#include <string>
#include <string_view>

#include "veritas/wpa/WpaExecutor.h"

namespace veritas::wpa {

class CppConformanceExecutor final : public WpaExecutor {
 public:
  // Constructs an executor with the given non-production identity. Returns
  // InvalidArgument for kSouffle, which the C++ engine must never carry.
  static StatusOr<CppConformanceExecutor> Create(
      facts::EngineIdentity identity, std::string toolchain_identity);

  facts::EngineIdentity identity() const override;
  std::string_view toolchain_identity() const override;
  StatusOr<facts::RawWpaEvaluation> Execute(
      const WpaExecutionEnvelope& input,
      const WpaExecutionLimits& limits) const override;

 private:
  CppConformanceExecutor(facts::EngineIdentity identity,
                         std::string toolchain_identity);

  facts::EngineIdentity identity_;
  std::string toolchain_identity_;
};

}  // namespace veritas::wpa

#endif  // VERITAS_WPA_CPP_CONFORMANCE_EXECUTOR_H_
