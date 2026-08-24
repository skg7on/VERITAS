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

// Field-sensitive pointer analysis fixture
struct Record {
  int payload;
  int metadata;
};

int field_access(Record* record) {
  return record->payload;
}

void field_store(Record* record) {
  record->metadata = 42;
}

int variable_index(int* values, int index) {
  return values[index];
}
