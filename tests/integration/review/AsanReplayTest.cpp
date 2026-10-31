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

#include "veritas/review/AsanReplay.h"
#include "veritas/review/LocalEvidence.h"
#include "veritas/review/ReviewBackend.h"
#include "veritas/review/Verification.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace veritas::review {
namespace {

std::string ReadFile(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}
std::string Fixture(std::string_view name) {
  return ReadFile(std::filesystem::path(VERITAS_SOURCE_DIR) /
                  "tests/fixtures/review/asan_replay" / name);
}
void Write(const std::filesystem::path &path, std::string_view text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << text;
}
int Git(const std::filesystem::path &root, std::vector<std::string> args) {
  args.insert(args.begin(), {"/usr/bin/git", "-C", root.string()});
  std::vector<char *> argv;
  for (auto &arg : args)
    argv.push_back(arg.data());
  argv.push_back(nullptr);
  pid_t pid = fork();
  if (pid == 0) {
    execv(argv[0], argv.data());
    _exit(127);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
// Bounded probe used only to select a host compiler whose ASan runtime works.
bool RunForSetup(const std::vector<std::string> &args, int timeout_ms) {
  pid_t pid = fork();
  if (pid == 0) {
    setpgid(0, 0);
    int null = open("/dev/null", O_WRONLY);
    if (null >= 0) {
      dup2(null, STDOUT_FILENO);
      dup2(null, STDERR_FILENO);
      close(null);
    }
    std::vector<char *> argv;
    for (const auto &arg : args)
      argv.push_back(const_cast<char *>(arg.c_str()));
    argv.push_back(nullptr);
    execv(argv[0], argv.data());
    _exit(127);
  }
  setpgid(pid, pid);
  for (int elapsed = 0; elapsed < timeout_ms; elapsed += 10) {
    int status = 0;
    if (waitpid(pid, &status, WNOHANG) == pid)
      return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    usleep(10000);
  }
  kill(-pid, SIGKILL);
  int status = 0;
  waitpid(pid, &status, 0);
  return false;
}
std::string Capture(const char *command) {
  std::string out;
  std::FILE *pipe = ::popen(command, "r");
  if (!pipe)
    return out;
  char buffer[4096];
  while (std::fgets(buffer, sizeof buffer, pipe))
    out += buffer;
  ::pclose(pipe);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
    out.pop_back();
  return out;
}
std::string FindAsanCompiler() {
  if (const char *env = std::getenv("VERITAS_ASAN_COMPILER"))
    if (::access(env, X_OK) == 0)
      return env;
  std::vector<std::string> candidates;
#ifdef __APPLE__
  // The /usr/bin shim needs the xcrun cache; the real CLT driver does not when
  // an explicit sysroot is supplied.
  candidates = {"/Library/Developer/CommandLineTools/usr/bin/clang++",
                "/usr/bin/clang++"};
#else
  candidates = {"/usr/bin/clang++", "/usr/lib/llvm-24/bin/clang++",
                "/usr/lib/llvm-18/bin/clang++", "/usr/bin/clang++-18",
                "/usr/bin/clang++-19"};
#endif
  std::string dir = "/private/tmp/veritas-asan-probe-XXXXXX";
#ifndef __APPLE__
  dir = "/tmp/veritas-asan-probe-XXXXXX";
#endif
  char *made = mkdtemp(dir.data());
  if (!made)
    return {};
  std::filesystem::path probe = made;
  Write(probe / "ok.cc", "int main() { return 0; }\n");
  std::string sysroot;
#ifdef __APPLE__
  sysroot = Capture("/usr/bin/xcrun --show-sdk-path");
#endif
  for (const auto &compiler : candidates) {
    if (::access(compiler.c_str(), X_OK) != 0)
      continue;
    std::vector<std::string> args = {compiler};
    if (!sysroot.empty())
      args.insert(args.end(), {"-isysroot", sysroot});
    args.insert(args.end(),
                {"-fsanitize=address", "-g", "-o", (probe / "ok").string(),
                 (probe / "ok.cc").string()});
    if (!RunForSetup(args, 20000))
      continue;
    if (RunForSetup({(probe / "ok").string()}, 10000))
      return compiler;
  }
  return {};
}

class TempDir {
public:
  TempDir() {
    std::string pattern = "/private/tmp/veritas-replay-XXXXXX";
#ifndef __APPLE__
    pattern = "/tmp/veritas-replay-XXXXXX";
#endif
    char *result = mkdtemp(pattern.data());
    if (!result)
      std::abort();
    path_ = result;
  }
  ~TempDir() {
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(path_, ec);
         !ec && it != std::filesystem::recursive_directory_iterator();
         it.increment(ec))
      if (it->is_directory())
        std::filesystem::permissions(it->path(),
                                     std::filesystem::perms::owner_all, ec);
    std::filesystem::remove_all(path_, ec);
  }
  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

Json Ref(const EntityRef &r) {
  return Json::Object{{"index_id", r.index_id.value()}, {"ordinal", r.ordinal}};
}

class AsanReplayTest : public ::testing::Test {
protected:
  TempDir temp_;
  std::string compiler_;
  std::string isolation_;
  uint32_t projects_ = 0;
  void SetUp() override {
    compiler_ = FindAsanCompiler();
    ASSERT_FALSE(compiler_.empty())
        << "no compiler with a working AddressSanitizer runtime";
#ifdef __APPLE__
    isolation_ = "macos-sandbox";
#else
    isolation_ = "linux-bwrap";
#endif
  }
  struct Project {
    std::filesystem::path root;
    ReviewSnapshot snapshot;
    StructuralIndex index;
  };
  struct Payload {
    std::string bytes, path;
  };
  Payload WritePayload(size_t size) {
    Payload p;
    p.bytes.resize(size);
    for (size_t i = 0; i < size; ++i)
      p.bytes[i] = static_cast<char>(i % 251);
    p.path =
        (temp_.path() / ("payload-" + std::to_string(++projects_) + ".bin"))
            .string();
    Write(p.path, p.bytes);
    return p;
  }
  std::optional<Project> Capture(std::string_view source,
                                 std::vector<std::string> flags = {}) {
    auto root = temp_.path() / ("project-" + std::to_string(++projects_));
    std::filesystem::create_directories(root);
    if (Git(root, {"init", "-q"}) != 0) {
      ADD_FAILURE() << "git init failed";
      return std::nullopt;
    }
    Write(root / "packet.h", Fixture("packet.h"));
    Write(root / "main.cc", source);
    Json::Array arguments{"clang++", "-std=c++20"};
    for (auto &flag : flags)
      arguments.emplace_back(flag);
    arguments.emplace_back("-c");
    arguments.emplace_back("main.cc");
    Write(root / "compile_commands.json",
          *CanonicalJson(Json::Array{Json::Object{{"directory", root.string()},
                                                  {"file", "main.cc"},
                                                  {"arguments", arguments}}}));
    if (Git(root, {"add", "."}) != 0 ||
        Git(root, {"-c", "user.name=Replay Test", "-c",
                   "user.email=replay@example.invalid", "commit", "-qm",
                   "base"}) != 0) {
      ADD_FAILURE() << "git commit failed";
      return std::nullopt;
    }
    SnapshotRequest request;
    request.project_root = root;
    request.repository_id = "repo:sha256:" + std::string(64, 'a');
    request.compile_commands_path = "compile_commands.json";
    auto snapshot = CaptureReviewSnapshot(request);
    if (!snapshot.ok()) {
      ADD_FAILURE() << snapshot.status().message();
      return std::nullopt;
    }
    auto index = BuildStructuralIndex(*snapshot, {});
    if (!index.ok()) {
      ADD_FAILURE() << index.status().message();
      return std::nullopt;
    }
    return Project{root, *snapshot, *index};
  }
  std::optional<Project> CaptureChanged(std::string_view base,
                                        std::string_view working) {
    auto root = temp_.path() / ("project-" + std::to_string(++projects_));
    std::filesystem::create_directories(root);
    if (Git(root, {"init", "-q"}) != 0) {
      ADD_FAILURE() << "git init failed";
      return std::nullopt;
    }
    Write(root / "packet.h", Fixture("packet.h"));
    Write(root / "main.cc", base);
    Write(root / "compile_commands.json",
          *CanonicalJson(Json::Array{
              Json::Object{{"directory", root.string()},
                           {"file", "main.cc"},
                           {"arguments", Json::Array{"clang++", "-std=c++20",
                                                     "-c", "main.cc"}}}}));
    if (Git(root, {"add", "."}) != 0 ||
        Git(root, {"-c", "user.name=Replay Test", "-c",
                   "user.email=replay@example.invalid", "commit", "-qm",
                   "base"}) != 0) {
      ADD_FAILURE() << "git commit failed";
      return std::nullopt;
    }
    // Leave the bound removal as an uncommitted change so seed candidates
    // observe a real base diff.
    Write(root / "main.cc", working);
    SnapshotRequest request;
    request.project_root = root;
    request.repository_id = "repo:sha256:" + std::string(64, 'a');
    request.compile_commands_path = "compile_commands.json";
    auto snapshot = CaptureReviewSnapshot(request);
    if (!snapshot.ok()) {
      ADD_FAILURE() << snapshot.status().message();
      return std::nullopt;
    }
    auto index = BuildStructuralIndex(*snapshot, {});
    if (!index.ok()) {
      ADD_FAILURE() << index.status().message();
      return std::nullopt;
    }
    return Project{root, *snapshot, *index};
  }
  std::optional<LocalEvidence> Evidence(const Project &project, EntityRef call,
                                        uint64_t capacity) {
    auto evidence = InvestigateLocal({project.index,
                                      project.snapshot.binding(),
                                      "copy_bound.v1",
                                      call,
                                      {"EXTENT", "OPERANDS"},
                                      100});
    if (!evidence.ok())
      return std::nullopt;
    for (const auto &observation : evidence->observations)
      if (observation.slot == "EXTENT" &&
          std::get<ByteExtent>(observation.value).bytes == capacity)
        return *evidence;
    return std::nullopt;
  }
  std::optional<LocalEvidence> FindEvidence(const Project &project,
                                            uint64_t capacity) {
    for (const auto &entity : project.index.entities()) {
      if (entity.kind != "CALLSITE")
        continue;
      auto evidence =
          Evidence(project, {project.index.id(), entity.ordinal}, capacity);
      if (evidence)
        return evidence;
    }
    return std::nullopt;
  }
  Json Request(const Project &project, const LocalEvidence &evidence,
               uint64_t capacity, uint64_t requested, uint64_t readable,
               const Payload &payload, std::string_view harness,
               uint64_t wall_ms = 15000) {
    const CopyOperands *operands = nullptr;
    for (const auto &observation : evidence.observations)
      if (observation.slot == "OPERANDS")
        operands = &std::get<CopyOperands>(observation.value);
    if (!operands) {
      ADD_FAILURE() << "evidence has no admitted copy operands";
      return Json::Object{};
    }
    const std::string digest = HashBytes(payload.bytes);
    Json request = Json::Object{
        {"binding", ToJson(project.snapshot.binding())},
        {"predicate_kind", "copy_bound.v1"},
        {"typed_operands",
         Json::Object{{"kind", "copy_bound.v1"},
                      {"callsite_ref", Ref(evidence.callsite_ref)},
                      {"destination_ref", Ref(operands->destination)},
                      {"source_ref", Ref(operands->source)},
                      {"length_ref", Ref(operands->length)},
                      {"capacity_bytes", capacity},
                      {"requested_bytes", requested},
                      {"readable_bytes", readable},
                      {"payload_digest", digest},
                      {"harness_id", std::string(harness)}}},
        {"input_artifact_ids", Json::Array{evidence.local_evidence_id.value()}},
        {"assumptions", Json::Array{}},
        {"execution_domain",
         Json::Object{
             {"kind", "CONCRETE_INPUT"},
             {"snapshot_id", project.snapshot.binding().snapshot_id->value()},
             {"input_digest", digest},
             {"harness_id", std::string(harness)}}},
        {"backend_id", "asan-replay.v1"},
        {"backend_version", "1"},
        {"budget", Json::Object{{"wall_ms", wall_ms},
                                {"output_bytes", 65536},
                                {"examined", 100}}}};
    auto obligation = VerificationObligationId(request);
    EXPECT_TRUE(obligation.ok());
    std::get<Json::Object>(request.data)["obligation_id"] = *obligation;
    return request;
  }
  ReplayConfig Config(const Project &project, std::string harness,
                      std::string payload, std::string isolation,
                      uint64_t replay_wall_ms = 15000) {
    ReplayConfig config;
    config.compiler = compiler_;
    config.isolation = std::move(isolation);
    config.work_parent = temp_.path().string();
    config.deny_read_paths = {project.root.string()};
    config.harnesses["copy-bound"] =
        ReplayHarness{std::move(harness), std::move(payload)};
    config.limits.wall_ms = 60000;
    config.limits.output_bytes = 1024 * 1024;
    config.limits.rss_bytes = 1024ull * 1024 * 1024;
    (void)replay_wall_ms;
    return config;
  }
  std::string HarnessPath() {
    return (std::filesystem::path(VERITAS_SOURCE_DIR) /
            "tests/fixtures/review/asan_replay/harness.cc")
        .string();
  }
  VerificationPolicy Policy(const Json &request, const ReplayResult &result) {
    VerificationPolicy policy;
    const auto &producer =
        *result.verification.getAsObject()->getObject("producer");
    policy.producers.insert({*producer.getString("name"),
                             *producer.getString("version"),
                             *producer.getString("config_digest")});
    for (const auto &[id, bytes] : result.artifacts)
      policy.artifacts[id] = bytes;
    policy.authorized_results.insert(
        *result.verification.getAsObject()->getString("verification_id"));
    (void)request;
    return policy;
  }
  void ExpectStatus(const ReplayResult &result, std::string_view status,
                    std::string_view outcome) {
    const auto &v = *result.verification.getAsObject();
    EXPECT_EQ(*v.getString("status"), status);
    EXPECT_EQ(*v.getString("operational_outcome"), outcome);
    EXPECT_TRUE(
        ValidateWireType(result.verification, "VerificationResult").ok());
  }
  bool HasReason(const ReplayResult &result, std::string_view needle) {
    const auto &reasons = *result.verification.getAsObject()
                               ->getObject("coverage")
                               ->getArray("reasons");
    for (const auto &reason : reasons)
      if (reason.getAsString()->find(needle) != std::string::npos)
        return true;
    return false;
  }
  void ExpectPinnedDependencies(const ReplayResult &result) {
    const auto &dependencies =
        *result.verification.getAsObject()->getArray("input_dependencies");
    for (const auto &dependency : dependencies) {
      const auto &object = *dependency.getAsObject();
      const std::string id = *object.getString("artifact_id");
      ASSERT_NE(result.artifacts.find(id), result.artifacts.end()) << id;
      EXPECT_EQ(HashBytes(result.artifacts.at(id)),
                *object.getString("content_digest"))
          << id;
    }
  }
};

TEST_F(AsanReplayTest, ModifiedCopyOverflowsAndBaseRejects33) {
  Payload payload = WritePayload(33);
  auto modified = Capture(Fixture("modified.cc"));
  ASSERT_TRUE(modified);
  auto evidence = FindEvidence(*modified, 32);
  ASSERT_TRUE(evidence);
  Json request =
      Request(*modified, *evidence, 32, 33, 33, payload, "copy-bound");
  auto result = RunAsanReplay(
      request, Config(*modified, HarnessPath(), payload.path, isolation_),
      modified->snapshot, modified->index);
  ASSERT_TRUE(result.ok()) << result.status().message();
  ExpectStatus(*result, "REFUTED", "COMPLETED");
  EXPECT_TRUE(result->compiled);
  EXPECT_NE(result->exit_code, 0);
  EXPECT_FALSE(result->timed_out);
  ExpectPinnedDependencies(*result);
  const auto &v = *result->verification.getAsObject();
  ASSERT_FALSE(v.getArray("counterexample_artifact_ids")->empty());
  EXPECT_TRUE(v.getArray("certificate_artifact_ids")->empty());
  // The exact result is admissible only with its own witness and receipt.
  auto policy = Policy(request, *result);
  EXPECT_TRUE(
      ValidateVerificationResult(request, result->verification, policy).ok());
  auto applied = ApplyVerification(ReviewDisposition::SupportedConcern, request,
                                   result->verification, policy);
  ASSERT_TRUE(applied.ok());
  EXPECT_EQ(*applied, ReviewDisposition::VerifiedFinding);
  VerificationPolicy unauthorized;
  unauthorized.producers = policy.producers;
  unauthorized.artifacts = policy.artifacts;
  EXPECT_FALSE(
      ValidateVerificationResult(request, result->verification, unauthorized)
          .ok());
  // The base rejects the same input; that is a control, not a proof.
  auto base = Capture(Fixture("base.cc"));
  ASSERT_TRUE(base);
  auto base_evidence = FindEvidence(*base, 32);
  ASSERT_TRUE(base_evidence);
  Json base_request =
      Request(*base, *base_evidence, 32, 33, 33, payload, "copy-bound");
  auto base_result = RunAsanReplay(
      base_request, Config(*base, HarnessPath(), payload.path, isolation_),
      base->snapshot, base->index);
  ASSERT_TRUE(base_result.ok()) << base_result.status().message();
  ExpectStatus(*base_result, "UNKNOWN", "COMPLETED");
  EXPECT_EQ(base_result->exit_code, 2);
  EXPECT_TRUE(HasReason(*base_result, "rejected"));
  EXPECT_TRUE(base_result->verification.getAsObject()
                  ->getArray("counterexample_artifact_ids")
                  ->empty());
  auto base_policy = Policy(base_request, *base_result);
  EXPECT_TRUE(ValidateVerificationResult(base_request,
                                         base_result->verification, base_policy)
                  .ok());
  auto base_applied =
      ApplyVerification(ReviewDisposition::SupportedConcern, base_request,
                        base_result->verification, base_policy);
  ASSERT_TRUE(base_applied.ok());
  EXPECT_EQ(*base_applied, ReviewDisposition::SupportedConcern);
}

TEST_F(AsanReplayTest, IdenticalReplayHasStableContentAddress) {
  Payload payload = WritePayload(33);
  auto modified = Capture(Fixture("modified.cc"));
  ASSERT_TRUE(modified);
  auto evidence = FindEvidence(*modified, 32);
  ASSERT_TRUE(evidence);
  Json request =
      Request(*modified, *evidence, 32, 33, 33, payload, "copy-bound");
  auto first = RunAsanReplay(
      request, Config(*modified, HarnessPath(), payload.path, isolation_),
      modified->snapshot, modified->index);
  auto second = RunAsanReplay(
      request, Config(*modified, HarnessPath(), payload.path, isolation_),
      modified->snapshot, modified->index);
  ASSERT_TRUE(first.ok()) << first.status().message();
  ASSERT_TRUE(second.ok()) << second.status().message();
  ExpectStatus(*first, "REFUTED", "COMPLETED");
  ExpectStatus(*second, "REFUTED", "COMPLETED");
  EXPECT_EQ(*first->verification.getAsObject()->getString("verification_id"),
            *second->verification.getAsObject()->getString("verification_id"));
  auto digest_of = [](const ReplayResult &result, std::string_view prefix) {
    for (const auto &dependency :
         *result.verification.getAsObject()->getArray("input_dependencies")) {
      const auto &object = *dependency.getAsObject();
      if (std::string(*object.getString("artifact_id")).starts_with(prefix))
        return std::string(*object.getString("content_digest"));
    }
    return std::string();
  };
  EXPECT_FALSE(digest_of(*first, "replay-executable:").empty());
  EXPECT_EQ(digest_of(*first, "replay-executable:"),
            digest_of(*second, "replay-executable:"));
  EXPECT_EQ(digest_of(*first, "replay-compiler:"),
            digest_of(*second, "replay-compiler:"));
}

TEST_F(AsanReplayTest, Length0And32AreNotDeciding) {
  Payload payload = WritePayload(33);
  auto modified = Capture(Fixture("modified.cc"));
  ASSERT_TRUE(modified);
  auto evidence = FindEvidence(*modified, 32);
  ASSERT_TRUE(evidence);
  for (uint64_t length : {0, 32}) {
    SCOPED_TRACE(length);
    Json request =
        Request(*modified, *evidence, 32, length, 33, payload, "copy-bound");
    auto result = RunAsanReplay(
        request, Config(*modified, HarnessPath(), payload.path, isolation_),
        modified->snapshot, modified->index);
    ASSERT_TRUE(result.ok()) << result.status().message();
    ExpectStatus(*result, "UNKNOWN", "COMPLETED");
    EXPECT_EQ(result->exit_code, 0);
    ExpectPinnedDependencies(*result);
    EXPECT_TRUE(result->verification.getAsObject()
                    ->getArray("counterexample_artifact_ids")
                    ->empty());
    auto policy = Policy(request, *result);
    auto applied = ApplyVerification(ReviewDisposition::SupportedConcern,
                                     request, result->verification, policy);
    ASSERT_TRUE(applied.ok());
    EXPECT_EQ(*applied, ReviewDisposition::SupportedConcern);
  }
}

TEST_F(AsanReplayTest, ShortPayloadIsUnsupported) {
  Payload payload = WritePayload(32);
  auto modified = Capture(Fixture("modified.cc"));
  ASSERT_TRUE(modified);
  auto evidence = FindEvidence(*modified, 32);
  ASSERT_TRUE(evidence);
  Json request =
      Request(*modified, *evidence, 32, 33, 33, payload, "copy-bound");
  auto result = RunAsanReplay(
      request, Config(*modified, HarnessPath(), payload.path, isolation_),
      modified->snapshot, modified->index);
  ASSERT_FALSE(result.ok());
  EXPECT_NE(std::string(result.status().message()).find("readable"),
            std::string::npos)
      << result.status().message();
}

TEST_F(AsanReplayTest, RequestBeyondReadableExtentIsRejected) {
  Payload payload = WritePayload(33);
  auto modified = Capture(Fixture("modified.cc"));
  ASSERT_TRUE(modified);
  auto evidence = FindEvidence(*modified, 32);
  ASSERT_TRUE(evidence);
  Json request =
      Request(*modified, *evidence, 32, 33, 32, payload, "copy-bound");
  auto result = RunAsanReplay(
      request, Config(*modified, HarnessPath(), payload.path, isolation_),
      modified->snapshot, modified->index);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), StatusCode::kInvalidArgument);
}

TEST_F(AsanReplayTest, OtherCrashIsNotThisObligation) {
  Payload payload = WritePayload(33);
  auto project = Capture(Fixture("crash.cc"));
  ASSERT_TRUE(project);
  auto evidence = FindEvidence(*project, 32);
  ASSERT_TRUE(evidence);
  Json request =
      Request(*project, *evidence, 32, 33, 33, payload, "copy-bound");
  auto result = RunAsanReplay(
      request, Config(*project, HarnessPath(), payload.path, isolation_),
      project->snapshot, project->index);
  ASSERT_TRUE(result.ok()) << result.status().message();
  ExpectStatus(*result, "ERROR", "COMPLETED");
  EXPECT_TRUE(result->verification.getAsObject()
                  ->getArray("counterexample_artifact_ids")
                  ->empty());
  auto policy = Policy(request, *result);
  EXPECT_TRUE(
      ValidateVerificationResult(request, result->verification, policy).ok());
  auto applied = ApplyVerification(ReviewDisposition::SupportedConcern, request,
                                   result->verification, policy);
  ASSERT_TRUE(applied.ok());
  EXPECT_EQ(*applied, ReviewDisposition::SupportedConcern);
}

TEST_F(AsanReplayTest, WrongSiteCannotVerify) {
  Payload payload = WritePayload(33);
  auto project = Capture(Fixture("wrong_site.cc"));
  ASSERT_TRUE(project);
  auto evidence = FindEvidence(*project, 32);
  ASSERT_TRUE(evidence);
  Json request =
      Request(*project, *evidence, 32, 33, 33, payload, "copy-bound");
  auto result = RunAsanReplay(
      request, Config(*project, HarnessPath(), payload.path, isolation_),
      project->snapshot, project->index);
  ASSERT_TRUE(result.ok()) << result.status().message();
  ExpectStatus(*result, "ERROR", "COMPLETED");
  EXPECT_TRUE(result->verification.getAsObject()
                  ->getArray("counterexample_artifact_ids")
                  ->empty());
  EXPECT_TRUE(HasReason(*result, "site") || HasReason(*result, "match"));
}

TEST_F(AsanReplayTest, MissingRuntimeIsUnsupported) {
  Payload payload = WritePayload(33);
  auto project = Capture(Fixture("modified.cc"));
  ASSERT_TRUE(project);
  auto evidence = FindEvidence(*project, 32);
  ASSERT_TRUE(evidence);
  Json request =
      Request(*project, *evidence, 32, 33, 33, payload, "copy-bound");
  auto config = Config(*project, HarnessPath(), payload.path, isolation_);
  config.compiler = (temp_.path() / "missing-clang++").string();
  auto result =
      RunAsanReplay(request, config, project->snapshot, project->index);
  ASSERT_FALSE(result.ok());
  EXPECT_NE(std::string(result.status().message()).find("compiler"),
            std::string::npos)
      << result.status().message();
  config.compiler = "/bin/true";
  result = RunAsanReplay(request, config, project->snapshot, project->index);
  ASSERT_FALSE(result.ok());
}

TEST_F(AsanReplayTest, NoIsolationProfileIsUnsupported) {
  Payload payload = WritePayload(33);
  auto project = Capture(Fixture("modified.cc"));
  ASSERT_TRUE(project);
  auto evidence = FindEvidence(*project, 32);
  ASSERT_TRUE(evidence);
  Json request =
      Request(*project, *evidence, 32, 33, 33, payload, "copy-bound");
  const auto marker = temp_.path() / "compiler-was-run";
  const auto script = temp_.path() / "marker-compiler";
  Write(script, "#!/bin/sh\ntouch \"" + marker.string() + "\"\n");
  ASSERT_EQ(::chmod(script.c_str(), 0755), 0);
  auto config = Config(*project, HarnessPath(), payload.path, "");
  config.compiler = script.string();
  auto result =
      RunAsanReplay(request, config, project->snapshot, project->index);
  ASSERT_FALSE(result.ok());
  EXPECT_NE(std::string(result.status().message()).find("isolation"),
            std::string::npos)
      << result.status().message();
  EXPECT_FALSE(std::filesystem::exists(marker));
}

TEST_F(AsanReplayTest, GlobalInitializerCannotWriteHostCanary) {
  Payload payload = WritePayload(33);
  const auto canary = temp_.path() / "host-canary";
  std::filesystem::remove(canary);
  auto project = Capture(Fixture("global_canary.cc"),
                         {"-DVERITAS_CANARY_PATH=\"" + canary.string() + "\""});
  ASSERT_TRUE(project);
  auto evidence = FindEvidence(*project, 32);
  ASSERT_TRUE(evidence);
  Json request =
      Request(*project, *evidence, 32, 32, 33, payload, "copy-bound");
  auto result = RunAsanReplay(
      request, Config(*project, HarnessPath(), payload.path, isolation_),
      project->snapshot, project->index);
  ASSERT_TRUE(result.ok()) << result.status().message();
  ExpectStatus(*result, "UNKNOWN", "COMPLETED");
  EXPECT_EQ(result->exit_code, 0);
  EXPECT_FALSE(std::filesystem::exists(canary));
}

TEST_F(AsanReplayTest, NetworkIsDenied) {
  Payload payload = WritePayload(33);
  int listener = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(listener, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(0x7f000001);
  address.sin_port = 0;
  ASSERT_EQ(
      ::bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof address),
      0);
  ASSERT_EQ(::listen(listener, 1), 0);
  socklen_t length = sizeof address;
  ASSERT_EQ(
      ::getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length),
      0);
  const int port = ntohs(address.sin_port);
  const auto harness = temp_.path() / "network-harness.cc";
  Write(
      harness,
      std::string("#include <arpa/inet.h>\n"
                  "#include <cstdio>\n"
                  "#include <cstdlib>\n"
                  "#include <netinet/in.h>\n"
                  "#include <sys/socket.h>\n"
                  "#include <unistd.h>\n"
                  "int decode(const char *, unsigned long);\n"
                  "int main(int argc,char**argv){\n"
                  "  if(argc!=3)return 3;\n"
                  "  unsigned long n=strtoul(argv[2],nullptr,10);\n"
                  "  FILE*f=fopen(argv[1],\"rb\"); if(!f)return 3;\n"
                  "  unsigned char s[64]; size_t got=fread(s,1,sizeof s,f);\n"
                  "  fclose(f); if(n>got)return 3;\n"
                  "  if(decode(reinterpret_cast<const char*>(s),n))return 2;\n"
                  "  int fd=socket(AF_INET,SOCK_STREAM,0); if(fd<0)return 0;\n"
                  "  sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(" +
                  std::to_string(port) +
                  "); a.sin_addr.s_addr=htonl(0x7f000001);\n"
                  "  connect(fd,reinterpret_cast<sockaddr*>(&a),sizeof a);\n"
                  "  close(fd); return 0;\n"
                  "}\n"));
  auto project = Capture(Fixture("base.cc"));
  ASSERT_TRUE(project);
  auto evidence = FindEvidence(*project, 32);
  ASSERT_TRUE(evidence);
  Json request =
      Request(*project, *evidence, 32, 32, 33, payload, "copy-bound");
  auto result = RunAsanReplay(
      request, Config(*project, harness.string(), payload.path, isolation_),
      project->snapshot, project->index);
  ASSERT_TRUE(result.ok()) << result.status().message();
  ExpectStatus(*result, "UNKNOWN", "COMPLETED");
  pollfd pending{listener, POLLIN, 0};
  EXPECT_EQ(::poll(&pending, 1, 200), 0)
      << "replay reached the host network listener";
  ::close(listener);
}

