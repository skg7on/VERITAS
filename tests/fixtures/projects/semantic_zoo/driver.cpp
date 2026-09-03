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

extern "C" {
int zoo_external_state = 0;
}

extern "C" void zoo_driver(int choose, ZooBuffer* buffer,
                           const ZooEnvelope* envelope) {
  ZooEnvelope mutable_envelope = *envelope;
  int local_slot = 0;
  int* slot = &local_slot;

  zoo_memory_shapes(buffer, buffer, &mutable_envelope, &slot, choose);
  zoo_callback_direct(buffer, choose);
  zoo_callback_indirect(buffer, choose);
  zoo_callback_parameter(zoo_callback_direct, buffer, choose);
  zoo_callback_select(choose, buffer, choose);
  zoo_modeled_copy(buffer, &mutable_envelope);
  zoo_dispatch_direct(buffer);
  zoo_virtual_one(buffer);
  zoo_virtual_select(choose, buffer);
  zoo_recursive_entry(choose, buffer);
}
