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

extern int zoo_external_state;

int zoo_memory_global;
static int zoo_memory_static;

void zoo_memory_shapes(ZooBuffer* left, ZooBuffer* right,
                       ZooEnvelope* envelope, int** slot, int index) {
  ZooBuffer* copied = left;
  ZooBuffer first_stack = {{0}, 32, 0};
  ZooBuffer second_stack = {{0}, 32, 0};
  unsigned char constant_array[4] = {1, 2, 3, 4};
  unsigned char variable_array[8] = {0};
  union ZooOverlap {
    int whole;
    unsigned char bytes[sizeof(int)];
  } overlap;
  int local_value = index;

  zoo_memory_global = index;
  zoo_memory_static = zoo_memory_global + 1;
  copied->data[0] = (unsigned char)zoo_memory_static;
  left->data[1] = right->data[2];
  first_stack.data[0] = constant_array[1];
  second_stack.data[0] = first_stack.data[0];
  envelope->payload.bytes[0] = second_stack.data[0];
  envelope->payload.length = (ZooSize)(index + 1);
  variable_array[(unsigned int)index & 7U] = envelope->payload.bytes[0];
  right->data[(unsigned int)index & 31U] = variable_array[0];
  *slot = &local_value;
  **slot = zoo_memory_static;
  overlap.whole = **slot;
  zoo_external_state = overlap.bytes[0];
}
