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

// The safe shape: the length <= sizeof(destination) check dominates the sink on
// every path that reaches it.
void copy_payload(Packet* p, Buffer* b) {
  if (p->length <= sizeof(b->data)) {
    memcpy(b->data, p->payload, p->length);
  }
}
