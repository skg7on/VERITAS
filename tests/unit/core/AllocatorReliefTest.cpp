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

#include "veritas/core/AllocatorRelief.h"

#include <cstddef>
#include <gtest/gtest.h>
#include <vector>

using namespace veritas::core;

// ReleaseFreedMemory is a safety property, not an effect property: the effect
// it is meant to have (returning emptied arenas to the OS) is only observable
// as a resident-set step in a process that has actually built a large working
// set, so it is measured on the analyzer run rather than asserted here. What
// this test pins is that the call is valid at any time, on any heap state, and
// that the large-free path below does not leave it in a state where it aborts.
TEST(AllocatorReliefTest, ReleaseIsSafeWithNothingToReleaseAndAfterAllocation) {
  ReleaseFreedMemory();  // must not crash or abort on a clean heap
  {
    std::vector<std::byte> big(64u * 1024u * 1024u);
    (void)big[0];
  }
  ReleaseFreedMemory();  // must not crash after a large free
  SUCCEED();
}
