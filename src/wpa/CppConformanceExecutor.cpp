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

#include "veritas/wpa/CppConformanceExecutor.h"

#include "veritas/wpa/CppRuleEvaluator.h"

namespace veritas::wpa {

StatusOr<CppConformanceExecutor> CppConformanceExecutor::Create(
    facts::EngineIdentity identity, std::string toolchain_identity) {
  if (identity == facts::EngineIdentity::kSouffle) {
    return Status::InvalidArgument(
        "CppConformanceExecutor cannot carry the production Souffle identity");
  }
  if (identity != facts::EngineIdentity::kCppConformance &&
      identity != facts::EngineIdentity::kCppEmergency) {
    return Status::InvalidArgument(
        "CppConformanceExecutor requires a recognized non-production identity");
  }
  return CppConformanceExecutor(identity, std::move(toolchain_identity));
}

CppConformanceExecutor::CppConformanceExecutor(
    facts::EngineIdentity identity, std::string toolchain_identity)
    : identity_(identity), toolchain_identity_(std::move(toolchain_identity)) {}

facts::EngineIdentity CppConformanceExecutor::identity() const {
  return identity_;
}

std::string_view CppConformanceExecutor::toolchain_identity() const {
  return toolchain_identity_;
}

StatusOr<facts::RawWpaEvaluation> CppConformanceExecutor::Execute(
    const WpaExecutionEnvelope& input, const WpaExecutionLimits& limits) const {
  if (input.run.engine != identity_) {
    return Status::InvalidArgument(
        "envelope engine identity does not match this executor");
  }
  if (input.run.engine_toolchain_identity != toolchain_identity_) {
    return Status::InvalidArgument(
        "envelope toolchain identity does not match this executor");
  }
  (void)limits;  // The in-process C++ evaluator has no subprocess limits.
  CppRuleEvaluator evaluator;
  return evaluator.Evaluate(input.logical);
}

}  // namespace veritas::wpa
