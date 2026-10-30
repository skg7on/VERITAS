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

#include "veritas/review/Verification.h"
#include <fstream>
#include <gtest/gtest.h>
namespace veritas::review {
namespace {
Json::Object &Mutable(Json &v) { return std::get<Json::Object>(v.data); }
struct Fixture {
  Json request, result;
  VerificationPolicy policy;
  Fixture() {
    std::ifstream in(std::string(VERITAS_SOURCE_DIR) +
                     "/tests/fixtures/review/verification/authority.json");
    auto data =
        ParseStrictJson(std::string(std::istreambuf_iterator<char>(in), {}));
    if (!data.ok())
      std::abort();
    request = *data->getAsObject()->get("request");
    result = *data->getAsObject()->get("result");
    policy.producers.emplace("test-bound-check", "1", std::string(64, 'a'));
    auto inputs = request.getAsObject()->getArray("input_artifact_ids");
    policy.artifacts[*inputs->front().getAsString()] = "evidence";
    policy.artifacts["witness"] = "witness";
    Resign();
  }
  void Resign() {
    Mutable(result)["verification_id"] = *VerificationResultId(result);
    policy.authorized_results.clear();
    policy.authorized_results.insert(
        *result.getAsObject()->getString("verification_id"));
  }
};
} // namespace
TEST(VerificationTest, ExactRegisteredCertificateAloneMayDecide) {
  Fixture f;
  EXPECT_EQ(*VerificationObligationId(f.request),
            *f.request.getAsObject()->getString("obligation_id"));
  EXPECT_EQ(*VerificationRequestDigest(f.request),
            *f.result.getAsObject()->getString("request_digest"));
  EXPECT_TRUE(ValidateVerificationResult(f.request, f.result, f.policy).ok());
  auto outcome = ApplyVerification(ReviewDisposition::SupportedConcern,
                                   f.request, f.result, f.policy);
  ASSERT_TRUE(outcome.ok());
  EXPECT_EQ(*outcome, ReviewDisposition::VerifiedFinding);
  EXPECT_EQ(*ApplyVerification(ReviewDisposition::SupportedConcern, f.request,
                               f.result, f.policy),
            *outcome);
}
TEST(VerificationTest,
     WrongBindingPredicateOperandProducerAndInputAreRejected) {
  for (auto key : {"binding", "predicate_kind", "typed_operands", "producer",
                   "input_dependencies"}) {
    Fixture f;
    auto &o = Mutable(f.result);
    auto value = *o.get(key);
    if (auto map = std::get_if<Json::Object>(&value.data)) {
      if (std::string_view(key) == "binding")
        (*map)["working_tree_digest"] = std::string(64, 'c');
      else if (std::string_view(key) == "typed_operands")
        (*map)["capacity_bytes"] = 64;
      else
        (*map)["name"] = "model";
    } else if (auto list = std::get_if<Json::Array>(&value.data)) {
      auto dep = *list->front().getAsObject();
      dep["content_digest"] = std::string(64, 'c');
      list->front() = dep;
    } else
      value = "invented";
    o[key] = value;
    f.Resign();
    EXPECT_FALSE(ValidateVerificationResult(f.request, f.result, f.policy).ok())
        << key;
  }
}
TEST(VerificationTest, CopiedAndCircularProofCannotPromote) {
  Fixture f;
  f.policy.authorized_results.clear();
  EXPECT_FALSE(ValidateVerificationResult(f.request, f.result, f.policy).ok());
  f.Resign();
  auto &o = Mutable(f.result);
  o["counterexample_artifact_ids"] =
      *f.request.getAsObject()->get("input_artifact_ids");
  f.Resign();
  EXPECT_FALSE(ValidateVerificationResult(f.request, f.result, f.policy).ok());
}
TEST(VerificationTest, TimeoutUnsupportedAndIncompleteCannotPromote) {
  for (auto pair :
       {std::pair{"UNKNOWN", "TIMEOUT"}, std::pair{"INCOMPLETE", "UNSUPPORTED"},
        std::pair{"ERROR", "ERROR"}}) {
    Fixture f;
    auto &o = Mutable(f.result);
    o["status"] = pair.first;
    o["operational_outcome"] = pair.second;
    f.Resign();
    auto applied = ApplyVerification(ReviewDisposition::SupportedConcern,
                                     f.request, f.result, f.policy);
    ASSERT_TRUE(applied.ok());
    EXPECT_EQ(*applied, ReviewDisposition::SupportedConcern);
  }
  Fixture f;
  Mutable(f.result)["operational_outcome"] = "TIMEOUT";
  f.Resign();
  EXPECT_FALSE(ValidateVerificationResult(f.request, f.result, f.policy).ok());
}
TEST(VerificationTest, UndefinedDomainAndUniversalSafetyAreRejected) {
  Fixture f;
  auto &q = Mutable(f.request);
  q["execution_domain"] =
      Json::Object{{"kind", "FORMAL"}, {"domain", Json::Array{}}};
  EXPECT_FALSE(ValidateVerificationResult(f.request, f.result, f.policy).ok());
}
TEST(VerificationTest, ConcreteProofNeverProducesUniversalSafety) {
  Fixture f;
  auto &q = Mutable(f.request);
  auto operands = *q.getObject("typed_operands");
  operands["requested_bytes"] = 32;
  q["typed_operands"] = operands;
  q["obligation_id"] = *VerificationObligationId(f.request);
  auto &o = Mutable(f.result);
  o["typed_operands"] = operands;
  o["request_digest"] = *VerificationRequestDigest(f.request);
  o["status"] = "PROVED";
  o["certificate_artifact_ids"] = o["counterexample_artifact_ids"];
  o["counterexample_artifact_ids"] = Json::Array{};
  f.Resign();
  auto outcome = ApplyVerification(ReviewDisposition::SupportedConcern,
                                   f.request, f.result, f.policy);
  ASSERT_TRUE(outcome.ok());
  EXPECT_EQ(*outcome, ReviewDisposition::NoDefectObserved);
}
TEST(VerificationTest, ObligationIdentityDoesNotHashItself) {
  Fixture f;
  auto id = VerificationObligationId(f.request);
  ASSERT_TRUE(id.ok());
  Mutable(f.request)["obligation_id"] = "different";
  EXPECT_EQ(*VerificationObligationId(f.request), *id);
}
} // namespace veritas::review
