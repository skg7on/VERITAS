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

#include "veritas/review/SnapshotCapture.h"
#include "veritas/review/ProgramScope.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace veritas::review {
namespace {
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

class TempDir {
public:
  TempDir() {
    std::string pattern = "/private/tmp/veritas-snapshot-XXXXXX";
#ifndef __APPLE__
    pattern = "/tmp/veritas-snapshot-XXXXXX";
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
class SnapshotCaptureTest : public ::testing::Test {
protected:
  TempDir temp_;
  std::filesystem::path root_ = temp_.path() / "project";
  void SetUp() override {
    std::filesystem::create_directories(root_);
    ASSERT_EQ(Git(root_, {"init", "-q"}), 0);
    Write(root_ / "src/a.cc", "base");
    Write(root_ / "lib.cc", "library");
    Write(root_ / "test.cc", "test");
    ASSERT_EQ(Git(root_, {"add", "."}), 0);
    ASSERT_EQ(Git(root_, {"-c", "user.name=Review Test", "-c",
                          "user.email=review@example.invalid", "commit", "-qm",
                          "base"}),
              0);
  }
  SnapshotRequest Request() const {
    SnapshotRequest r;
    r.project_root = root_;
    r.repository_id = "repo:sha256:" + std::string(64, 'a');
    return r;
  }
};
TEST_F(SnapshotCaptureTest, DirtySourceIsCapturedOnce) {
  Write(root_ / "src/a.cc", "before");
  auto snap = CaptureReviewSnapshot(Request());
  ASSERT_TRUE(snap.ok()) << snap.status().message();
  EXPECT_EQ(*snap->Read("src/a.cc"), "before");
  EXPECT_EQ(*snap->ReadBase("src/a.cc"), "base");
  auto context = snap->Context("src/a.cc", "SOURCE", 0, 6);
  ASSERT_TRUE(context.ok());
  EXPECT_EQ(context->excerpt, "before");
  Write(root_ / "src/a.cc", "after");
  EXPECT_EQ(*snap->Read("src/a.cc"), "before");
  auto pinned = snap->Context("src/a.cc", "SOURCE", 0, 6);
  ASSERT_TRUE(pinned.ok());
  EXPECT_EQ(pinned->excerpt, "before");
  EXPECT_EQ(pinned->context_id, context->context_id);
  ASSERT_TRUE(snap->LiveTreeAdvanced().ok());
  EXPECT_TRUE(*snap->LiveTreeAdvanced());
  EXPECT_FALSE(snap->binding().native_binding.has_value());
  auto worker = temp_.path() / "worker";
  ASSERT_TRUE(snap->Materialize(worker).ok());
  std::ifstream input(worker / "src/a.cc");
  std::string bytes;
  input >> bytes;
  EXPECT_EQ(bytes, "before");
  EXPECT_FALSE(snap->Materialize(worker).ok());
}
TEST_F(SnapshotCaptureTest, UntrackedRequiresExplicitSelection) {
  Write(root_ / "new.cc", "new");
  auto snap = CaptureReviewSnapshot(Request());
  ASSERT_TRUE(snap.ok());
  EXPECT_FALSE(snap->Read("new.cc").ok());
  EXPECT_NE(
      std::find(snap->excluded().begin(), snap->excluded().end(), "new.cc"),
      snap->excluded().end());
  auto r = Request();
  r.selected_untracked = {"new.cc"};
  auto selected = CaptureReviewSnapshot(r);
  ASSERT_TRUE(selected.ok());
  EXPECT_EQ(*selected->Read("new.cc"), "new");
  r.selected_untracked = {"src/a.cc"};
  EXPECT_FALSE(CaptureReviewSnapshot(r).ok());
}
TEST_F(SnapshotCaptureTest, OutputHintsDoNotCloseMembership) {
  Write(
      root_ / "compile_commands.json",
      R"([{"directory":".","file":"src/a.cc","arguments":["clang++","-c","src/a.cc","-o","app/a.o"]},{"directory":".","file":"test.cc","arguments":["clang++","-c","test.cc","-o","app/test.o"]}])");
  auto r = Request();
  r.compile_commands_path = "compile_commands.json";
  auto snap = CaptureReviewSnapshot(r);
  ASSERT_TRUE(snap.ok()) << snap.status().message();
  EXPECT_EQ(snap->program_scope().coverage, "OPEN");
  EXPECT_EQ(snap->program_scope().membership_source, "OUTPUT_HINT");
  EXPECT_EQ(snap->program_scope().member_tu_digests.size(), 2u);
}
TEST_F(SnapshotCaptureTest, SharedLibraryDependencyIsNotDropped) {
  Write(
      root_ / "program.json",
      R"({"version":"veritas.review.program.v1","targets":[{"key":"app","kind":"EXECUTABLE","translation_units":["src/a.cc"],"dependencies":["shared"],"artifacts":[]},{"key":"shared","kind":"SHARED_LIBRARY","translation_units":["lib.cc"],"dependencies":[],"artifacts":[]},{"key":"tests","kind":"TEST","translation_units":["test.cc"],"dependencies":[],"artifacts":[]}]})");
  auto r = Request();
  r.program_manifest_path = "program.json";
  r.program_target_key = "app";
  auto snap = CaptureReviewSnapshot(r);
  ASSERT_TRUE(snap.ok()) << snap.status().message();
  const auto &scope = snap->program_scope();
  EXPECT_EQ(scope.coverage, "DECLARED_CLOSED");
  EXPECT_EQ(scope.member_tu_digests.size(), 2u);
  EXPECT_FALSE(scope.assumptions.empty());
  r.membership_source = "BUILD_LINK_ARTIFACT";
  auto linked = CaptureReviewSnapshot(r);
  ASSERT_TRUE(linked.ok());
  EXPECT_EQ(linked->program_scope().membership_source, "BUILD_LINK_ARTIFACT");
  EXPECT_EQ(linked->program_scope().member_tu_digests.size(), 2u);
  r.program_target_key = "shared";
  auto library = CaptureReviewSnapshot(r);
  ASSERT_TRUE(library.ok());
  EXPECT_EQ(library->program_scope().member_tu_digests.size(), 1u);
}
TEST_F(SnapshotCaptureTest, MissingDependencyKeepsScopeUnresolved) {
  Write(
      root_ / "program.json",
      R"({"version":"veritas.review.program.v1","targets":[{"key":"app","kind":"EXECUTABLE","translation_units":["src/a.cc"],"dependencies":["missing"],"artifacts":[]}]})");
  auto r = Request();
  r.program_manifest_path = "program.json";
  r.program_target_key = "app";
  auto snap = CaptureReviewSnapshot(r);
  ASSERT_TRUE(snap.ok());
  EXPECT_EQ(snap->program_scope().coverage, "UNRESOLVED");
  EXPECT_FALSE(snap->program_scope().frontier.empty());
}
TEST_F(SnapshotCaptureTest, TraversalAndSymlinkEscapeAreRejected) {
  auto r = Request();
  r.selected_untracked = {"../outside"};
  EXPECT_FALSE(CaptureReviewSnapshot(r).ok());
  Write(temp_.path() / "secret", "secret");
  std::filesystem::create_symlink(temp_.path() / "secret", root_ / "link");
  r.selected_untracked = {"link"};
  EXPECT_FALSE(CaptureReviewSnapshot(r).ok());
  std::filesystem::create_directory_symlink(temp_.path(), root_ / "dirlink");
  r.selected_untracked = {"dirlink/secret"};
  EXPECT_FALSE(CaptureReviewSnapshot(r).ok());
  std::filesystem::create_symlink(root_ / "src/a.cc", root_ / "internal");
  r.selected_untracked = {"internal"};
  EXPECT_FALSE(CaptureReviewSnapshot(r).ok());
  r = Request();
  r.base_ref = "--upload-pack=bad";
  EXPECT_FALSE(CaptureReviewSnapshot(r).ok());
}
TEST_F(SnapshotCaptureTest,
       MissingHeaderIsPartialAndGeneratedIncludesArePinned) {
  Write(
      root_ / "compile_commands.json",
      R"([{"directory":".","file":"src/a.cc","arguments":["clang++","-Igenerated","-include","missing.h","-c","src/a.cc"]}])");
  Write(root_ / "generated/config.h", "before");
  auto r = Request();
  r.compile_commands_path = "compile_commands.json";
  r.selected_artifacts = {"generated"};
  auto snap = CaptureReviewSnapshot(r);
  ASSERT_TRUE(snap.ok()) << snap.status().message();
  EXPECT_FALSE(snap->compiler_coverage().supported);
  EXPECT_FALSE(snap->compiler_coverage().reasons.empty());
  Write(root_ / "generated/config.h", "after");
  EXPECT_EQ(*snap->Read("generated/config.h"), "before");
  auto changed = CaptureReviewSnapshot(r);
  ASSERT_TRUE(changed.ok());
  EXPECT_NE(changed->binding().build_context_digest,
            snap->binding().build_context_digest);
}
TEST_F(SnapshotCaptureTest, DeletionsAndBuildConfigurationChangeIdentity) {
  auto a = CaptureReviewSnapshot(Request());
  ASSERT_TRUE(a.ok());
  std::filesystem::remove(root_ / "src/a.cc");
  auto b = CaptureReviewSnapshot(Request());
  ASSERT_TRUE(b.ok());
  EXPECT_FALSE(b->Read("src/a.cc").ok());
  EXPECT_EQ(*b->ReadBase("src/a.cc"), "base");
  EXPECT_NE(a->binding().snapshot_id, b->binding().snapshot_id);
  auto r = Request();
  r.provider_versions = {{"local", "1", std::string(64, 'b')}};
  auto c = CaptureReviewSnapshot(r);
  ASSERT_TRUE(c.ok());
  EXPECT_NE(b->binding().snapshot_id, c->binding().snapshot_id);
}
TEST_F(SnapshotCaptureTest, FileChangingDuringCaptureIsCoherentOrRefused) {
  const std::string before(4 * 1024 * 1024, 'a'), after(4 * 1024 * 1024, 'b');
  Write(root_ / "big.h", before);
  auto r = Request();
  r.selected_untracked = {"big.h"};
  std::atomic<bool> stop = false;
  std::thread writer([&] {
    while (!stop) {
      Write(root_ / "replacement", after);
      std::filesystem::rename(root_ / "replacement", root_ / "big.h");
      Write(root_ / "replacement", before);
      std::filesystem::rename(root_ / "replacement", root_ / "big.h");
    }
  });
  auto snap = CaptureReviewSnapshot(r);
  stop = true;
  writer.join();
  if (snap.ok()) {
    auto bytes = snap->Read("big.h");
    ASSERT_TRUE(bytes.ok());
    EXPECT_TRUE(*bytes == before || *bytes == after);
  } else
    EXPECT_EQ(snap.status().code(), StatusCode::kFailedPrecondition);
}

TEST_F(SnapshotCaptureTest, CheckoutPathsAndAuditStampsDoNotChangeIdentity) {
  auto database = [](const std::filesystem::path &root) {
    return "[{\"directory\":\"" + root.string() + "\",\"file\":\"" +
           (root / "src/a.cc").string() +
           "\",\"arguments\":[\"clang++\",\"-c\",\"" +
           (root / "src/a.cc").string() + "\"]}]";
  };
  Write(root_ / "compile_commands.json", database(root_));
  auto r = Request();
  r.compile_commands_path = "compile_commands.json";
  auto first = CaptureReviewSnapshot(r);
  ASSERT_TRUE(first.ok());
  auto moved = temp_.path() / "moved";
  std::filesystem::copy(root_, moved, std::filesystem::copy_options::recursive);
  Write(moved / "compile_commands.json", database(moved));
  r.project_root = moved;
  auto relocated = CaptureReviewSnapshot(r);
  ASSERT_TRUE(relocated.ok()) << relocated.status().message();
  EXPECT_EQ(first->binding().snapshot_id, relocated->binding().snapshot_id);
  Write(root_ / "src/a.cc", "base");
  r.project_root = root_;
  auto touched = CaptureReviewSnapshot(r);
  ASSERT_TRUE(touched.ok());
  EXPECT_EQ(first->binding().snapshot_id, touched->binding().snapshot_id);
}
TEST_F(SnapshotCaptureTest, CapturedTestAndDocArtifactsHaveTypedContext) {
  Write(root_ / "docs/contract.md", "contract");
  Write(root_ / "generated/header.h", "header");
  auto r = Request();
  r.selected_artifacts = {"docs", "generated"};
  auto snap = CaptureReviewSnapshot(r);
  ASSERT_TRUE(snap.ok());
  auto doc = snap->Context("docs/contract.md", "DOC", 0, 8);
  ASSERT_TRUE(doc.ok());
  EXPECT_EQ(doc->excerpt, "contract");
  EXPECT_TRUE(doc->entity_bindings.empty());
  EXPECT_FALSE(snap->Context("docs/contract.md", "DOC", 8, 9).ok());
  EXPECT_FALSE(snap->Read("../secret").ok());
}
TEST_F(SnapshotCaptureTest, InvalidManifestAndBudgetAreRefused) {
  Write(
      root_ / "program.json",
      R"({"version":"veritas.review.program.v1","targets":[],"authority":"FORMAL"})");
  auto r = Request();
  r.program_manifest_path = "program.json";
  r.program_target_key = "app";
  EXPECT_FALSE(CaptureReviewSnapshot(r).ok());
  r = Request();
  r.max_bytes = 1;
  EXPECT_FALSE(CaptureReviewSnapshot(r).ok());
  r.max_bytes = 0;
  EXPECT_FALSE(CaptureReviewSnapshot(r).ok());
}

TEST_F(SnapshotCaptureTest, BaseSymlinksAreNotSourceBlobs) {
  std::filesystem::create_symlink("src/a.cc", root_ / "old-link");
  ASSERT_EQ(Git(root_, {"add", "old-link"}), 0);
  ASSERT_EQ(Git(root_,
                {"-c", "user.name=Review Test", "-c",
                 "user.email=review@example.invalid", "commit", "-qm", "link"}),
            0);
  std::filesystem::remove(root_ / "old-link");
  Write(root_ / "old-link", "regular now");
  auto r = Request();
  r.selected_tracked = {"old-link"};
  EXPECT_FALSE(CaptureReviewSnapshot(r).ok());
}
TEST_F(SnapshotCaptureTest,
       MaterializationRejectsExistingAndSymlinkDestinations) {
  auto snap = CaptureReviewSnapshot(Request());
  ASSERT_TRUE(snap.ok());
  EXPECT_FALSE(snap->Materialize(root_).ok());
  EXPECT_EQ(*snap->Read("src/a.cc"), "base");
  auto outside = temp_.path() / "outside";
  std::filesystem::create_directory(outside);
  std::filesystem::create_directory_symlink(outside, temp_.path() / "alias");
  EXPECT_FALSE(snap->Materialize(temp_.path() / "alias" / "worker").ok());
  EXPECT_FALSE(std::filesystem::exists(outside / "worker"));
}
TEST_F(SnapshotCaptureTest, DependencyArtifactContentPinsProgramScope) {
  Write(root_ / "libshared.so", "shared-before");
  Write(
      root_ / "program.json",
      R"({"version":"veritas.review.program.v1","targets":[{"key":"lib","kind":"SHARED_LIBRARY","translation_units":["lib.cc"],"dependencies":[],"artifacts":["libshared.so"]}]})");
  auto r = Request();
  r.program_manifest_path = "program.json";
  r.program_target_key = "lib";
  r.selected_artifacts = {"libshared.so"};
  auto before = CaptureReviewSnapshot(r);
  ASSERT_TRUE(before.ok());
  EXPECT_EQ(before->program_scope().dependency_artifact_digests.size(), 1u);
  Write(root_ / "libshared.so", "shared-after");
  auto after = CaptureReviewSnapshot(r);
  ASSERT_TRUE(after.ok());
  EXPECT_EQ(*before->Read("libshared.so"), "shared-before");
  EXPECT_NE(before->binding().program_scope_id,
            after->binding().program_scope_id);
}

TEST_F(SnapshotCaptureTest, SemanticMacroValuesCannotAliasRelocatedPaths) {
  auto database = [](const std::string &macro) {
    auto value = CanonicalJson(Json::Array{
        Json::Object{{"directory", "."},
                     {"file", "src/a.cc"},
                     {"arguments", Json::Array{"clang++", "-DVALUE=" + macro,
                                               "-c", "src/a.cc"}}}});
    return *value;
  };
  Write(root_ / "compile_commands.json", database(root_.string()));
  auto r = Request();
  r.compile_commands_path = "compile_commands.json";
  auto path_macro = CaptureReviewSnapshot(r);
  ASSERT_TRUE(path_macro.ok());
  Write(root_ / "compile_commands.json", database("@REPO@"));
  auto literal_macro = CaptureReviewSnapshot(r);
  ASSERT_TRUE(literal_macro.ok());
  EXPECT_NE(path_macro->binding().snapshot_id,
            literal_macro->binding().snapshot_id);
  EXPECT_NE(path_macro->binding().build_context_digest,
            literal_macro->binding().build_context_digest);
}
TEST_F(SnapshotCaptureTest,
       IgnoredGeneratedDirectoryMembershipAdvancesLiveTree) {
  Write(root_ / ".gitignore", "generated/\n");
  Write(root_ / "generated/a.h", "before");
  auto r = Request();
  r.selected_artifacts = {"generated"};
  auto snap = CaptureReviewSnapshot(r);
  ASSERT_TRUE(snap.ok());
  ASSERT_TRUE(snap->LiveTreeAdvanced().ok());
  EXPECT_FALSE(*snap->LiveTreeAdvanced());
  Write(root_ / "generated/b.h", "new header");
  ASSERT_TRUE(snap->LiveTreeAdvanced().ok());
  EXPECT_TRUE(*snap->LiveTreeAdvanced());
  EXPECT_FALSE(snap->Read("generated/b.h").ok());
}
TEST_F(SnapshotCaptureTest, EverySelectionRouteRejectsBaseSymlinks) {
  std::filesystem::create_symlink("src/a.cc", root_ / "old-link");
  ASSERT_EQ(Git(root_, {"add", "old-link"}), 0);
  ASSERT_EQ(Git(root_,
                {"-c", "user.name=Review Test", "-c",
                 "user.email=review@example.invalid", "commit", "-qm", "link"}),
            0);
  std::filesystem::remove(root_ / "old-link");
  Write(root_ / "old-link", "regular now");
  auto r = Request();
  r.selected_tracked = {"src/a.cc"};
  r.selected_artifacts = {"old-link"};
  EXPECT_FALSE(CaptureReviewSnapshot(r).ok());
  r.selected_artifacts.clear();
  Write(
      root_ / "program.json",
      R"({"version":"veritas.review.program.v1","targets":[{"key":"app","kind":"EXECUTABLE","translation_units":["old-link"],"dependencies":[],"artifacts":[]}]})");
  r.program_manifest_path = "program.json";
  r.program_target_key = "app";
  EXPECT_FALSE(CaptureReviewSnapshot(r).ok());
}
TEST_F(SnapshotCaptureTest, PartialContextRetainsOmittedRegions) {
  Write(root_ / "src/a.cc", "guard\ncopy\ntail\n");
  auto snap = CaptureReviewSnapshot(Request());
  ASSERT_TRUE(snap.ok());
  auto context = snap->Context("src/a.cc", "SOURCE", 6, 10);
  ASSERT_TRUE(context.ok());
  EXPECT_EQ(context->excerpt, "copy");
  ASSERT_EQ(context->omitted_regions.size(), 2u);
  EXPECT_EQ(context->omitted_regions[0].start, 0u);
  EXPECT_EQ(context->omitted_regions[0].end, 6u);
  EXPECT_EQ(context->omitted_regions[1].start, 10u);
  EXPECT_EQ(context->omitted_regions[1].end, 16u);
  auto full = snap->Context("src/a.cc", "SOURCE", 0, 16);
  ASSERT_TRUE(full.ok());
  EXPECT_TRUE(full->omitted_regions.empty());
  EXPECT_NE(full->context_id, context->context_id);
}
} // namespace
} // namespace veritas::review
