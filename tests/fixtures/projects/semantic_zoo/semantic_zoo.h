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

#ifndef VERITAS_TESTS_FIXTURES_PROJECTS_SEMANTIC_ZOO_SEMANTIC_ZOO_H_
#define VERITAS_TESTS_FIXTURES_PROJECTS_SEMANTIC_ZOO_SEMANTIC_ZOO_H_

typedef unsigned long ZooSize;

typedef struct ZooPayload {
  unsigned char bytes[16];
  ZooSize length;
} ZooPayload;

typedef struct ZooEnvelope {
  int tag;
  ZooPayload payload;
} ZooEnvelope;

typedef struct ZooBuffer {
  unsigned char data[32];
  ZooSize capacity;
  ZooSize length;
} ZooBuffer;

typedef void (*ZooCallback)(ZooBuffer*, int);

#ifdef __cplusplus
extern "C" {
#endif

extern int zoo_external_state;

void zoo_memory_shapes(ZooBuffer*, ZooBuffer*, ZooEnvelope*, int**, int);
void zoo_callback_direct(ZooBuffer*, int);
void zoo_callback_indirect(ZooBuffer*, int);
void zoo_callback_parameter(ZooCallback, ZooBuffer*, int);
void zoo_callback_select(int, ZooBuffer*, int);
void zoo_modeled_copy(ZooBuffer*, const ZooEnvelope*);
void zoo_dispatch_direct(ZooBuffer*);
void zoo_virtual_one(ZooBuffer*);
void zoo_virtual_select(int, ZooBuffer*);
void zoo_recursive_entry(int, ZooBuffer*);
void zoo_driver(int, ZooBuffer*, const ZooEnvelope*);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // VERITAS_TESTS_FIXTURES_PROJECTS_SEMANTIC_ZOO_SEMANTIC_ZOO_H_
