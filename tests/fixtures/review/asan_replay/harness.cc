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

// Trusted operator-registered harness. It supplies the captured entry point
// with exactly the declared concrete bytes and length. The extents are
// re-checked by the replay compiler, so a target-layout mismatch cannot be
// mistaken for the analyzed program.
#include <cstddef>
#include <cstdio>
#include <cstdlib>

#include "packet.h"

#ifndef VERITAS_EXPECTED_CAPACITY
#error "replay capacity extent is required"
#endif
#ifndef VERITAS_EXPECTED_READABLE
#error "replay readable extent is required"
#endif

inline constexpr unsigned long kHarnessSourceCapacity = 64;

static_assert(kDestinationCapacity == VERITAS_EXPECTED_CAPACITY,
              "replay destination extent differs from the analyzed extent");
static_assert(kHarnessSourceCapacity >= VERITAS_EXPECTED_READABLE,
              "harness source is shorter than the declared readable extent");

int main(int argc, char **argv) {
  if (argc != 3)
    return 3;
  char *end = nullptr;
  unsigned long requested = std::strtoul(argv[2], &end, 10);
  if (!end || *end)
    return 3;
  std::FILE *file = std::fopen(argv[1], "rb");
  if (!file)
    return 3;
  unsigned char storage[kHarnessSourceCapacity];
  std::size_t got = std::fread(storage, 1, sizeof storage, file);
  std::fclose(file);
  if (requested > got)
    return 3;
  // A non-zero entry-point result is a rejection control, not a finding.
  return decode(reinterpret_cast<const char *>(storage), requested) == 0 ? 0 : 2;
}
