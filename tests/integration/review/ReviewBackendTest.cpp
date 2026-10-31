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

#include "veritas/review/ReviewBackend.h"
#include <gtest/gtest.h>
namespace veritas::review {
Json Request(std::string op, Json parameters, std::string id = "request-1") {
  return Json::Object{{"protocol", kProtocol},
                      {"request_id", id},
                      {"operation", op},
                      {"parameters", parameters}};
}
TEST(ReviewBackendTest, HandshakeDoesNotRequireSummaryDb) {
  ReviewBackend backend(BackendConfig{});
  auto response = backend.Call(*CanonicalJson(Request(
      "handshake", Json::Object{{"accepted_protocols",
                                 Json::Array{std::string(kProtocol)}}})));
  ASSERT_TRUE(response.ok()) << response.status().message();
  auto decoded = DecodeV2(*response, "request-1", "handshake");
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(decoded->ok, true);
  EXPECT_FALSE(decoded->binding.has_value());
}
TEST(ReviewBackendTest, NoCapabilityDoesNotSelectPractice) {
  ReviewBackend backend(BackendConfig{});
  auto response = backend.Call(*CanonicalJson(Request(
      "handshake", Json::Object{{"accepted_protocols",
                                 Json::Array{"veritas.review.backend.v1"}}})));
  ASSERT_TRUE(response.ok());
  auto decoded = DecodeV2(*response, "request-1");
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(decoded->ok, false);
  EXPECT_EQ(backend.published_snapshots(), 0U);
}
TEST(ReviewBackendTest,
     RejectsWrongOperationAndDuplicateRequestBeforePublication) {
  ReviewBackend backend(BackendConfig{});
  auto request = *CanonicalJson(Request(
      "handshake", Json::Object{{"accepted_protocols",
                                 Json::Array{std::string(kProtocol)}}}));
  ASSERT_TRUE(backend.Call(request).ok());
  auto duplicate = backend.Call(request);
  ASSERT_TRUE(duplicate.ok());
  EXPECT_FALSE(*DecodeV2(*duplicate)->ok);
  auto wrong = backend.Call(*CanonicalJson(Request("shell", Json::Object{})));
  EXPECT_FALSE(wrong.ok());
  EXPECT_EQ(backend.published_snapshots(), 0U);
}
TEST(ReviewBackendTest, VerifyIsAdvertisedOnlyWithAnIsolationProfile) {
  BackendConfig config;
  ReplayConfig replay;
  replay.compiler = "/usr/bin/clang++";
  replay.isolation = "macos-sandbox";
  config.replay = replay;
  auto response = [&](const BackendConfig &candidate) {
    ReviewBackend backend(candidate);
    return backend.Call(*CanonicalJson(Request(
        "handshake", Json::Object{{"accepted_protocols",
                                   Json::Array{std::string(kProtocol)}}})));
  };
  auto enabled = response(config);
  ASSERT_TRUE(enabled.ok());
  auto decoded = DecodeV2(*enabled, "request-1", "handshake");
  ASSERT_TRUE(decoded.ok());
  bool advertised = false;
  for (const auto &capability :
       *decoded->document.getAsObject()->getObject("result")->getArray(
           "capabilities")) {
    const auto &object = *capability.getAsObject();
    if (*object.getString("operation") == "verify") {
      advertised = true;
      EXPECT_EQ(*object.getArray("predicate_kinds")->front().getAsString(),
                "copy_bound.v1");
      EXPECT_EQ(*object.getString("authority"), "CONCRETE_REPLAY");
    }
  }
  EXPECT_TRUE(advertised);
  config.replay->isolation = "";
  auto disabled = response(config);
  ASSERT_TRUE(disabled.ok());
  auto unadvertised = DecodeV2(*disabled, "request-1", "handshake");
  ASSERT_TRUE(unadvertised.ok());
  for (const auto &capability :
       *unadvertised->document.getAsObject()->getObject("result")->getArray(
           "capabilities"))
    EXPECT_NE(*capability.getAsObject()->getString("operation"), "verify");
}

TEST(ReviewBackendTest, DecodeConfigAcceptsQualifiedVerification) {
  const std::string json =
      R"({"project_key":"p","project_root":"/tmp/project",
          "repository_id":"repo:sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
          "base_ref":"HEAD","compile_commands_path":"compile_commands.json",
          "selected_tracked":[],"selected_artifacts":[],"allowed_untracked":[],
          "context_selections":{},"programs":{},
          "limits":{"wall_ms":1000,"output_bytes":4096,"examined":100,
                    "context_bytes":1024,"request_bytes":4096,"requests":10},
          "verification":{"compiler":"/usr/bin/clang++",
                          "isolation":"macos-sandbox"}})";
  auto decoded = DecodeBackendConfig(json);
  ASSERT_TRUE(decoded.ok()) << decoded.status().message();
  ASSERT_TRUE(decoded->replay.has_value());
  EXPECT_EQ(decoded->replay->compiler, "/usr/bin/clang++");
  EXPECT_EQ(decoded->replay->isolation, "macos-sandbox");
  EXPECT_FALSE(decoded->replay->deny_read_paths.empty());
  EXPECT_EQ(decoded->replay->deny_read_paths.front(), "/tmp/project");
  auto unknown = DecodeBackendConfig(
      R"({"project_key":"p","project_root":"/tmp/project",
          "repository_id":"r","base_ref":"HEAD",
          "compile_commands_path":"c.json","selected_tracked":[],
          "selected_artifacts":[],"allowed_untracked":[],
          "context_selections":{},"programs":{},
          "limits":{"wall_ms":1000,"output_bytes":4096,"examined":100,
                    "context_bytes":1024,"request_bytes":4096,"requests":10},
          "invented":true})");
  EXPECT_FALSE(unknown.ok());
  auto bad_replay = DecodeBackendConfig(
      R"({"project_key":"p","project_root":"/tmp/project",
          "repository_id":"r","base_ref":"HEAD",
          "compile_commands_path":"c.json","selected_tracked":[],
          "selected_artifacts":[],"allowed_untracked":[],
          "context_selections":{},"programs":{},
          "limits":{"wall_ms":1000,"output_bytes":4096,"examined":100,
                    "context_bytes":1024,"request_bytes":4096,"requests":10},
          "verification":{"compiler":"relative-clang","isolation":""}})");
  EXPECT_FALSE(bad_replay.ok());
}

TEST(ReviewBackendTest, RevisionTargetCannotSilentlySelectWorkingTree) {
  BackendConfig config;
  config.projects.emplace("fixture", BackendProject{});
  ReviewBackend backend(config);
  ASSERT_TRUE(backend
                  .Call(*CanonicalJson(Request(
                      "handshake",
                      Json::Object{{"accepted_protocols",
                                    Json::Array{std::string(kProtocol)}}})))
                  .ok());
  auto result = backend.Call(*CanonicalJson(
      Request("capture",
              Json::Object{{"project_key", "fixture"},
                           {"base_ref", "HEAD"},
                           {"target_ref", "HEAD"},
                           {"selected_untracked", Json::Array{}},
                           {"program_manifest_key", nullptr},
                           {"context_selection_keys", Json::Array{}}},
              "capture-1")));
  ASSERT_TRUE(result.ok());
  auto decoded = DecodeV2(*result);
  ASSERT_TRUE(decoded.ok());
  EXPECT_FALSE(*decoded->ok);
  EXPECT_EQ(
      *decoded->document.getAsObject()->getObject("error")->getString("code"),
      "UNSUPPORTED");
  EXPECT_EQ(backend.published_snapshots(), 0U);
}
} // namespace veritas::review
