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
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/JSONCompilationDatabase.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/SHA256.h"

namespace veritas::build {

namespace {

namespace fs = std::filesystem;
namespace tooling = clang::tooling;

// -- domain-separated hashing ------------------------------------------------

std::string HexDigest(llvm::ArrayRef<uint8_t> digest) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(digest.size() * 2);
  for (const auto byte : digest) {
    out.push_back(kHex[(byte >> 4) & 0xf]);
    out.push_back(kHex[byte & 0xf]);
  }
  return out;
}

std::string DomainHash(std::string_view domain, std::string_view bytes) {
  llvm::SHA256 hash;
  hash.update(llvm::ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t*>(domain.data()), domain.size()));
  static constexpr uint8_t separator = 0;
  hash.update(llvm::ArrayRef<uint8_t>(&separator, 1));
  hash.update(llvm::ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
  const auto digest = hash.final();
  return HexDigest(digest);
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

std::string JoinArguments(const std::vector<std::string>& args) {
  std::string joined;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (i > 0) joined.push_back('\0');
    joined.append(args[i]);
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

// Hash the sorted set of (source_path, content_hash) pairs. Sources that
// appear in multiple compile_commands entries (multi-target builds compiling
// `a.cpp` with -DTARGET=A and -DTARGET=B) contribute exactly once, so a
// restructured project that compiles each source once produces the same
// source_tree_hash as one that compiles it under N variants.
std::string ComputeSourceTreeHash(
    const std::vector<NormalizedCommand>& normalized) {
  std::vector<std::string> lines;
  lines.reserve(normalized.size());
  for (const auto& command : normalized) {
    std::string line;
    line.append(command.source_tagged.relative_path.generic_string());
    line.push_back('\0');
    line.append(command.source_content_hash);
    lines.push_back(std::move(line));
  }
  std::sort(lines.begin(), lines.end());
  lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
  std::string joined;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i > 0) joined.push_back('\n');
    joined.append(lines[i]);
  }
  return DomainHash("veritas.source_tree.v1", joined);
}

std::string ComputeCompileOptionsHash(
    const std::vector<NormalizedCommand>& normalized) {
  std::vector<std::string> unique_args;
  for (const auto& command : normalized) {
    for (const auto& arg : command.arguments) {
      unique_args.push_back(arg);
    }
  }
  std::sort(unique_args.begin(), unique_args.end());
  unique_args.erase(std::unique(unique_args.begin(), unique_args.end()),
                    unique_args.end());
  std::string joined;
  for (std::size_t i = 0; i < unique_args.size(); ++i) {
    if (i > 0) joined.push_back('\0');
    joined.append(unique_args[i]);
  }
  return DomainHash("veritas.compile_options.v1", joined);
}

// Return the sorted-unique set of compiler basenames observed across every
// translation unit, joined by `,`. This is order-independent (fixes the case
// where reordering entries between one clang++ TU and one gcc TU flipped
// `compiler_id`) and still collapses to a single token for the common case
// where every TU uses the same compiler.
std::string DetectCompilerId(
    const std::vector<NormalizedCommand>& normalized) {
  std::vector<std::string> compilers;
  compilers.reserve(normalized.size());
  for (const auto& command : normalized) {
    if (!command.arguments.empty()) {
      compilers.push_back(command.arguments.front());
    }
  }
  std::sort(compilers.begin(), compilers.end());
  compilers.erase(std::unique(compilers.begin(), compilers.end()),
                  compilers.end());
  std::string joined;
  for (std::size_t i = 0; i < compilers.size(); ++i) {
    if (i > 0) joined.push_back(',');
    joined.append(compilers[i]);
  }
  return joined;
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

  const auto source_tree_hash = ComputeSourceTreeHash(normalized);
  const auto compile_options_hash = ComputeCompileOptionsHash(normalized);
  const auto compiler_id = DetectCompilerId(normalized);

  // The compilation-database hash summarizes the *canonical* set of entries,
  // not the raw JSON bytes on disk. Raw bytes would leak the checkout path
  // through the `directory` fields and produce different digests for the
  // same project laid out under two different roots. Hashing the normalized
  // (path, args) tuples keeps the identity stable.
  std::vector<std::string> database_lines;
  database_lines.reserve(normalized.size());
  for (const auto& command : normalized) {
    std::string line;
    line.append(command.source_tagged.relative_path.generic_string());
    line.push_back('\0');
    line.append(JoinArguments(command.arguments));
    database_lines.push_back(std::move(line));
  }
  std::sort(database_lines.begin(), database_lines.end());
  std::string database_joined;
  for (std::size_t i = 0; i < database_lines.size(); ++i) {
    if (i > 0) database_joined.push_back('\n');
    database_joined.append(database_lines[i]);
  }
  const auto compilation_database_hash =
      DomainHash("veritas.compilation_database.v1", database_joined);

  std::string variant_input;
  variant_input.append(compiler_id);
  variant_input.push_back('\0');
  variant_input.append(compile_options_hash);
  const auto build_variant_id =
      TaggedIdentifier("bv", "veritas.build_variant.v1", variant_input);

  std::string repository_input;
  repository_input.append(source_tree_hash);
  repository_input.push_back('\0');
  repository_input.append(compilation_database_hash);
  const auto repository_id =
      TaggedIdentifier("repo", "veritas.repository.v1", repository_input);
  const auto revision_id =
      TaggedIdentifier("rev", "veritas.revision.v1", source_tree_hash);

  AnalysisManifest manifest;
  auto& ctx = manifest.context;
  ctx.repository_id = repository_id;
  ctx.revision_id = revision_id;
  ctx.build_variant_id = build_variant_id;
  ctx.project_root = input.project_root;
  ctx.vcs_kind = "none";
  ctx.vcs_revision = "";
  ctx.source_tree_hash = source_tree_hash;
  ctx.compilation_database_hash = compilation_database_hash;
  ctx.target_triple = "";
  ctx.compiler_id = compiler_id;
  ctx.compiler_version = "";
  ctx.compile_options_hash = compile_options_hash;
  ctx.macro_set_hash = "";
  ctx.include_closure_hash = "";
  ctx.type_layout_hash = "";

  // Sort translation units so the manifest itself is deterministic. The
  // canonical serializer already re-sorts by the same key, but keeping the
  // in-memory order stable lets callers index into `translation_units`
  // predictably (a.cpp before b.cpp, regardless of database entry order).
  std::sort(normalized.begin(), normalized.end(),
            [](const NormalizedCommand& a, const NormalizedCommand& b) {
              return a.source_tagged.relative_path.generic_string() <
                     b.source_tagged.relative_path.generic_string();
            });

  manifest.translation_units.reserve(normalized.size());
  for (const auto& command : normalized) {
    TranslationUnitCommand tu;
    tu.revision_id = revision_id;
    tu.build_variant_id = build_variant_id;
    tu.source_path = command.source_tagged;
    tu.working_directory = command.working_directory_tagged;
    tu.arguments = command.arguments;
    const auto joined = JoinArguments(command.arguments);
    tu.command_hash = DomainHash("veritas.command.v1", joined);
    tu.preprocessor_hash = "";
    std::string tu_input;
    tu_input.append(revision_id);
    tu_input.push_back('\0');
    tu_input.append(command.source_tagged.relative_path.generic_string());
    tu_input.push_back('\0');
    tu_input.append(tu.command_hash);
    tu.translation_unit_id =
        TaggedIdentifier("tu", "veritas.translation_unit.v1", tu_input);
    manifest.translation_units.push_back(std::move(tu));
  }

  return manifest;
}

}  // namespace veritas::build
