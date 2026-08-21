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

#ifndef VERITAS_FACTS_SOUFFLE_RUNNER_H_
#define VERITAS_FACTS_SOUFFLE_RUNNER_H_

#include <filesystem>

#include "veritas/core/Status.h"

namespace veritas::facts {

class SouffleRunner {
public:
  // Returns InvalidArgument when an input path has the wrong shape. Returns
  // Internal when filesystem setup or the child process fails.
  static Status Run(const std::filesystem::path &executable,
                    const std::filesystem::path &rule_file,
                    const std::filesystem::path &input_directory,
                    const std::filesystem::path &output_directory);
};

} // namespace veritas::facts

#endif // VERITAS_FACTS_SOUFFLE_RUNNER_H_
