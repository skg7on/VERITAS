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

// Indirect call through a global function pointer fixture. `fp` is initialized
// to `identity` so Andersen's points-to analysis resolves the indirect call in
// `invoke_callback` to `identity`, which surfaces as a stable MAY target.
typedef int (*Callback)(int);

int identity(int x) {
  return x;
}

Callback fp = &identity;

int invoke_callback(int value) {
  return fp(value);
}
