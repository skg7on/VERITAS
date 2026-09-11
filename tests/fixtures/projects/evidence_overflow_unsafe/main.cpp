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

// The in-process Clang invocation carries no system include paths, so the
// memcpy declaration is written out rather than pulled from <cstring>.
extern "C" void* memcpy(void* destination, const void* source,
                        unsigned long length);

// The unsafe shape: packet.length flows straight to the memcpy size with no
// bounds check. A uint16 length admits [0, 65535], exceeding the 2048-byte
// destination on the upper half of its range.
void copy_payload(Packet* p, Buffer* b) {
  memcpy(b->data, p->payload, p->length);
}
