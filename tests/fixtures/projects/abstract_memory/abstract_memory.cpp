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

// Abstract-memory fixture. write_first writes a single byte of a struct field;
// caller reaches that write through a resolved call, so may-write facts must
// flow from write_first up to caller.

struct Buffer {
  char data[64];
};

void write_first(Buffer* b, char c) {
  b->data[0] = c;
}

void caller(Buffer* b) {
  write_first(b, 'x');
}
