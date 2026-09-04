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

namespace {

void WritingLeaf(ZooBuffer* buffer) {
  buffer->data[6] = 6;
  zoo_callback_parameter(zoo_callback_direct, buffer, 7);
}

void SelfRecursive(int count, ZooBuffer* buffer) {
  if (count <= 0) {
    WritingLeaf(buffer);
    return;
  }
  SelfRecursive(count - 1, buffer);
}

void MutualOdd(int count, ZooBuffer* buffer);

void MutualEven(int count, ZooBuffer* buffer) {
  if (count <= 0) {
    WritingLeaf(buffer);
    return;
  }
  MutualOdd(count - 1, buffer);
}

void MutualOdd(int count, ZooBuffer* buffer) {
  if (count <= 0) {
    WritingLeaf(buffer);
    return;
  }
  MutualEven(count - 1, buffer);
}

}  // namespace

extern "C" void zoo_recursive_entry(int count, ZooBuffer* buffer) {
  SelfRecursive(count, buffer);
  MutualEven(count, buffer);
}
