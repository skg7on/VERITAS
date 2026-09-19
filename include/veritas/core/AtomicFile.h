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

#ifndef VERITAS_CORE_ATOMIC_FILE_H_
#define VERITAS_CORE_ATOMIC_FILE_H_

#include <string>

#include "veritas/core/Status.h"

namespace veritas::core {

// Replaces `destination` with `bytes` through a sibling temporary, leaving an
// existing destination untouched when writing or replacement fails.
Status WriteFileAtomically(const std::string& destination,
                           const std::string& bytes);

}  // namespace veritas::core

#endif  // VERITAS_CORE_ATOMIC_FILE_H_
