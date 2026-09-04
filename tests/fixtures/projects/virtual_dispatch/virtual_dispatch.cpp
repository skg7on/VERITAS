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

struct DispatchBase {
  virtual void write(int *) = 0;
};

struct DispatchTag {
  virtual int tag() const = 0;
};

struct DispatchLeft final : DispatchBase {
  void write(int *p) override { p[0] = 1; }
};

struct DispatchRight final : DispatchBase, DispatchTag {
  void write(int *p) override { p[1] = 2; }
  int tag() const override { return 2; }
  void direct(int *p) { p[2] = 3; }
};

void virtual_one(DispatchBase *value, int *p) { value->write(p); }

void virtual_two(bool choose, int *p) {
  DispatchLeft left;
  DispatchRight right;
  DispatchBase *value = choose ? static_cast<DispatchBase *>(&left)
                               : static_cast<DispatchBase *>(&right);
  value->write(p);
}

void nonvirtual(DispatchRight *value, int *p) { value->direct(p); }
