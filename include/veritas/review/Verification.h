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

#ifndef VERITAS_REVIEW_VERIFICATION_H_
#define VERITAS_REVIEW_VERIFICATION_H_
#include "veritas/review/ReviewTypes.h"
#include <set>
#include <tuple>
namespace veritas::review {
// Trusted host policy; never decoded from a model request. Registering a name
// alone grants no authority: an independently checked immutable receipt is
// required for the exact request/result, including its domain and dependencies.
struct VerificationPolicy {
  std::set<std::tuple<std::string, std::string, std::string>> producers;
  std::map<std::string, std::string> artifacts;
  std::set<std::string> authorized_results, admitted_assumptions;
};
enum class ReviewDisposition {
  Inconclusive,
  SupportedConcern,
  SemanticObservation,
  VerifiedFinding,
  NoDefectObserved
};
StatusOr<std::string> VerificationObligationId(const Json &request);
StatusOr<std::string> VerificationRequestDigest(const Json &request);
StatusOr<std::string> VerificationResultId(const Json &result);
Status ValidateVerificationRequest(const Json &request);
Status ValidateVerificationResult(const Json &request, const Json &result,
                                  const VerificationPolicy &policy);
StatusOr<ReviewDisposition> ApplyVerification(ReviewDisposition state,
                                              const Json &request,
                                              const Json &result,
                                              const VerificationPolicy &policy);
} // namespace veritas::review
#endif
