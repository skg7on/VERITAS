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

// Toolchain.h — runtime toolchain adjustments for ClangTool invocations.
//
// VERITAS reproduces AST and LLVM IR from external `compile_commands.json`
// files. Those databases record the command line the *driver* was invoked
// with; on macOS that command line almost never carries an explicit
// `-isysroot`, because the driver discovers the SDK automatically. VERITAS
// links a Clang built with `CLANG_USE_XCSELECT=OFF`, so that automatic
// discovery does not happen and the C++ standard library and POSIX system
// headers become unresolvable. The adjuster below restores the sysroot
// explicitly so the tool sees the same header environment the driver would.

#ifndef VERITAS_BUILD_TOOLCHAIN_H_
#define VERITAS_BUILD_TOOLCHAIN_H_

#include "clang/Tooling/ArgumentsAdjusters.h"

namespace veritas::build {

// Returns a ClangTool arguments adjuster that injects the platform system
// include root. On macOS this adds `-isysroot <SDK>` (resolved from the
// `SDKROOT` environment variable, `xcrun --show-sdk-path`, or a well-known
// SDK location, in that order) so system headers resolve without xcselect
// support. Commands that already specify `-isysroot` or `--sysroot` are left
// untouched. On other platforms the returned adjuster is a no-op.
clang::tooling::ArgumentsAdjuster MakeSystemIncludeAdjuster();

}  // namespace veritas::build

#endif  // VERITAS_BUILD_TOOLCHAIN_H_
