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

// The copy is guarded, so a 33-byte request does not overflow. The replay
// instead exceeds its deadline; that outcome never decides the obligation.
int decode(const char *payload, unsigned long len) {
  char local[kDestinationCapacity];
  if (len <= sizeof(local))
    memcpy(local, payload, len);
  for (volatile unsigned long spin = 0;; ++spin) {
    (void)spin;
  }
}
