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

typedef void (*DispatchCallback)(int *);

static void callback_left(int *p) { p[0] = 1; }
static void callback_right(int *p) { p[1] = 2; }

DispatchCallback callback_global = callback_left;
DispatchCallback callback_table[2] = {callback_left, callback_right};

void callback_direct(int *p) { callback_left(p); }

void callback_seed(int choose) {
  callback_global = choose ? callback_left : callback_right;
}

void callback_indirect(int *p) { callback_global(p); }

void callback_parameter(DispatchCallback cb, int *p) { cb(p); }

void callback_parameter_entry(int *p) { callback_parameter(callback_left, p); }

void callback_cast_data_pointer(void *raw, int *p) {
  ((DispatchCallback)raw)(p);
}

void callback_cast_data_pointer_entry(int *p) {
  callback_cast_data_pointer((void *)callback_right, p);
}

void callback_forwarding_slot(DispatchCallback cb, int *p) {
  DispatchCallback forwarded = cb;
  forwarded(p);
}

void callback_forwarding_slot_entry(int *p) {
  callback_forwarding_slot(callback_right, p);
}

void callback_select(int choose, int *p) {
  callback_table[choose != 0](p);
}
