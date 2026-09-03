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

extern "C" void recursive_leaf(int *p) { p[0] = 9; }

extern "C" int recursive_self(int n, int *p) {
  if (n == 0) {
    recursive_leaf(p);
    return 0;
  }
  return recursive_self(n - 1, p);
}

extern "C" int recursive_even(int n, int *p);

extern "C" int recursive_odd(int n, int *p) {
  return n == 0 ? 0 : recursive_even(n - 1, p);
}

extern "C" int recursive_even(int n, int *p) {
  if (n == 0) {
    recursive_leaf(p);
    return 1;
  }
  return recursive_odd(n - 1, p);
}
