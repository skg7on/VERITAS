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

// Calls a plain modeled external function. SVF clones such extapi models into
// the module as synthetic definitions carrying no function-variant identity,
// which the CPG projection must tolerate.
#include <string.h>

const char *find_sep(const char *s) {
  return strchr(s, '/');
}

int main(void) {
  const char *p = find_sep("a/b");
  return p != 0 ? 0 : 1;
}