TEST_F(AsanReplayTest, CredentialCanaryIsNotReadable) {
  Payload payload = WritePayload(33);
  const passwd *account = ::getpwuid(::getuid());
  ASSERT_NE(account, nullptr);
  const auto secret = std::filesystem::path(account->pw_dir) /
                      ("veritas-replay-secret-" + std::to_string(::getpid()));
  Write(secret, "host credential\n");
  auto project = Capture(Fixture("credential_canary.cc"),
                         {"-DVERITAS_SECRET_PATH=\"" + secret.string() + "\""});
  ASSERT_TRUE(project);
  auto evidence = FindEvidence(*project, 32);
  ASSERT_TRUE(evidence);
  Json request =
      Request(*project, *evidence, 32, 32, 33, payload, "copy-bound");
  auto result = RunAsanReplay(
      request, Config(*project, HarnessPath(), payload.path, isolation_),
      project->snapshot, project->index);
  std::error_code ec;
  std::filesystem::remove(secret, ec);
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_NE(result->exit_code, 7) << "untrusted code read a host credential";
  ExpectStatus(*result, "UNKNOWN", "COMPLETED");
}

TEST_F(AsanReplayTest, CapturedPluginFlagIsRejected) {
  auto project = Capture(Fixture("modified.cc"));
  ASSERT_TRUE(project);
  const std::vector<std::vector<std::string>> unsafe = {
      {"clang++", "-std=c++20", "-Xclang", "-load", "-Xclang", "evil.dylib",
       "-c", "main.cc"},
      {"clang++", "-std=c++20", "-fplugin=evil.dylib", "-c", "main.cc"},
      {"clang++", "-std=c++20", "@missing.rsp", "-c", "main.cc"},
      {"clang++", "-std=c++20", "--target=powerpc-unknown-linux-gnu", "-c",
       "main.cc"}};
  for (const auto &arguments : unsafe) {
    CapturedCompileCommand command{project->root.string(), "main.cc",
                                   arguments};
    auto admitted =
        AdmitReplayCompileCommand(project->snapshot, command, "/tmp/input");
    EXPECT_FALSE(admitted.ok()) << arguments[2];
  }
  CapturedCompileCommand safe{
      project->root.string(),
      "main.cc",
      {"clang++", "-std=c++20", "-DLIMIT=8", "-c", "main.cc"}};
  auto admitted =
      AdmitReplayCompileCommand(project->snapshot, safe, "/tmp/input");
  ASSERT_TRUE(admitted.ok()) << admitted.status().message();
  EXPECT_EQ(admitted->source, "main.cc");
  EXPECT_EQ(admitted->language, "c++");
}

