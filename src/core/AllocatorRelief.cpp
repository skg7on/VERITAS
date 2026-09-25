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

// <cstdlib> is included for its side effect before the platform test below:
// on glibc it is what pulls in <features.h>, which is what defines __GLIBC__.
// Without it the __GLIBC__ branch would be skipped on a Linux that does have
// malloc_trim, and the helper would silently do nothing there.
#include <cstdlib>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

namespace veritas::core {

void ReleaseFreedMemory() {
#if defined(__APPLE__)
  // Darwin: the null zone means "every zone" and the zero byte count means
  // "as much as each can spare". The return value is the number of bytes
  // released; it is deliberately discarded, because the contract here is
  // advisory relief on a best-effort basis, not a reported quantity.
  (void)malloc_zone_pressure_relief(nullptr, 0);
#elif defined(__GLIBC__)
  // glibc: malloc_trim(0) walks the arenas and returns the free space it can
  // release at the top of the heap. Its int result is a success flag, not a
  // byte count, and is likewise discarded.
  (void)malloc_trim(0);
#else
  // No facility is known to return emptied arenas on this platform. Doing
  // nothing is the correct behaviour: the contract is best-effort relief, and
  // a platform without the facility is not a failure.
#endif
}

}  // namespace veritas::core
