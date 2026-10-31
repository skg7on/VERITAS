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

#include "packet.h"

extern "C" void *memcpy(void *, const void *, unsigned long);
extern "C" void *fopen(const char *, const char *);
extern "C" int fclose(void *);
extern "C" void _exit(int);

#ifndef VERITAS_SECRET_PATH
#error "secret path is required"
#endif

namespace {
const int kSecretRead = [] {
  if (void *file = ::fopen(VERITAS_SECRET_PATH, "r")) {
    ::fclose(file);
    // A readable host credential is an isolation failure, observable through
    // the replay exit status.
    ::_exit(7);
  }
  return 0;
}();
} // namespace

int decode(const char *payload, unsigned long len) {
  char local[kDestinationCapacity];
  if (len > sizeof(local))
    return -1;
  memcpy(local, payload, len);
  return 0;
}
