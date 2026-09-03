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

// Callback-dispatch fixture. A function-pointer table routes the indirect call
// in `dispatch` to `inc` and `dec`, which Andersen's points-to analysis must
// resolve into two stable MAY call targets.

typedef int (*Handler)(int);

int inc(int x) {
  return x + 1;
}

int dec(int x) {
  return x - 1;
}

Handler handlers[2] = {inc, dec};

int dispatch(int which, int value) {
  return handlers[which](value);
}
