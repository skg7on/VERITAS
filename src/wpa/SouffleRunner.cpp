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

// SouffleRunner.cpp — the in-process compiled-Souffle entry point.
//
// This is the only translation unit that instantiates the Souffle programs and
// therefore the only one that must be compiled with RTTI and exceptions. It is
// linked into a shared library and reached through the C ABI in
// SouffleRunner.h, so no Souffle type or exception crosses into VERITAS.

#include "veritas/wpa/SouffleRunner.h"

#include <string_view>

#include "souffle/SouffleInterface.h"

namespace {

// Maps a component name to the registered program name. These are the output
// filenames the build generates the bundles from (v2_reach.cpp /
// v2_maywrite.cpp), sanitized by Souffle's synthesiser.
const char* ProgramNameForComponent(std::string_view component) {
  if (component == "reachability") return "v2_reach";
  if (component == "memory-effects") return "v2_maywrite";
  return nullptr;
}

}  // namespace

int veritas_souffle_run(const char* component, const char* input_dir,
                        const char* output_dir, unsigned jobs) {
  const char* program_name = ProgramNameForComponent(component);
  if (program_name == nullptr) {
    return 2;
  }

  souffle::SouffleProgram* program =
      souffle::ProgramFactory::newInstance(program_name);
  if (program == nullptr) {
    return 3;
  }

  program->setNumThreads(jobs);
  try {
    program->runAll(input_dir, output_dir, /*performIO=*/true,
                    /*pruneImdtRels=*/true);
  } catch (const std::exception&) {
    delete program;
    return 1;
  }
  delete program;
  return 0;
}
