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

// Fixture for system-header resolution. The translation unit pulls in both a
// clang builtin header (<stdint.h>, which lives in the clang resource
// directory) and a POSIX SDK header (<sys/types.h>, which on macOS lives under
// the SDK sysroot). VERITAS links a Clang built without xcselect support, so
// analysis must inject both `-resource-dir` and `-isysroot` for these headers
// to resolve.

#include <stdint.h>
#include <sys/types.h>

int64_t system_header_function(ssize_t value) { return value + 1; }
