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

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>

#include <sys/resource.h>

int main(int argc, char** argv) {
  std::filesystem::path output_dir;
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string_view(argv[i]) == "-D") {
      output_dir = argv[i + 1];
      break;
    }
  }
  if (output_dir.empty()) {
    return 2;
  }

  rlimit address_space{};
  if (::getrlimit(RLIMIT_AS, &address_space) != 0 ||
      address_space.rlim_cur !=
          static_cast<rlim_t>(VERITAS_EXPECTED_MEMORY_BYTES)) {
    return 3;
  }

  std::ofstream result(output_dir / "ReachableCall.csv");
  std::ofstream witness(output_dir / "Witness.csv");
  if (!result || !witness) {
    return 4;
  }
  return 0;
}
