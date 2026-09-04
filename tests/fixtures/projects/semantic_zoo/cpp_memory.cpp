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

#include "semantic_zoo.h"

extern "C" void* malloc(unsigned long size);
extern "C" void free(void* pointer);
extern "C" void* memcpy(void* destination, const void* source,
                        unsigned long length);

namespace {

ZooBuffer* ReusedAllocation() {
  return static_cast<ZooBuffer*>(malloc(sizeof(ZooBuffer)));
}

}  // namespace

extern "C" void zoo_modeled_copy(ZooBuffer* destination,
                                  const ZooEnvelope* source) {
  ZooBuffer* first_site = static_cast<ZooBuffer*>(malloc(sizeof(ZooBuffer)));
  ZooBuffer* reused_first = ReusedAllocation();
  ZooBuffer* reused_second = ReusedAllocation();

  if (first_site != nullptr) {
    first_site->data[0] = source->payload.bytes[0];
    first_site->length = source->payload.length;
    memcpy(first_site->data, source->payload.bytes, 16);
  }
  if (reused_first != nullptr) {
    reused_first->data[1] = source->payload.bytes[1];
    reused_first->capacity = 32;
  }
  if (reused_second != nullptr) {
    reused_second->data[2] = source->payload.bytes[2];
    reused_second->length = source->payload.length;
  }

  memcpy(destination->data, source->payload.bytes, 16);
  destination->length = source->payload.length;
  free(reused_second);
  free(reused_first);
  free(first_site);
}
