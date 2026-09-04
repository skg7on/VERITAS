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

int alias_copy(int *p) { int *q = p; *q = 7; return *p; }

int alias_disjoint(void) { int a = 1; int b = 2; return a + b; }

int alias_parameters(int *left, int *right) {
  *left = 3;
  return *right;
}

int alias_indirect(int **slot, int *value) {
  *slot = value;
  **slot = 11;
  return *value;
}

// Concrete local addresses provide one proven, singleton-points-to alias.
int alias_must_local(void) {
  int value = 0;
  int *left = &value;
  int *right = &value;
  *left = 5;
  return *right;
}

// A conditional right-hand target overlaps left but is not singleton, so it
// must remain a MAY alias instead of being strengthened.
int alias_may_local(int condition) {
  int first = 0;
  int second = 0;
  int *left = &first;
  int *right = condition ? &first : &second;
  *left = 5;
  return *right;
}
