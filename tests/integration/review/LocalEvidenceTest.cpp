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

#include "veritas/review/LocalEvidence.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <sys/wait.h>
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
class LocalEvidenceTest : public ::testing::Test {
protected:
  TempDir temp_;
  std::filesystem::path root_ = temp_.path() / "project";
  void SetUp() override {
    std::filesystem::create_directories(root_);
    ASSERT_EQ(Git(root_, {"init", "-q"}), 0);
    std::ifstream input(std::filesystem::path(VERITAS_SOURCE_DIR) /
                        "tests/fixtures/review/local_bounds/sites.cc");
    Write(root_ / "main.cc",
          std::string(std::istreambuf_iterator<char>(input), {}));
    Commands({});
    ASSERT_EQ(Git(root_, {"add", "."}), 0);
    ASSERT_EQ(Git(root_, {"-c", "user.name=Review Test", "-c",
                          "user.email=review@example.invalid", "commit", "-qm",
                          "base"}),
              0);
  }
  void Commands(std::vector<std::string> extra) {
    Json::Array args{"clang++", "-std=c++20",
                     "--target=x86_64-unknown-linux-gnu"};
    for (auto &x : extra)
      args.push_back(x);
    args.push_back("-c");
    args.push_back("main.cc");
    Write(root_ / "compile_commands.json",
          *CanonicalJson(Json::Array{Json::Object{{"directory", root_.string()},
                                                  {"file", "main.cc"},
                                                  {"arguments", args}}}));
  }
  StatusOr<ReviewSnapshot> Capture() {
    SnapshotRequest r;
    r.project_root = root_;
    r.repository_id = "repo:sha256:" + std::string(64, 'a');
    r.compile_commands_path = "compile_commands.json";
    return CaptureReviewSnapshot(r);
  }
  std::vector<EntityRef> Calls(const StructuralIndex &index) {
    std::vector<EntityRef> refs;
    for (const auto &e : index.entities())
      if (e.kind == "CALLSITE")
        refs.push_back({index.id(), e.ordinal});
    return refs;
  }
  StatusOr<LocalEvidence>
  Query(const StructuralIndex &index, EntityRef ref,
        std::vector<std::string> slots = {"EXTENT", "GUARD", "OPERANDS"},
        uint64_t limit = 100) {
    return InvestigateLocal(
        {index, index.binding(), "copy_bound.v1", ref, slots, limit});
  }
  const LocalObservation *Slot(const LocalEvidence &e, std::string_view name) {
    for (const auto &o : e.observations)
      if (o.slot == name)
        return &o;
    return nullptr;
  }
};
TEST_F(LocalEvidenceTest, ArrayExtentIsBytes) {
  auto snap = Capture();
  ASSERT_TRUE(snap.ok()) << snap.status().message();
  auto index = BuildStructuralIndex(*snap, {});
  ASSERT_TRUE(index.ok()) << index.status().message();
  auto calls = Calls(*index);
  ASSERT_EQ(calls.size(), 5u);
  auto evidence = Query(*index, calls[0]);
  ASSERT_TRUE(evidence.ok()) << evidence.status().message();
  ASSERT_NE(Slot(*evidence, "EXTENT"), nullptr);
  EXPECT_EQ(std::get<ByteExtent>(Slot(*evidence, "EXTENT")->value).bytes, 32u);
  ASSERT_NE(Slot(*evidence, "OPERANDS"), nullptr);
  const auto &operands =
      std::get<CopyOperands>(Slot(*evidence, "OPERANDS")->value);
  EXPECT_EQ(index->entities()[operands.destination.ordinal].operand_index, 0u);
  EXPECT_EQ(index->entities()[operands.source.ordinal].operand_index, 1u);
  EXPECT_EQ(index->entities()[operands.length.ordinal].operand_index, 2u);
  EXPECT_EQ(evidence->binding, snap->binding());
  EXPECT_FALSE(evidence->binding.native_binding);
  EXPECT_TRUE(ValidateWireType(ToJson(*evidence), "LocalEvidence").ok());
  EXPECT_TRUE(evidence->local_evidence_id.value().starts_with(
      "local-evidence:sha256:"));
  EXPECT_FALSE(Slot(*evidence, "EXTENT")->source_refs.empty());
}
TEST_F(LocalEvidenceTest, KeepsTwoMemcpySitesSeparate) {
  auto snap = Capture();
  ASSERT_TRUE(snap.ok());
  auto index = BuildStructuralIndex(*snap, {});
  ASSERT_TRUE(index.ok());
  auto calls = Calls(*index);
  ASSERT_EQ(calls.size(), 5u);
  auto a = Query(*index, calls[0]);
  auto b = Query(*index, calls[1]);
  ASSERT_TRUE(a.ok());
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(std::get<ByteExtent>(Slot(*a, "EXTENT")->value).bytes, 32u);
  EXPECT_EQ(std::get<ByteExtent>(Slot(*b, "EXTENT")->value).bytes, 7u);
  EXPECT_NE(a->local_evidence_id, b->local_evidence_id);
  EXPECT_NE(std::get<CopyOperands>(Slot(*a, "OPERANDS")->value).length,
            std::get<CopyOperands>(Slot(*b, "OPERANDS")->value).length);
  ASSERT_NE(Slot(*a, "GUARD"), nullptr);
  EXPECT_TRUE(std::get<LocalGuard>(Slot(*a, "GUARD")->value).dominates);
}
TEST_F(LocalEvidenceTest, SiblingGuardDoesNotDominate) {
  auto snap = Capture();
  ASSERT_TRUE(snap.ok());
  auto index = BuildStructuralIndex(*snap, {});
  ASSERT_TRUE(index.ok());
  auto calls = Calls(*index);
  ASSERT_EQ(calls.size(), 5u);
  auto e = Query(*index, calls[2]);
  ASSERT_TRUE(e.ok());
  ASSERT_NE(Slot(*e, "GUARD"), nullptr);
  EXPECT_FALSE(std::get<LocalGuard>(Slot(*e, "GUARD")->value).dominates);
}
TEST_F(LocalEvidenceTest, WideArrayExtentIsBytes) {
  auto snap = Capture();
  ASSERT_TRUE(snap.ok());
  auto index = BuildStructuralIndex(*snap, {});
  ASSERT_TRUE(index.ok());
  auto calls = Calls(*index);
  ASSERT_EQ(calls.size(), 5u);
  auto e = Query(*index, calls[3]);
  ASSERT_TRUE(e.ok());
  ASSERT_NE(Slot(*e, "EXTENT"), nullptr);
  EXPECT_EQ(std::get<ByteExtent>(Slot(*e, "EXTENT")->value).bytes, 32u);
}
TEST_F(LocalEvidenceTest, PointerExtentIsUnsupported) {
  auto snap = Capture();
  ASSERT_TRUE(snap.ok());
  auto index = BuildStructuralIndex(*snap, {});
  ASSERT_TRUE(index.ok());
  auto calls = Calls(*index);
  ASSERT_EQ(calls.size(), 5u);
  auto e = Query(*index, calls[4]);
  ASSERT_TRUE(e.ok());
  EXPECT_EQ(Slot(*e, "EXTENT"), nullptr);
  EXPECT_FALSE(e->coverage.supported);
  EXPECT_FALSE(e->coverage.reasons.empty());
}
TEST_F(LocalEvidenceTest, MissingHeaderIsPartial) {
  Write(root_ / "main.cc", "#include \"uncaptured.h\"\nvoid f() {}\n");
  auto snap = Capture();
  ASSERT_TRUE(snap.ok());
  auto index = BuildStructuralIndex(*snap, {});
  ASSERT_TRUE(index.ok());
  EXPECT_FALSE(index->coverage().supported);
  EXPECT_EQ(index->coverage().traversal, "INCOMPLETE");
  EXPECT_FALSE(index->coverage().frontier_refs.empty());
}
TEST_F(LocalEvidenceTest, NoNativeAnalysisAndCompilerCostsAreCounted) {
  auto snap = Capture();
  ASSERT_TRUE(snap.ok());
  auto index = BuildStructuralIndex(*snap, {});
  ASSERT_TRUE(index.ok());
  EXPECT_EQ(index->metrics().compiler_invocations, 1u);
  EXPECT_EQ(index->metrics().svf_invocations, 0u);
  EXPECT_EQ(index->metrics().wpa_components_executed, 0u);
  EXPECT_GT(index->usage().peak_rss_bytes, 0u);
  EXPECT_GT(index->usage().wall_ms, 0u);
  EXPECT_GT(index->usage().cpu_ms, 0u);
  EXPECT_GT(index->usage().output_bytes, 0u);
}
TEST_F(LocalEvidenceTest, CapturedBytesAndIncludesIgnoreLiveEdits) {
  Write(root_ / "main.cc",
        "#include \"bound.h\"\nextern \"C\" void *memcpy(void*,const "
        "void*,__SIZE_TYPE__);\nvoid f(const char*s,__SIZE_TYPE__ n){char "
        "d[CAP];memcpy(d,s,n);}\n");
  Write(root_ / "bound.h", "#define CAP 32\n");
  ASSERT_EQ(Git(root_, {"add", "bound.h"}), 0);
  auto snap = Capture();
  ASSERT_TRUE(snap.ok());
  Write(root_ / "bound.h", "#define CAP 99\n");
  Write(root_ / "main.cc", "broken after capture");
  auto index = BuildStructuralIndex(*snap, {});
  ASSERT_TRUE(index.ok());
  auto calls = Calls(*index);
  ASSERT_EQ(calls.size(), 1u);
  auto e = Query(*index, calls[0], {"EXTENT"});
  ASSERT_TRUE(e.ok());
  ASSERT_NE(Slot(*e, "EXTENT"), nullptr);
  EXPECT_EQ(std::get<ByteExtent>(Slot(*e, "EXTENT")->value).bytes, 32u);
  auto ctx = index->Context("main.cc", 0, snap->Read("main.cc")->size());
  ASSERT_TRUE(ctx.ok());
  EXPECT_FALSE(ctx->entity_bindings.empty());
  EXPECT_EQ(ctx->excerpt, *snap->Read("main.cc"));
}
TEST_F(LocalEvidenceTest, MutatedReferenceSizeCannotSupplyADominatingGuard) {
  Write(root_ / "main.cc",
        R"(extern "C" void *memcpy(void*,const void*,__SIZE_TYPE__);
void change(__SIZE_TYPE__ &);
void f(const char*s,__SIZE_TYPE__ n){char d[32];if(n<=sizeof(d)){change(n);memcpy(d,s,n);}}
)");
  auto snap = Capture();
  ASSERT_TRUE(snap.ok());
  auto index = BuildStructuralIndex(*snap, {});
  ASSERT_TRUE(index.ok());
  auto calls = Calls(*index);
  ASSERT_EQ(calls.size(), 2u);
  auto e = Query(*index, calls[1], {"GUARD"});
  ASSERT_TRUE(e.ok());
  EXPECT_EQ(Slot(*e, "GUARD"), nullptr);
  EXPECT_FALSE(e->coverage.supported);
}
TEST_F(LocalEvidenceTest, MacroOperandsAndNonDefaultCopyAbiAreUnsupported) {
  for (
      const auto &source : std::vector<std::string>{
          R"(extern "C" void *memcpy(void*,const void*,__SIZE_TYPE__);
#define DEST d
void f(const char*s,__SIZE_TYPE__ n){char d[32];memcpy(DEST,s,n);})",
          R"(extern "C" __attribute__((ms_abi)) void *memcpy(void*,const void*,__SIZE_TYPE__);
void f(const char*s,__SIZE_TYPE__ n){char d[32];memcpy(d,s,n);})"}) {
    Write(root_ / "main.cc", source);
    auto snap = Capture();
    ASSERT_TRUE(snap.ok());
    auto index = BuildStructuralIndex(*snap, {});
    ASSERT_TRUE(index.ok());
    auto calls = Calls(*index);
    // A compiler refusal is also a partial result, never an extent claim.
    if (calls.empty()) {
      EXPECT_FALSE(index->coverage().supported);
      continue;
    }
    ASSERT_EQ(calls.size(), 1u);
    auto e = Query(*index, calls[0], {"EXTENT"});
    ASSERT_TRUE(e.ok());
    EXPECT_EQ(Slot(*e, "EXTENT"), nullptr);
    EXPECT_FALSE(e->coverage.supported);
  }
}
TEST_F(LocalEvidenceTest, NoAmbientFilesystemFallbackAndResourceRefusals) {
  Write(temp_.path() / "secret.h", "#define CAP 99\n");
  Write(root_ / "main.cc", "#include \"" +
                               (temp_.path() / "secret.h").string() +
                               "\"\nvoid f(){char d[CAP];}\n");
  auto snap = Capture();
  ASSERT_TRUE(snap.ok());
  auto partial = BuildStructuralIndex(*snap, {});
  ASSERT_TRUE(partial.ok());
  EXPECT_FALSE(partial->coverage().supported);
  Write(root_ / "main.cc", "void f() {}\n");
  snap = Capture();
  ASSERT_TRUE(snap.ok());
  StructuralBudget budget;
  budget.output_bytes = 1;
  EXPECT_FALSE(BuildStructuralIndex(*snap, budget).ok());
  budget = {};
  budget.max_source_bytes = 1;
  EXPECT_FALSE(BuildStructuralIndex(*snap, budget).ok());
  budget = {};
  budget.child_rss_bytes = 1;
  EXPECT_FALSE(BuildStructuralIndex(*snap, budget).ok());
}
TEST_F(LocalEvidenceTest, LocalOutputUsageIncludesTheSerializedUsage) {
  auto snap = Capture();
  ASSERT_TRUE(snap.ok());
  auto index = BuildStructuralIndex(*snap, {});
  ASSERT_TRUE(index.ok());
  auto calls = Calls(*index);
  ASSERT_FALSE(calls.empty());
  auto e = Query(*index, calls[0]);
  ASSERT_TRUE(e.ok());
  auto encoded = CanonicalJson(ToJson(*e));
  ASSERT_TRUE(encoded.ok());
  EXPECT_GE(e->usage.output_bytes, encoded->size());
}
TEST_F(LocalEvidenceTest, RejectsUnsafeCompilerFlagsBeforeExecution) {
  auto safe = Capture();
  ASSERT_TRUE(safe.ok());
  ASSERT_TRUE(BuildStructuralIndex(*safe, {}).ok());
  for (const auto &flags : std::vector<std::vector<std::string>>{
           {"-Xclang", "-load", "evil.dylib"},
           {"-fplugin=evil.dylib"},
           {"-include-pch", "evil.pch"},
           {"-fmodules"},
           {"-funknown-semantics"},
           {"-target", "powerpc-unknown-linux-gnu"},
           {"-m32"},
           {"-I", "/usr/include"},
           {"@missing.rsp"},
           {"-D"}}) {
    Commands(flags);
    auto snap = Capture();
    ASSERT_TRUE(snap.ok());
    EXPECT_FALSE(BuildStructuralIndex(*snap, {}).ok());
  }
}
TEST_F(LocalEvidenceTest, CapturedResponseFilesAreBoundedAndValidated) {
  Write(root_ / "args.rsp", "-std=c++20 -DCOUNT=8");
  ASSERT_EQ(Git(root_, {"add", "args.rsp"}), 0);
  Commands({"@args.rsp"});
  auto snap = Capture();
  ASSERT_TRUE(snap.ok());
  EXPECT_TRUE(BuildStructuralIndex(*snap, {}).ok());
  Write(root_ / "args.rsp", "-Xclang -load evil.dylib");
  snap = Capture();
  ASSERT_TRUE(snap.ok());
  EXPECT_FALSE(BuildStructuralIndex(*snap, {}).ok());
  Write(root_ / "args.rsp", "@args.rsp");
  snap = Capture();
  ASSERT_TRUE(snap.ok());
  EXPECT_FALSE(BuildStructuralIndex(*snap, {}).ok());
}
TEST_F(LocalEvidenceTest, MacroCastAndIndirectFormsRetainGaps) {
  Write(root_ / "main.cc",
        R"(extern "C" void *memcpy(void*,const void*,__SIZE_TYPE__);
#define COPY(d,s,n) memcpy(d,s,n)
void f(char*p,const char*s,__SIZE_TYPE__ n){char d[32];COPY(d,s,n);memcpy((void*)d,s,n);memcpy(d,s,(unsigned long)n);auto fn=&memcpy;fn(d,s,n);}
)");
  auto snap = Capture();
  ASSERT_TRUE(snap.ok());
  auto index = BuildStructuralIndex(*snap, {});
  ASSERT_TRUE(index.ok());
  auto calls = Calls(*index);
  ASSERT_EQ(calls.size(), 4u);
  for (const auto &c : calls) {
    auto e = Query(*index, c);
    ASSERT_TRUE(e.ok());
    EXPECT_FALSE(e->coverage.supported);
    EXPECT_EQ(Slot(*e, "EXTENT"), nullptr);
  }
  EXPECT_FALSE(index->entities()[calls.back().ordinal].direct_call);
}
TEST_F(LocalEvidenceTest, CTranslationUnitRetainsItsLanguage) {
  Write(root_ / "main.c",
        R"(extern void *memcpy(void*,const void*,__SIZE_TYPE__);
void f(const char*s,__SIZE_TYPE__ n){int class[8];memcpy(class,s,n);})");
  ASSERT_EQ(Git(root_, {"add", "main.c"}), 0);
  Write(root_ / "compile_commands.json",
        *CanonicalJson(Json::Array{Json::Object{
            {"directory", root_.string()},
            {"file", "main.c"},
            {"arguments", Json::Array{"clang", "-std=c17", "-c", "main.c"}}}}));
  auto snap = Capture();
  ASSERT_TRUE(snap.ok());
  auto index = BuildStructuralIndex(*snap, {});
  ASSERT_TRUE(index.ok());
  EXPECT_TRUE(index->coverage().supported);
  auto calls = Calls(*index);
  ASSERT_EQ(calls.size(), 1u);
  auto e = Query(*index, calls[0], {"EXTENT"});
  ASSERT_TRUE(e.ok());
  ASSERT_NE(Slot(*e, "EXTENT"), nullptr);
  EXPECT_EQ(std::get<ByteExtent>(Slot(*e, "EXTENT")->value).bytes, 32u);
}
TEST_F(LocalEvidenceTest, OnlyDeclaredProgramTranslationUnitsAreParsed) {
  Write(root_ / "other.cc", "void unrelated(){}\n");
  Write(
      root_ / "program.json",
      R"({"version":"veritas.review.program.v1","targets":[{"key":"app","kind":"EXECUTABLE","translation_units":["main.cc"],"dependencies":[],"artifacts":[]}]})");
  ASSERT_EQ(Git(root_, {"add", "other.cc", "program.json"}), 0);
  auto initial = Capture();
  ASSERT_TRUE(initial.ok());
  auto db = ParseStrictJson(*initial->Read("compile_commands.json"));
  ASSERT_TRUE(db.ok());
  auto commands = *db->getAsArray();
  commands.push_back(
      Json::Object{{"directory", root_.string()},
                   {"file", "other.cc"},
                   {"arguments", Json::Array{"clang++", "-c", "other.cc"}}});
  Write(root_ / "compile_commands.json", *CanonicalJson(commands));
  SnapshotRequest r;
  r.project_root = root_;
  r.repository_id = "repo:sha256:" + std::string(64, 'a');
  r.compile_commands_path = "compile_commands.json";
  r.program_manifest_path = "program.json";
  r.program_target_key = "app";
  auto snap = CaptureReviewSnapshot(r);
  ASSERT_TRUE(snap.ok());
  auto index = BuildStructuralIndex(*snap, {});
  ASSERT_TRUE(index.ok());
  EXPECT_EQ(index->metrics().compiler_invocations, 1u);
  EXPECT_TRUE(index->coverage().scope_closed);
  for (const auto &e : index->entities())
    EXPECT_NE(e.path, "other.cc");
}
TEST_F(LocalEvidenceTest, RejectsForeignBindingRefsAndInvalidBudgets) {
  auto snap = Capture();
  ASSERT_TRUE(snap.ok());
  auto index = BuildStructuralIndex(*snap, {});
  ASSERT_TRUE(index.ok());
  auto calls = Calls(*index);
  ASSERT_FALSE(calls.empty());
  LocalRequest r{*index,   index->binding(), "copy_bound.v1",
                 calls[0], {"EXTENT"},       10};
  r.binding.working_tree_digest = std::string(64, 'f');
  EXPECT_FALSE(InvestigateLocal(r).ok());
  r.binding = index->binding();
  r.max_examined = 0;
  EXPECT_FALSE(InvestigateLocal(r).ok());
  r.max_examined = 10;
  r.callsite_ref.ordinal = 99999;
  EXPECT_FALSE(InvestigateLocal(r).ok());
  r.callsite_ref = calls[0];
  r.requested_slots = {"GUARD", "EXTENT"};
  EXPECT_FALSE(InvestigateLocal(r).ok());
  StructuralBudget b;
  b.max_tus = 0;
  EXPECT_FALSE(BuildStructuralIndex(*snap, b).ok());
}
TEST_F(LocalEvidenceTest, TruncationAndIdsPreserveCoverageAndIgnoreTiming) {
  auto snap = Capture();
  ASSERT_TRUE(snap.ok());
  auto a = BuildStructuralIndex(*snap, {});
  auto b = BuildStructuralIndex(*snap, {});
  ASSERT_TRUE(a.ok());
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(a->id(), b->id());
  auto calls = Calls(*a);
  ASSERT_FALSE(calls.empty());
  auto e = Query(*a, calls[0]);
  auto f = Query(*b, {b->id(), calls[0].ordinal});
  ASSERT_TRUE(e.ok());
  ASSERT_TRUE(f.ok());
  EXPECT_EQ(e->local_evidence_id, f->local_evidence_id);
  auto limited = Query(*a, calls[0], {"EXTENT", "GUARD", "OPERANDS"}, 1);
  ASSERT_TRUE(limited.ok());
  EXPECT_EQ(limited->coverage.traversal, "INCOMPLETE");
  EXPECT_FALSE(limited->coverage.frontier_refs.empty());
  StructuralBudget budget;
  budget.max_entities = 2;
  auto truncated = BuildStructuralIndex(*snap, budget);
  ASSERT_TRUE(truncated.ok());
  EXPECT_EQ(truncated->coverage().traversal, "INCOMPLETE");
}
TEST_F(LocalEvidenceTest, ReviewAggregateReferenceEscape) {
  Write(root_ / "main.cc",
        R"(extern "C" void *memcpy(void*,const void*,__SIZE_TYPE__);
struct Ref { __SIZE_TYPE__ &value; };
void f(const char*s,__SIZE_TYPE__ n){char d[32];Ref r{n};if(n<=sizeof(d)){r.value=64;memcpy(d,s,n);}}
)");
  auto snap = Capture();
  ASSERT_TRUE(snap.ok());
  auto index = BuildStructuralIndex(*snap, {});
  ASSERT_TRUE(index.ok());
  auto calls = Calls(*index);
  ASSERT_EQ(calls.size(), 1u);
  auto e = Query(*index, calls[0], {"GUARD"});
  ASSERT_TRUE(e.ok());
  EXPECT_EQ(Slot(*e, "GUARD"), nullptr);
}
TEST_F(LocalEvidenceTest, ReviewConditionalReferenceEscape) {
  Write(root_ / "main.cc",
        R"(extern "C" void *memcpy(void*,const void*,__SIZE_TYPE__);
void change(__SIZE_TYPE__ &);
void f(const char*s,__SIZE_TYPE__ n,bool b){char d[32];if(n<=sizeof(d)){change(b?n:n);memcpy(d,s,n);}}
)");
  auto snap = Capture();
  ASSERT_TRUE(snap.ok());
  auto index = BuildStructuralIndex(*snap, {});
  ASSERT_TRUE(index.ok());
  auto calls = Calls(*index);
  ASSERT_EQ(calls.size(), 2u);
  auto e = Query(*index, calls[1], {"GUARD"});
  ASSERT_TRUE(e.ok());
  EXPECT_EQ(Slot(*e, "GUARD"), nullptr);
}
TEST_F(LocalEvidenceTest, ReviewIndexedContextIdentity) {
  auto snap = Capture();
  ASSERT_TRUE(snap.ok());
  auto full = BuildStructuralIndex(*snap, {});
  ASSERT_TRUE(full.ok());
  StructuralBudget budget;
  budget.max_entities = 2;
  auto partial = BuildStructuralIndex(*snap, budget);
  ASSERT_TRUE(partial.ok());
  auto size = snap->Read("main.cc")->size();
  auto plain = snap->Context("main.cc", "SOURCE", 0, size);
  ASSERT_TRUE(plain.ok());
  auto a = full->Context("main.cc", 0, size);
  ASSERT_TRUE(a.ok());
  auto b = partial->Context("main.cc", 0, size);
  ASSERT_TRUE(b.ok());
  ASSERT_NE(a->entity_bindings.size(), b->entity_bindings.size());
  EXPECT_NE(a->context_id, b->context_id);
  EXPECT_NE(a->context_id, plain->context_id);
}
} // namespace
} // namespace veritas::review
