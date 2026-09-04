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

static void zoo_callback_left(ZooBuffer* buffer, int value) {
  buffer->data[0] = (unsigned char)value;
  buffer->length = 1;
}

static void zoo_callback_right(ZooBuffer* buffer, int value) {
  buffer->data[1] = (unsigned char)(value + 1);
  buffer->length = 2;
}

ZooCallback zoo_callback_global = zoo_callback_left;
ZooCallback zoo_callback_table[2] = {zoo_callback_left, zoo_callback_right};

void zoo_callback_direct(ZooBuffer* buffer, int value) {
  zoo_callback_left(buffer, value);
}

void zoo_callback_indirect(ZooBuffer* buffer, int value) {
  zoo_callback_global(buffer, value);
}

void zoo_callback_parameter(ZooCallback callback, ZooBuffer* buffer,
                            int value) {
  callback(buffer, value);
}

void zoo_callback_select(int choose, ZooBuffer* buffer, int value) {
  zoo_callback_table[choose != 0](buffer, value);
}