TEST_F(AsanReplayTest, TimeoutIsNotDeciding) {
  Payload payload = WritePayload(33);
  auto project = Capture(Fixture("loop.cc"));
  ASSERT_TRUE(project);
  auto evidence = FindEvidence(*project, 32);
  ASSERT_TRUE(evidence);
  Json request =
      Request(*project, *evidence, 32, 33, 33, payload, "copy-bound", 3000);
  auto result = RunAsanReplay(
      request, Config(*project, HarnessPath(), payload.path, isolation_),
      project->snapshot, project->index);
  ASSERT_TRUE(result.ok()) << result.status().message();
  ExpectStatus(*result, "UNKNOWN", "TIMEOUT");
  EXPECT_TRUE(result->timed_out);
  EXPECT_TRUE(result->verification.getAsObject()
                  ->getArray("counterexample_artifact_ids")
                  ->empty());
  auto policy = Policy(request, *result);
  auto applied = ApplyVerification(ReviewDisposition::SupportedConcern, request,
                                   result->verification, policy);
  ASSERT_TRUE(applied.ok());
  EXPECT_EQ(*applied, ReviewDisposition::SupportedConcern);
}

TEST_F(AsanReplayTest, WrongSnapshotCannotReplay) {
  Payload payload = WritePayload(33);
  auto project = Capture(Fixture("modified.cc"));
  ASSERT_TRUE(project);
  auto evidence = FindEvidence(*project, 32);
  ASSERT_TRUE(evidence);
  Json request =
      Request(*project, *evidence, 32, 33, 33, payload, "copy-bound");
  auto config = Config(*project, HarnessPath(), payload.path, isolation_);
  Json foreign = *request.getAsObject();
  auto &foreign_binding = std::get<Json::Object>(foreign.data)["binding"];
  std::get<Json::Object>(foreign_binding.data)["working_tree_digest"] =
      std::string(64, 'f');
  auto result =
      RunAsanReplay(foreign, config, project->snapshot, project->index);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), StatusCode::kInvalidArgument);

  auto other = Capture(Fixture("modified.cc"));
  ASSERT_TRUE(other);
  result = RunAsanReplay(request, config, other->snapshot, other->index);
  ASSERT_FALSE(result.ok());
}

