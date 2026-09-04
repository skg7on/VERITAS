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

#include "semantic_zoo.h"

namespace {

class ZooWriter {
 public:
  virtual void Write(ZooBuffer* buffer) = 0;
};

class ZooTag {
 public:
  virtual int Tag() const = 0;
};

class ZooSingle final : public ZooWriter {
 public:
  void Write(ZooBuffer* buffer) override { buffer->data[3] = 3; }
  void Direct(ZooBuffer* buffer) { buffer->data[4] = 4; }
};

class ZooMultiple final : public ZooWriter, public ZooTag {
 public:
  void Write(ZooBuffer* buffer) override { buffer->data[5] = 5; }
  int Tag() const override { return 5; }
};

}  // namespace

extern "C" void zoo_dispatch_direct(ZooBuffer* buffer) {
  ZooSingle value;
  value.Direct(buffer);
}

extern "C" void zoo_virtual_one(ZooBuffer* buffer) {
  ZooSingle value;
  ZooWriter* writer = &value;
  writer->Write(buffer);
}

extern "C" void zoo_virtual_select(int choose, ZooBuffer* buffer) {
  ZooSingle single;
  ZooMultiple multiple;
  ZooWriter* writer = choose != 0 ? static_cast<ZooWriter*>(&single)
                                  : static_cast<ZooWriter*>(&multiple);
  writer->Write(buffer);
}
