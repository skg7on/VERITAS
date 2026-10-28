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

extern "C" void *memcpy(void *, const void *, __SIZE_TYPE__);
void two_sites(const char *src, __SIZE_TYPE__ n) {
  int dst[8];
  char other[7];
  if (n <= sizeof(dst))
    memcpy(dst, src, n);
  memcpy(other, src, n);
}
void sibling(const char *src, __SIZE_TYPE__ n) {
  char dst[32];
  if (n <= sizeof(dst)) {
    (void)0;
  }
  memcpy(dst, src, n);
}
void wide(const char *src, __SIZE_TYPE__ n) {
  wchar_t dst[8];
  memcpy(dst, src, n);
}
void pointer(char *dst, const char *src, __SIZE_TYPE__ n) {
  memcpy(dst, src, n);
}