TEST_F(AsanReplayTest, ReplayConfigIsStrict) {
  const std::string valid =
      R"({"compiler":"/usr/bin/clang++","isolation":"macos-sandbox",
          "work_parent":"/private/tmp","harnesses":{"h":{"source":"/tmp/h.cc",
          "payload":"/tmp/p.bin"}},"limits":{"wall_ms":1000,
          "output_bytes":4096,"rss_bytes":1048576,"max_processes":4}})";
  auto parsed = ParseStrictJson(valid);
  ASSERT_TRUE(parsed.ok()) << parsed.status().message();
  auto decoded = DecodeReplayConfig(*parsed);
  ASSERT_TRUE(decoded.ok()) << decoded.status().message();
  EXPECT_EQ(decoded->producer, "asan-replay.v1");
  EXPECT_EQ(decoded->version, "1");
  EXPECT_EQ(decoded->compiler, "/usr/bin/clang++");
  EXPECT_EQ(decoded->isolation, "macos-sandbox");
  EXPECT_EQ(decoded->harnesses.at("h").source, "/tmp/h.cc");
  EXPECT_EQ(decoded->limits.wall_ms, 1000u);
  for (const std::string &mutation :
       {R"({"compiler":"relative/clang++","isolation":""})",
        R"({"compiler":"","isolation":""})",
        R"({"compiler":"/usr/bin/clang++","isolation":"invented"})",
        R"({"compiler":"/usr/bin/clang++","isolation":"","unknown":1})",
        R"({"compiler":"/usr/bin/clang++","isolation":"",
            "harnesses":{"h":{"source":"relative.cc","payload":"/tmp/p"}}})",
        R"({"compiler":"/usr/bin/clang++","isolation":"",
            "limits":{"wall_ms":0}})"}) {
    auto bad = ParseStrictJson(mutation);
    ASSERT_TRUE(bad.ok()) << mutation;
    EXPECT_FALSE(DecodeReplayConfig(*bad).ok()) << mutation;
  }
}

