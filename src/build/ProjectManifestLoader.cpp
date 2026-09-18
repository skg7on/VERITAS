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

#include "veritas/build/ProjectManifestLoader.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <numeric>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/JSONCompilationDatabase.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringExtras.h"
// `getDefaultTargetTriple` moved out of `llvm/Support/Host.h` into
// `llvm/TargetParser/Host.h`; VERITAS links the aggregate `LLVM` component
// library, so the symbol is already available.
#include "llvm/Support/SHA256.h"
#include "llvm/TargetParser/Host.h"

namespace veritas::build {

namespace {

namespace fs = std::filesystem;
namespace tooling = clang::tooling;

// -- domain-separated hashing ------------------------------------------------

llvm::ArrayRef<uint8_t> AsBytes(std::string_view s) {
  return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

std::string DomainHash(std::string_view domain, std::string_view bytes) {
  llvm::SHA256 hash;
  hash.update(AsBytes(domain));
  static constexpr uint8_t separator = 0;
  hash.update(llvm::ArrayRef<uint8_t>(&separator, 1));
  hash.update(AsBytes(bytes));
  return llvm::toHex(hash.final(), /*LowerCase=*/true);
}

std::string TaggedIdentifier(std::string_view kind, std::string_view domain,
                             std::string_view bytes) {
  std::string out(kind);
  out.append(":sha256:");
  out.append(DomainHash(domain, bytes));
  return out;
}

// -- file I/O ----------------------------------------------------------------

StatusOr<std::string> ReadFile(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return Status::FailedPrecondition("cannot read " + path.string());
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

// -- path classification -----------------------------------------------------

// Returns true when `child` lies inside `parent` (both must be canonical).
bool IsWithin(const fs::path& child, const fs::path& parent) {
  const auto p = parent.lexically_normal();
  const auto c = child.lexically_normal();
  auto pi = p.begin();
  auto ci = c.begin();
  for (; pi != p.end() && ci != c.end(); ++pi, ++ci) {
    if (*pi != *ci) return false;
  }
  return pi == p.end();
}

TaggedPath ClassifyPath(const fs::path& absolute, const fs::path& project_root) {
  if (IsWithin(absolute, project_root)) {
    return TaggedPath{
        PathRootKind::kRepository, "repository",
        fs::relative(absolute, project_root).lexically_normal(),
    };
  }
  return TaggedPath{
      PathRootKind::kExternal, "external", absolute.lexically_normal(),
  };
}

// -- argument normalization --------------------------------------------------
//
// argv[0] is reduced to a basename so `/usr/bin/clang++` and `clang++` produce
// the same command bytes. Every other argument is left as the compiler-tool
// wrote it, except that any occurrence of the canonical project-root prefix is
// substituted with the sentinel `<repo>`. That handles both stand-alone paths
// (`/abs/proj/main.cpp`) and joined flags (`-I/abs/proj/include`) without ever
// asking the filesystem "does this string happen to name a file inside the
// project?" — which would speculatively rewrite arguments like `-DFOO=main.cpp`
// or `-o build/foo.o` and mutate the command M4 will replay.

std::string BasenameOf(std::string_view program) {
  const auto slash = program.find_last_of('/');
  if (slash == std::string_view::npos) return std::string(program);
  return std::string(program.substr(slash + 1));
}

std::string SubstituteProjectRoot(std::string_view argument,
                                  std::string_view project_root_string) {
  if (project_root_string.empty() || argument.empty()) {
    return std::string(argument);
  }
  std::string out;
  out.reserve(argument.size());
  std::size_t cursor = 0;
  while (cursor < argument.size()) {
    const auto hit = argument.find(project_root_string, cursor);
    if (hit == std::string_view::npos) {
      out.append(argument.substr(cursor));
      break;
    }
    out.append(argument.substr(cursor, hit - cursor));
    out.append("<repo>");
    cursor = hit + project_root_string.size();
  }
  return out;
}

std::vector<std::string> NormalizeArguments(
    const std::vector<std::string>& raw,
    std::string_view project_root_string) {
  std::vector<std::string> normalized;
  normalized.reserve(raw.size());
  if (raw.empty()) return normalized;
  normalized.push_back(BasenameOf(raw.front()));
  for (std::size_t i = 1; i < raw.size(); ++i) {
    normalized.push_back(SubstituteProjectRoot(raw[i], project_root_string));
  }
  return normalized;
}

// NUL-join keeps composite hash inputs unambiguous: no argument can contain a
// literal `\0`, so the joined string is a bijection over the input vector.
std::string JoinNul(std::initializer_list<std::string_view> parts) {
  std::string joined;
  bool first = true;
  for (const auto part : parts) {
    if (!first) joined.push_back('\0');
    first = false;
    joined.append(part);
  }
  return joined;
}

std::string JoinArguments(const std::vector<std::string>& args) {
  std::string joined;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (i > 0) joined.push_back('\0');
    joined.append(args[i]);
  }
  return joined;
}

// Sort, dedupe, and join a set of records with the given separator. Used for
// every hash input that summarizes an unordered collection (source tree,
// compile options, canonical compilation database, compiler set).
std::string SortedUniqueJoined(std::vector<std::string> records, char sep) {
  std::sort(records.begin(), records.end());
  records.erase(std::unique(records.begin(), records.end()), records.end());
  std::string joined;
  for (std::size_t i = 0; i < records.size(); ++i) {
    if (i > 0) joined.push_back(sep);
    joined.append(records[i]);
  }
  return joined;
}

// -- per-TU construction -----------------------------------------------------

struct NormalizedCommand {
  TaggedPath source_tagged;
  TaggedPath working_directory_tagged;
  std::vector<std::string> arguments;
  std::string source_content_hash;
};

StatusOr<NormalizedCommand> NormalizeCommand(
    const tooling::CompileCommand& command,
    const fs::path& project_root) {
  if (command.Directory.empty()) {
    return Status::FailedPrecondition(
        "compile command has empty working directory for: " + command.Filename);
  }
  std::error_code wd_error;
  const auto canonical_working_dir =
      fs::weakly_canonical(fs::path(command.Directory), wd_error);
  if (wd_error) {
    return Status::FailedPrecondition(
        "cannot canonicalize compile command directory: " + command.Directory);
  }

  fs::path resolved_source = fs::path(command.Filename);
  if (!resolved_source.is_absolute()) {
    resolved_source = canonical_working_dir / resolved_source;
  }
  std::error_code canonical_error;
  resolved_source =
      fs::weakly_canonical(resolved_source, canonical_error);
  if (canonical_error) {
    return Status::FailedPrecondition(
        "cannot canonicalize translation-unit source: " + command.Filename);
  }

  std::error_code stat_error;
  if (!fs::is_regular_file(resolved_source, stat_error) || stat_error) {
    return Status::FailedPrecondition(
        "translation-unit source is missing: " + resolved_source.string());
  }

  auto contents = ReadFile(resolved_source);
  if (!contents.ok()) return contents.status();

  NormalizedCommand out;
  out.source_tagged = ClassifyPath(resolved_source, project_root);
  out.working_directory_tagged =
      ClassifyPath(canonical_working_dir, project_root);
  out.arguments =
      NormalizeArguments(command.CommandLine, project_root.generic_string());
  out.source_content_hash =
      DomainHash("veritas.source_file.v1", *contents);
  return out;
}

// -- top-level orchestration -------------------------------------------------

// Hash the sorted-unique set of (source_path, content_hash) pairs. Sources
// that appear in multiple compile_commands entries (multi-target builds
// compiling `a.cpp` with -DTARGET=A and -DTARGET=B) contribute exactly once,
// so a restructured project that compiles each source once produces the same
// source_tree_hash as one that compiles it under N variants.
std::string ComputeSourceTreeHash(
    const std::vector<NormalizedCommand>& normalized) {
  std::vector<std::string> lines;
  lines.reserve(normalized.size());
  for (const auto& command : normalized) {
    lines.push_back(JoinNul({
        command.source_tagged.relative_path.generic_string(),
        command.source_content_hash,
    }));
  }
  return DomainHash("veritas.source_tree.v1",
                    SortedUniqueJoined(std::move(lines), '\n'));
}

std::string ComputeCompileOptionsHash(
    const std::vector<NormalizedCommand>& normalized) {
  std::vector<std::string> args;
  for (const auto& command : normalized) {
    args.insert(args.end(), command.arguments.begin(),
                command.arguments.end());
  }
  return DomainHash("veritas.compile_options.v1",
                    SortedUniqueJoined(std::move(args), '\0'));
}

// Return the sorted-unique set of compiler basenames observed across every
// translation unit, joined by `,`. Order-independent (fixes the case where
// reordering entries between one clang++ TU and one gcc TU flipped
// `compiler_id`) and collapses to a single token when every TU uses the same
// compiler.
std::string DetectCompilerId(
    const std::vector<NormalizedCommand>& normalized) {
  std::vector<std::string> compilers;
  compilers.reserve(normalized.size());
  for (const auto& command : normalized) {
    if (!command.arguments.empty()) {
      compilers.push_back(command.arguments.front());
    }
  }
  return SortedUniqueJoined(std::move(compilers), ',');
}

// -- target identity ---------------------------------------------------------
//
// Every flag spelling that names a compilation target. Both the joined
// (`-target=arm64-…`) and the separated (`-target arm64-…`) form of each is
// accepted: a command that names a target in a spelling the loader did not
// recognize would silently fall back to the host triple and bind the build to
// a target it does not actually compile for.
//
// `--target` is listed before `-target` so the longer prefix matches first.
constexpr std::string_view kTargetFlags[] = {"--target", "-target", "-triple"};

// The target named by one normalized command, or "" when it names none.
// Searches from index 1: index 0 is the compiler basename, never a flag.
std::string DetectCommandTarget(const std::vector<std::string>& arguments) {
  for (std::size_t i = 1; i < arguments.size(); ++i) {
    const std::string_view argument = arguments[i];
    for (const std::string_view flag : kTargetFlags) {
      if (argument == flag) {
        return i + 1 < arguments.size() ? arguments[i + 1] : std::string();
      }
      if (argument.size() > flag.size() &&
          argument.compare(0, flag.size(), flag) == 0 &&
          argument[flag.size()] == '=') {
        return std::string(argument.substr(flag.size() + 1));
      }
    }
  }
  return std::string();
}

// The target triple the manifest records for the build variant.
//
// A target named on any command line wins, and the set is sorted-unique joined
// exactly like `compiler_id`, so reordering database entries cannot flip the
// value. A project that names no target anywhere is compiled for the analysis
// host — that is genuinely what the compilation targets — so the host's
// default triple is the honest answer rather than "". This is consequently
// host-derived, and deliberately so: a cross-host-stable value here would
// describe a target the build does not have.
std::string DetectTargetTriple(
    const std::vector<NormalizedCommand>& normalized) {
  std::vector<std::string> targets;
  targets.reserve(normalized.size());
  for (const auto& command : normalized) {
    std::string target = DetectCommandTarget(command.arguments);
    if (!target.empty()) targets.push_back(std::move(target));
  }
  if (targets.empty()) return llvm::sys::getDefaultTargetTriple();
  return SortedUniqueJoined(std::move(targets), ',');
}

}  // namespace

StatusOr<AnalysisManifest> LoadProjectManifest(const ProjectInput& input) {
  std::string error;
  auto database = tooling::JSONCompilationDatabase::loadFromFile(
      input.compile_database_path.string(), error,
      tooling::JSONCommandLineSyntax::AutoDetect);
  if (!database) {
    return Status::InvalidArgument(
        "invalid compile_commands.json (" +
        input.compile_database_path.string() + "): " + error);
  }
  const auto all_commands = database->getAllCompileCommands();
  if (all_commands.empty()) {
    return Status::FailedPrecondition(
        "compile_commands.json contains no entries: " +
        input.compile_database_path.string());
  }

  std::vector<NormalizedCommand> normalized;
  normalized.reserve(all_commands.size());
  for (const auto& command : all_commands) {
    auto result = NormalizeCommand(command, input.project_root);
    if (!result.ok()) return result.status();
    normalized.push_back(std::move(*result));
  }

  // Pre-compute per-TU command hashes and relative-path strings once. Every
  // downstream field (compilation-DB hash, TU id, sort key) needs them; a
  // second pass would either recompute or hoist them, and hoisting is the
  // cheaper option.
  struct PreparedTu {
    std::string relative_path;  // repository-relative, generic form
    std::string joined_args;    // NUL-joined arguments
    std::string command_hash;   // SHA-256 over joined_args
  };
  std::vector<PreparedTu> prepared;
  prepared.reserve(normalized.size());
  for (const auto& command : normalized) {
    PreparedTu p;
    p.relative_path = command.source_tagged.relative_path.generic_string();
    p.joined_args = JoinArguments(command.arguments);
    p.command_hash = DomainHash("veritas.command.v1", p.joined_args);
    prepared.push_back(std::move(p));
  }

  const auto source_tree_hash = ComputeSourceTreeHash(normalized);
  const auto compile_options_hash = ComputeCompileOptionsHash(normalized);
  const auto compiler_id = DetectCompilerId(normalized);

  // M1's half of the program identity. The M1 design spec assigns the initial
  // derivation here: `target_triple` comes from an explicit `--target` on the
  // command line when there is one and from the analysis host's default triple
  // otherwise, and `type_layout_hash` is a content address over the target and
  // compiler configuration the layout depends on. Neither enters
  // `build_variant_id` — adding them there would re-identify every stored row
  // in the tree.
  const auto target_triple = DetectTargetTriple(normalized);
  const auto type_layout_hash = TaggedIdentifier(
      "layout", "veritas.type_layout.v1", JoinNul({target_triple, compiler_id}));

  // The compilation-database hash summarizes the canonical set of entries,
  // not the raw JSON bytes on disk. Raw bytes would leak the checkout path
  // through the `directory` fields and produce different digests for the same
  // project laid out under two different roots. Hashing the normalized
  // (path, args) tuples keeps the identity stable.
  std::vector<std::string> database_lines;
  database_lines.reserve(prepared.size());
  for (const auto& p : prepared) {
    database_lines.push_back(JoinNul({p.relative_path, p.joined_args}));
  }
  const auto compilation_database_hash =
      DomainHash("veritas.compilation_database.v1",
                 SortedUniqueJoined(std::move(database_lines), '\n'));

  const auto build_variant_id = TaggedIdentifier(
      "bv", "veritas.build_variant.v1",
      JoinNul({compiler_id, compile_options_hash}));
  const auto repository_id = TaggedIdentifier(
      "repo", "veritas.repository.v1",
      JoinNul({source_tree_hash, compilation_database_hash}));
  const auto revision_id =
      TaggedIdentifier("rev", "veritas.revision.v1", source_tree_hash);

  AnalysisManifest manifest;
  auto& ctx = manifest.context;
  ctx.repository_id = repository_id;
  ctx.revision_id = revision_id;
  ctx.build_variant_id = build_variant_id;
  ctx.project_root = input.project_root;
  ctx.vcs_kind = "none";
  ctx.source_tree_hash = source_tree_hash;
  ctx.compilation_database_hash = compilation_database_hash;
  ctx.compiler_id = compiler_id;
  ctx.compile_options_hash = compile_options_hash;
  ctx.target_triple = target_triple;
  ctx.type_layout_hash = type_layout_hash;
  // `type_layout_hash` above is M1's provisional derivation: a content address
  // over the target and compiler configuration, which is what the M1 design
  // spec sanctions ("M1 may initially derive type_layout_hash from the target
  // and compiler configuration"). It is not frontend-derived layout data. M4
  // replaces it with that before publishing analysis results, and until it
  // does, no consumer should read it as an ABI-layout digest.
  //
  // vcs_revision, compiler_version, macro_set_hash, include_closure_hash, and
  // each TU's preprocessor_hash still default to "" on the struct: M4 has not
  // populated them. (This comment previously asserted the opposite — that M4
  // "populates them from frontend data" — which no code implemented, and that
  // false premise is why target_triple and type_layout_hash stayed empty for
  // ten milestones.)

  // Sort TU indices by source path so the manifest itself is deterministic.
  // The canonical serializer re-sorts by the same key, but keeping the
  // in-memory order stable lets callers index into `translation_units`
  // predictably (a.cpp before b.cpp, regardless of database entry order).
  std::vector<std::size_t> tu_order(prepared.size());
  std::iota(tu_order.begin(), tu_order.end(), std::size_t{0});
  std::sort(tu_order.begin(), tu_order.end(),
            [&](std::size_t a, std::size_t b) {
              return prepared[a].relative_path < prepared[b].relative_path;
            });

  manifest.translation_units.reserve(prepared.size());
  for (const auto index : tu_order) {
    const auto& command = normalized[index];
    const auto& p = prepared[index];
    TranslationUnitCommand tu;
    tu.revision_id = revision_id;
    tu.build_variant_id = build_variant_id;
    tu.source_path = command.source_tagged;
    tu.working_directory = command.working_directory_tagged;
    tu.arguments = command.arguments;
    tu.command_hash = p.command_hash;
    tu.translation_unit_id = TaggedIdentifier(
        "tu", "veritas.translation_unit.v1",
        JoinNul({revision_id, p.relative_path, p.command_hash}));
    manifest.translation_units.push_back(std::move(tu));
  }

  return manifest;
}

}  // namespace veritas::build
