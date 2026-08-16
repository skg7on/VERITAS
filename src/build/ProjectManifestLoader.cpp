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
// Compiler basenames go into `compiler_id`; the argv[0] the caller keeps is
// the basename too so the canonical bytes stay stable across checkout paths.
// Repository-relative arguments are rewritten to `<repo>/...` so the same
// project produces the same command hash regardless of where it lives.

std::string BasenameOf(std::string_view program) {
  const auto slash = program.find_last_of('/');
  if (slash == std::string_view::npos) return std::string(program);
  return std::string(program.substr(slash + 1));
}

std::string NormalizeArgument(const std::string& argument,
                              const fs::path& working_dir,
                              const fs::path& project_root) {
  if (argument.empty()) return argument;
  fs::path maybe_path(argument);
  std::error_code error;
  fs::path resolved = maybe_path.is_absolute()
                          ? maybe_path
                          : working_dir / maybe_path;
  resolved = fs::weakly_canonical(resolved, error);
  if (!error && fs::exists(resolved, error) &&
      IsWithin(resolved, project_root)) {
    return "<repo>/" + fs::relative(resolved, project_root)
                           .lexically_normal()
                           .generic_string();
  }
  return argument;
}

std::vector<std::string> NormalizeArguments(
    const std::vector<std::string>& raw,
    const fs::path& working_dir,
    const fs::path& project_root) {
  std::vector<std::string> normalized;
  normalized.reserve(raw.size());
  if (raw.empty()) return normalized;
  normalized.push_back(BasenameOf(raw.front()));
  for (std::size_t i = 1; i < raw.size(); ++i) {
    normalized.push_back(NormalizeArgument(raw[i], working_dir, project_root));
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
  fs::path source_absolute;
  TaggedPath source_tagged;
  TaggedPath working_directory_tagged;
  std::vector<std::string> arguments;
  std::string source_content_hash;
};

StatusOr<NormalizedCommand> NormalizeCommand(
    const tooling::CompileCommand& command,
    const fs::path& project_root) {
  const fs::path working_dir(command.Directory);
  fs::path resolved_source = fs::path(command.Filename);
  if (!resolved_source.is_absolute()) {
    resolved_source = working_dir / resolved_source;
  }
  std::error_code canonical_error;
  resolved_source =
      fs::weakly_canonical(resolved_source, canonical_error);
  if (canonical_error) {
    return Status::FailedPrecondition(
        "cannot canonicalize translation-unit source: " + command.Filename);
  }
  if (!fs::is_regular_file(resolved_source)) {
    return Status::FailedPrecondition(
        "translation-unit source is missing: " + resolved_source.string());
  }

  auto contents = ReadFile(resolved_source);
  if (!contents.ok()) return contents.status();

  std::error_code wd_error;
  const auto canonical_working_dir =
      fs::weakly_canonical(working_dir, wd_error);
  if (wd_error) {
    return Status::FailedPrecondition(
        "cannot canonicalize compile command directory: " + working_dir.string());
  }

  NormalizedCommand out;
  out.source_absolute = resolved_source;
  out.source_tagged = ClassifyPath(resolved_source, project_root);
  out.working_directory_tagged =
      ClassifyPath(canonical_working_dir, project_root);
  out.arguments = NormalizeArguments(command.CommandLine, canonical_working_dir,
                                     project_root);
  out.source_content_hash =
      DomainHash("veritas.source_file.v1", *contents);
  return out;
}

// -- top-level orchestration -------------------------------------------------

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

std::string DetectCompilerId(
    const std::vector<NormalizedCommand>& normalized) {
  for (const auto& command : normalized) {
    if (!command.arguments.empty()) return command.arguments.front();
  }
  return {};
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