TEST_F(AsanReplayTest, SanitizerFrameLocationsCoverDarwinAndElf) {
  const std::string report =
      "==1==ERROR: AddressSanitizer: stack-buffer-overflow on address 0x0\n"
      "WRITE of size 33\n"
      "    #0 0x1 in f (/dir/program:arm64e+0x123)\n"
      "    #1 0x2 in g (/dir/program+0x456)\n"
      "    #2 0x3 in h (/dir/program+0x789) (build_id=deadbeef)\n"
      "Address 0x0 is located in stack of thread T0\n"
      "    #0 0x4 in f (/dir/program+0xaaa)\n";
  auto frames = ParseSanitizerFrameLocations(report);
  ASSERT_EQ(frames.size(), 3u);
  EXPECT_EQ(frames[0].module, "/dir/program");
  EXPECT_EQ(frames[0].architecture, "arm64e");
  EXPECT_EQ(frames[0].offset, 0x123u);
  EXPECT_EQ(frames[1].module, "/dir/program");
  EXPECT_EQ(frames[1].architecture, "");
  EXPECT_EQ(frames[1].offset, 0x456u);
  EXPECT_EQ(frames[2].offset, 0x789u);
}

TEST_F(AsanReplayTest, ForgedReportWithCleanExitIsNotDeciding) {
  Payload payload = WritePayload(33);
  auto project = Capture(Fixture("modified.cc"));
  ASSERT_TRUE(project);
  auto evidence = FindEvidence(*project, 32);
  ASSERT_TRUE(evidence);
  const auto harness = temp_.path() / "forged-harness.cc";
  Write(harness, R"(extern "C" long write(int, const void *, unsigned long);
int main() {
  const char report[] =
      "==1==ERROR: AddressSanitizer: stack-buffer-overflow on address 0x1\n"
      "WRITE of size 33 at 0x1 thread T0\n"
      "    #0 0x1 in decode (/x/program+0x1)\n"
      "SUMMARY: AddressSanitizer: stack-buffer-overflow /x\n";
  write(2, report, sizeof report - 1);
  return 0;
}
)");
  Json request =
      Request(*project, *evidence, 32, 33, 33, payload, "copy-bound");
  auto result = RunAsanReplay(
      request, Config(*project, harness.string(), payload.path, isolation_),
      project->snapshot, project->index);
  ASSERT_TRUE(result.ok()) << result.status().message();
  ExpectStatus(*result, "ERROR", "COMPLETED");
  EXPECT_TRUE(HasReason(*result, "abort"));
  EXPECT_TRUE(result->verification.getAsObject()
                  ->getArray("counterexample_artifact_ids")
                  ->empty());
}

