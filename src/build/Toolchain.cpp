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

#include "veritas/build/Toolchain.h"

#include <cstdlib>
#include <optional>
#include <string>

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

namespace veritas::build {
namespace {

using CommandLineArguments = clang::tooling::CommandLineArguments;

// Directory holding the clang builtin headers (stddef.h, stdarg.h, ...). Baked
// in at configure time from LLVM_LIBRARY_DIR. ClangTool normally infers this
// from the running executable's path, which is wrong for VERITAS because it
// links a prebuilt LLVM whose builtin headers live under the LLVM library
// directory. Empty when the resource dir could not be located, in which case
// ClangTool's own inference is left in place.
constexpr const char* kClangResourceDir =
#ifdef VERITAS_CLANG_RESOURCE_DIR
    VERITAS_CLANG_RESOURCE_DIR;
#else
    "";
#endif

// Standard macOS SDK install locations, used as a last-resort fallback when
// neither `SDKROOT` nor `xcrun` is available.
constexpr const char* kWellKnownMacOSSdkPaths[] = {
    "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk",
    "/Applications/Xcode.app/Contents/Developer/Platforms/"
        "MacOSX.platform/Developer/SDKs/MacOSX.sdk",
    "/Applications/Xcode-beta.app/Contents/Developer/Platforms/"
        "MacOSX.platform/Developer/SDKs/MacOSX.sdk",
};

bool IsDirectory(llvm::StringRef path) {
  return llvm::sys::fs::is_directory(path);
}

// Runs `xcrun --show-sdk-path` and returns its trimmed stdout when it names an
// existing directory. `xcrun` is the mechanism the host driver itself uses to
// locate the active SDK; consulting it keeps VERITAS aligned with whatever
// Xcode / CommandLineTools selection the user has made.
std::optional<std::string> XcrunShowSdkPath() {
  auto xcrun = llvm::sys::findProgramByName("xcrun");
  if (!xcrun) {
    return std::nullopt;
  }

  llvm::SmallString<128> capture_path;
  if (llvm::sys::fs::createTemporaryFile("veritas-sdk", "txt", capture_path)) {
    return std::nullopt;
  }

  const llvm::StringRef args[] = {"xcrun", "--show-sdk-path"};
  const std::optional<llvm::StringRef> redirects[3] = {
      std::nullopt, llvm::StringRef(capture_path), std::nullopt};
  const int result = llvm::sys::ExecuteAndWait(
      *xcrun, llvm::ArrayRef<llvm::StringRef>(args), /*Env=*/std::nullopt,
      llvm::ArrayRef<std::optional<llvm::StringRef>>(redirects));

  std::optional<std::string> sdk;
  if (result == 0) {
    auto buffer = llvm::MemoryBuffer::getFile(capture_path);
    if (buffer) {
      std::string path = llvm::StringRef((*buffer)->getBuffer()).trim().str();
      if (!path.empty() && IsDirectory(path)) {
        sdk = std::move(path);
      }
    }
  }

  std::error_code remove_error = llvm::sys::fs::remove(capture_path);
  (void)remove_error;  // best-effort cleanup of a file in the system temp dir
  return sdk;
}

std::optional<std::string> ResolveMacOSSysroot() {
  llvm::Triple host_triple(llvm::sys::getDefaultTargetTriple());
  if (!host_triple.isOSDarwin()) {
    return std::nullopt;
  }

  if (const char* sdkroot = std::getenv("SDKROOT");
      sdkroot != nullptr && *sdkroot != '\0') {
    if (IsDirectory(sdkroot)) {
      return std::string(sdkroot);
    }
  }

  if (auto sdk = XcrunShowSdkPath()) {
    return sdk;
  }

  for (const char* candidate : kWellKnownMacOSSdkPaths) {
    if (IsDirectory(candidate)) {
      return std::string(candidate);
    }
  }
  return std::nullopt;
}

bool HasSysrootFlag(const CommandLineArguments& args) {
  for (const std::string& arg : args) {
    if (arg == "-isysroot" || arg.starts_with("-isysroot=") ||
        arg == "--sysroot" || arg.starts_with("--sysroot=")) {
      return true;
    }
  }
  return false;
}

bool HasFlagPrefix(const CommandLineArguments& args, llvm::StringRef prefix) {
  for (const std::string& arg : args) {
    if (llvm::StringRef(arg).starts_with(prefix)) {
      return true;
    }
  }
  return false;
}

}  // namespace

clang::tooling::ArgumentsAdjuster MakeSystemIncludeAdjuster() {
  // The SDK root and clang resource dir are stable for the lifetime of the
  // process; resolve the sysroot once and capture the baked-in resource dir.
  static const std::optional<std::string> kSysroot = ResolveMacOSSysroot();

  const std::string resource_dir =
      kClangResourceDir[0] != '\0' ? std::string(kClangResourceDir)
                                   : std::string{};
  const std::optional<std::string> sysroot = kSysroot;

  return [resource_dir, sysroot](const CommandLineArguments& args,
                                 llvm::StringRef) {
    std::vector<std::string> extra;
    if (!resource_dir.empty() && !HasFlagPrefix(args, "-resource-dir")) {
      extra.push_back("-resource-dir=" + resource_dir);
    }
    if (sysroot.has_value() && !HasSysrootFlag(args)) {
      extra.push_back("-isysroot");
      extra.push_back(*sysroot);
    }
    if (extra.empty()) {
      return args;
    }

    CommandLineArguments adjusted = args;
    auto position = adjusted.begin();
    if (position != adjusted.end()) {
      ++position;  // keep the program name (argv[0]) first
    }
    adjusted.insert(position, extra.begin(), extra.end());
    return adjusted;
  };
}

}  // namespace veritas::build
