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

extern "C" void* memcpy(void* destination, const void* source,
                        unsigned long length);

// The check exists only on a sibling branch; it does not dominate the sink,
// which is reachable on the unchecked path.
void copy_payload(Packet* p, Buffer* b, int verbose) {
  if (verbose) {
    if (p->length > sizeof(b->data)) {
      return;
    }
  }
  memcpy(b->data, p->payload, p->length);
}