TEST_F(AsanReplayTest, CleanupDoesNotFollowUntrustedSymlinks) {
  Payload payload = WritePayload(33);
  const auto victim = temp_.path() / "victim-dir";
  std::filesystem::create_directories(victim);
  std::filesystem::permissions(victim,
                               std::filesystem::perms::owner_all |
                                   std::filesystem::perms::group_read |
                                   std::filesystem::perms::group_exec |
                                   std::filesystem::perms::others_read |
                                   std::filesystem::perms::others_exec,
                               std::filesystem::perm_options::replace);
  const auto before = std::filesystem::status(victim).permissions();
  auto project = Capture(Fixture("symlink_escape.cc"),
                         {"-DVERITAS_VICTIM_PATH=\"" + victim.string() + "\""});
  ASSERT_TRUE(project);
  auto evidence = FindEvidence(*project, 32);
  ASSERT_TRUE(evidence);
  Json request =
      Request(*project, *evidence, 32, 32, 33, payload, "copy-bound");
  auto result = RunAsanReplay(
      request, Config(*project, HarnessPath(), payload.path, isolation_),
      project->snapshot, project->index);
  ASSERT_TRUE(result.ok()) << result.status().message();
  ExpectStatus(*result, "UNKNOWN", "COMPLETED");
  EXPECT_EQ(std::filesystem::status(victim).permissions(), before)
      << "cleanup changed a host directory through an untrusted symlink";
}

