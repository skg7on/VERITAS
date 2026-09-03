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

int memory_global_value;
static int memory_static_value;

union MemoryOverlap { int whole; unsigned char bytes[4]; };
struct MemoryInner { int values[4]; };
struct MemoryOuter { int tag; struct MemoryInner inner; };

void memory_global(void) {
  memory_global_value = 1;
  memory_static_value = 2;
}

int memory_nested(struct MemoryOuter *p) { return p->inner.values[2]; }

int memory_constant_index(int *p) { return p[3]; }

int memory_variable_index(int *p, int index) { return p[index]; }

int memory_overlap(union MemoryOverlap *p) {
  p->whole = 0;
  return p->bytes[0];
}

int memory_zero_range(struct MemoryOuter *p) { return p->tag; }
