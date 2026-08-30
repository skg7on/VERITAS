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

// Calls the three functions the shipped model bundle describes: malloc,
// memcpy, and free. Every other fixture avoids them, which left model
// materialization unexercised against real extraction output -- in particular
// what callee symbol survives into summary.v2 once Clang has lowered a call it
// recognizes as a library builtin.
//
// The declarations are written out rather than included: the in-process Clang
// invocation carries no system include paths, so a fixture that includes
// <cstring> fails to compile. Every other fixture avoids standard headers for
// the same reason.

extern "C" void* malloc(unsigned long size);
extern "C" void free(void* pointer);
extern "C" void* memcpy(void* destination, const void* source,
                        unsigned long length);

void copy_buffer(const char* source, unsigned long length) {
  char* buffer = static_cast<char*>(malloc(length));
  if (buffer == nullptr)
    return;
  memcpy(buffer, source, length);
  free(buffer);
}