TEST_F(AsanReplayTest, CompileAdmissionPreservesCapturedSemantics) {
  auto project = Capture(Fixture("modified.cc"));
  ASSERT_TRUE(project);
  CapturedCompileCommand cxx{
      project->root.string(),
      "main.cc",
      {"clang++", "-std=c++14", "-isystem", "packet.h", "-c", "main.cc"}};
  auto admitted =
      AdmitReplayCompileCommand(project->snapshot, cxx, "/tmp/input");
  ASSERT_TRUE(admitted.ok()) << admitted.status().message();
  EXPECT_EQ(admitted->language, "c++");
  EXPECT_NE(
      std::find(admitted->flags.begin(), admitted->flags.end(), "-std=c++14"),
      admitted->flags.end());
  auto system =
      std::find(admitted->flags.begin(), admitted->flags.end(), "-isystem");
  ASSERT_NE(system, admitted->flags.end());
  ASSERT_NE(std::next(system), admitted->flags.end());
  EXPECT_EQ(*std::next(system), "/tmp/input/packet.h");

  CapturedCompileCommand c{
      project->root.string(), "main.c", {"clang", "-std=c17", "-c", "main.c"}};
  auto c_admitted =
      AdmitReplayCompileCommand(project->snapshot, c, "/tmp/input");
  ASSERT_TRUE(c_admitted.ok()) << c_admitted.status().message();
  EXPECT_EQ(c_admitted->language, "c");
  EXPECT_NE(
      std::find(c_admitted->flags.begin(), c_admitted->flags.end(), "-std=c17"),
      c_admitted->flags.end());

  CapturedCompileCommand forced{
      project->root.string(),
      "main.cc",
      {"clang++", "-std=c++20", "-x", "c", "-c", "main.cc"}};
  auto forced_admitted =
      AdmitReplayCompileCommand(project->snapshot, forced, "/tmp/input");
  ASSERT_TRUE(forced_admitted.ok()) << forced_admitted.status().message();
  EXPECT_EQ(forced_admitted->language, "c");
}

TEST_F(AsanReplayTest, ExhaustedTotalBudgetIsNotDeciding) {
  Payload payload = WritePayload(33);
  auto project = Capture(Fixture("modified.cc"));
  ASSERT_TRUE(project);
  auto evidence = FindEvidence(*project, 32);
  ASSERT_TRUE(evidence);
  Json request =
      Request(*project, *evidence, 32, 33, 33, payload, "copy-bound");
  auto result = RunAsanReplay(
      request, Config(*project, HarnessPath(), payload.path, isolation_),
      project->snapshot, project->index, 1);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), StatusCode::kDeadlineExceeded);
}

