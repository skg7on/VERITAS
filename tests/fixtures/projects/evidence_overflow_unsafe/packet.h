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

#ifndef EVIDENCE_OVERFLOW_UNSAFE_PACKET_H_
#define EVIDENCE_OVERFLOW_UNSAFE_PACKET_H_

// A 16-bit unsigned length provides the [0, 65535] value range without a
// handwritten fact; a 2048-byte array provides the destination capacity.

struct Packet {
  const unsigned char* payload;
  unsigned short length;
};

struct Buffer {
  unsigned char data[2048];
};

#endif  // EVIDENCE_OVERFLOW_UNSAFE_PACKET_H_
