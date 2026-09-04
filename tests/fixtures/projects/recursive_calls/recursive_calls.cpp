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

// Mutual-recursion fixture. is_odd and is_even form a single SCC, so
// reachability from either requires the local transitive rule to close the
// cycle, not just a direct call edge.

int is_even(int n);

int is_odd(int n) {
  return n == 0 ? 0 : is_even(n - 1);
}

int is_even(int n) {
  return n == 0 ? 1 : is_odd(n - 1);
}
