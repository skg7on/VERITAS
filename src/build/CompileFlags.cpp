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

#include "veritas/build/CompileFlags.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "clang/Options/Options.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"

namespace veritas::build {

namespace {

std::string NormalizePathSeparators(std::string path) {
  for (char& character : path) {
    if (character == '\\') {
      character = '/';
    }
  }
  return path;
}

std::string LexicallyNormalizePath(std::string path) {
  return std::filesystem::path(NormalizePathSeparators(std::move(path)))
      .lexically_normal()
      .generic_string();
}

bool IsAbsoluteComparisonPath(std::string_view path) {
  return (!path.empty() && path.front() == '/') ||
         (path.size() >= 3 && path[1] == ':' && path[2] == '/');
}

std::filesystem::path ResolveTaggedPath(
    const TaggedPath& path, const std::filesystem::path& project_root) {
  if (path.root_kind == PathRootKind::kRepository) {
    return project_root / path.relative_path;
  }
  return path.relative_path;
}

bool IsTranslationUnitInput(std::string_view input,
                            const std::filesystem::path& project_root,
                            const std::string& normalized_source_path,
                            const std::string& normalized_working_directory) {
  std::string candidate = LexicallyNormalizePath(
      ResolveArguments(std::string(input), project_root.generic_string()));
  if (!IsAbsoluteComparisonPath(candidate)) {
    candidate = LexicallyNormalizePath(
        (std::filesystem::path(normalized_working_directory) / candidate)
            .generic_string());
  }
  return candidate == normalized_source_path;
}

}  // namespace

std::string ResolveArguments(const std::string& argument,
                             const std::string& project_root) {
  std::string out = argument;
  std::size_t pos = 0;
  while ((pos = out.find("<repo>", pos)) != std::string::npos) {
    out.replace(pos, 6, project_root);
    pos += project_root.size();
  }
  return out;
}

std::vector<std::string> CompileFlags(
    const TranslationUnitCommand& command,
    const std::filesystem::path& project_root) {
  const std::string source_path = LexicallyNormalizePath(
      ResolveTaggedPath(command.source_path, project_root).generic_string());
  const std::string working_directory = LexicallyNormalizePath(
      ResolveTaggedPath(command.working_directory, project_root)
          .generic_string());
  std::vector<const char*> argv;
  argv.reserve(command.arguments.size());
  for (const auto& argument : command.arguments) {
    argv.push_back(argument.c_str());
  }
  llvm::opt::InputArgList args(argv.data(), argv.data() + argv.size());
  const llvm::opt::OptTable& options = clang::getDriverOptTable();
  std::vector<std::string> flags;
  flags.reserve(command.arguments.size());
  for (unsigned position = 1; position < command.arguments.size();) {
    const unsigned first_position = position;
    std::unique_ptr<llvm::opt::Arg> argument(options.ParseOneArg(
        args, position, llvm::opt::Visibility(clang::options::ClangOption)));
    if (argument == nullptr) {
      const unsigned parsed_end = std::min(
          position, static_cast<unsigned>(command.arguments.size()));
      const unsigned preserved_end =
          std::max(first_position + 1, parsed_end);
      for (unsigned i = first_position; i < preserved_end; ++i) {
        flags.push_back(ResolveArguments(command.arguments[i],
                                         project_root.generic_string()));
      }
      position = preserved_end;
      continue;
    }
    const llvm::opt::Option& option = argument->getOption();
    if (option.matches(clang::options::OPT_c) ||
        option.matches(clang::options::OPT_o)) {
      continue;
    }
    if (option.matches(clang::options::OPT_INPUT) &&
        IsTranslationUnitInput(argument->getValue(), project_root, source_path,
                               working_directory)) {
      continue;
    }
    for (unsigned i = first_position; i < position; ++i) {
      flags.push_back(ResolveArguments(command.arguments[i],
                                       project_root.generic_string()));
    }
  }
  return flags;
}

}  // namespace veritas::build
