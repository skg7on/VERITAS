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

extern "C" void *memcpy(void *, const void *, unsigned long);
extern "C" void observe(const void *, unsigned long);

void first_copy(void *destination, const void *source, unsigned long length) {
  memcpy(destination, source, length);
  observe(source, 7);
}

void second_copy(unsigned long length, const void *source, void *destination) {
  observe("unrelated external payload" + 1, length);
  memcpy(destination, source, 13);
}
