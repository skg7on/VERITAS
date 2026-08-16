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

// Version.h — VERITAS version identity.
//
// GetVersion() returns the semantic version baked in at configure time
// (PROJECT_VERSION_{MAJOR,MINOR,PATCH}) plus the short git SHA of HEAD at
// the moment CMake configured the build. FormatVersion() renders the
// shared `--version` string used by every VERITAS CLI.

#ifndef VERITAS_CORE_VERSION_H_
#define VERITAS_CORE_VERSION_H_

#include <string>

namespace veritas {

struct Version {
  int major;
  int minor;
  int patch;
  std::string git_revision;
};

Version GetVersion();

// Renders "VERITAS <major>.<minor>.<patch> (<git-revision>)".
std::string FormatVersion(const Version& version);

}  // namespace veritas

#endif  // VERITAS_CORE_VERSION_H_