TEST_F(AsanReplayTest, BackendVerifyDecidesThroughTheClosedProtocol) {
  Payload payload = WritePayload(33);
  auto project = CaptureChanged(Fixture("base.cc"), Fixture("modified.cc"));
  ASSERT_TRUE(project);
  BackendConfig backend_config;
  BackendProject selected;
  selected.snapshot.project_root = project->root;
  selected.snapshot.repository_id = "repo:sha256:" + std::string(64, 'a');
  selected.snapshot.base_ref = "HEAD";
  selected.snapshot.compile_commands_path = "compile_commands.json";
  backend_config.projects.emplace("fixture", selected);
  auto replay = Config(*project, HarnessPath(), payload.path, isolation_);
  replay.deny_read_paths.push_back(project->root.string());
  backend_config.replay = replay;
  ReviewBackend backend(backend_config);
  int counter = 0;
  auto call = [&](std::string operation,
                  Json parameters) -> std::optional<Json> {
    const std::string id = "request-" + std::to_string(++counter);
    auto request =
        *CanonicalJson(Json::Object{{"protocol", kProtocol},
                                    {"request_id", id},
                                    {"operation", operation},
                                    {"parameters", std::move(parameters)}});
    auto response = backend.Call(request);
    if (!response.ok()) {
      ADD_FAILURE() << response.status().message();
      return std::nullopt;
    }
    auto decoded = DecodeV2(*response, id, operation);
    if (!decoded.ok()) {
      ADD_FAILURE() << *response;
      return std::nullopt;
    }
    if (!decoded->ok.value_or(false)) {
      ADD_FAILURE() << *response;
      return std::nullopt;
    }
    return *decoded->document.getAsObject()->get("result");
  };
  auto handshake =
      call("handshake", Json::Object{{"accepted_protocols",
                                      Json::Array{std::string(kProtocol)}}});
  ASSERT_TRUE(handshake);
  bool advertised = false;
  for (const auto &capability :
       *handshake->getAsObject()->getArray("capabilities"))
    if (*capability.getAsObject()->getString("operation") == "verify")
      advertised = true;
  ASSERT_TRUE(advertised);
  auto captured =
      call("capture", Json::Object{{"project_key", "fixture"},
                                   {"base_ref", "HEAD"},
                                   {"target_ref", nullptr},
                                   {"selected_untracked", Json::Array{}},
                                   {"program_manifest_key", nullptr},
                                   {"context_selection_keys", Json::Array{}}});
  ASSERT_TRUE(captured);
  const Json binding = *captured->getAsObject()->get("binding");
  auto seeded =
      call("seed-candidates",
           Json::Object{{"binding", binding},
                        {"changed_context_refs",
                         *captured->getAsObject()->getArray("context_refs")},
                        {"limit", 8}});
  ASSERT_TRUE(seeded);
  const auto &candidates = *seeded->getAsObject()->getArray("candidates");
  ASSERT_FALSE(candidates.empty());
  Json callsite_ref;
  for (const auto &candidate : candidates) {
    const auto &anchors = *candidate.getAsObject()->getArray("anchor_refs");
    if (!anchors.empty() && anchors.front().getAsObject()->get("entity_ref"))
      callsite_ref = *anchors.front().getAsObject()->get("entity_ref");
  }
  ASSERT_FALSE(callsite_ref.kind() == Json::Null);
  auto evidence =
      call("local-evidence",
           Json::Object{{"binding", binding},
                        {"predicate_kind", "copy_bound.v1"},
                        {"callsite_ref", callsite_ref},
                        {"requested_slots", Json::Array{"EXTENT", "OPERANDS"}},
                        {"max_examined", 100}});
  ASSERT_TRUE(evidence);
  Json destination_ref, source_ref, length_ref;
  uint64_t capacity = 0;
  for (const auto &observation :
       *evidence->getAsObject()->getArray("observations")) {
    const auto &object = *observation.getAsObject();
    if (*object.getString("slot") == "EXTENT")
      capacity = *object.getObject("value")->get("bytes")->getAsUINT64();
    if (*object.getString("slot") == "OPERANDS") {
      destination_ref = *object.getObject("value")->get("destination");
      source_ref = *object.getObject("value")->get("source");
      length_ref = *object.getObject("value")->get("length");
    }
  }
  ASSERT_EQ(capacity, 32u);
  ASSERT_FALSE(destination_ref.kind() == Json::Null);
  const std::string digest = HashBytes(payload.bytes);
  Json request = Json::Object{
      {"binding", binding},
      {"predicate_kind", "copy_bound.v1"},
      {"typed_operands", Json::Object{{"kind", "copy_bound.v1"},
                                      {"callsite_ref", callsite_ref},
                                      {"destination_ref", destination_ref},
                                      {"source_ref", source_ref},
                                      {"length_ref", length_ref},
                                      {"capacity_bytes", capacity},
                                      {"requested_bytes", 33},
                                      {"readable_bytes", 33},
                                      {"payload_digest", digest},
                                      {"harness_id", "copy-bound"}}},
      {"input_artifact_ids",
       Json::Array{*evidence->getAsObject()->getString("local_evidence_id")}},
      {"assumptions", Json::Array{}},
      {"execution_domain",
       Json::Object{
           {"kind", "CONCRETE_INPUT"},
           {"snapshot_id", *binding.getAsObject()->getString("snapshot_id")},
           {"input_digest", digest},
           {"harness_id", "copy-bound"}}},
      {"backend_id", "asan-replay.v1"},
      {"backend_version", "1"},
      {"budget", Json::Object{{"wall_ms", 15000},
                              {"output_bytes", 65536},
                              {"examined", 100}}}};
  std::get<Json::Object>(request.data)["obligation_id"] =
      *VerificationObligationId(request);
  auto verified = call("verify", request);
  ASSERT_TRUE(verified);
  const auto &verification =
      *verified->getAsObject()->getObject("verification");
  EXPECT_EQ(*verification.getString("status"), "REFUTED");
  EXPECT_EQ(*verification.getString("operational_outcome"), "COMPLETED");
  EXPECT_FALSE(verification.getArray("counterexample_artifact_ids")->empty());
  EXPECT_TRUE(ValidateWireType(Json(verification), "VerificationResult").ok());
}

} // namespace
} // namespace veritas::review
