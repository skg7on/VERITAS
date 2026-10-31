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

#include "packet.h"

extern "C" void *memcpy(void *, const void *, unsigned long);

// The admitted (guarded) copy is skipped for a 33-byte request; the overflow
// happens at a different, unguarded callsite. A sanitizer crash there must not
// verify the admitted obligation.
int decode(const char *payload, unsigned long len) {
  char destination[kDestinationCapacity];
  if (len <= sizeof(destination))
    memcpy(destination, payload, len);
  char other[16];
  memcpy(other, payload, len);
  return 0;
}
