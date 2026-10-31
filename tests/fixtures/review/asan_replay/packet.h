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

#ifndef VERITAS_TEST_ASAN_REPLAY_PACKET_H_
#define VERITAS_TEST_ASAN_REPLAY_PACKET_H_
// The trusted harness asserts this concrete extent against the replay target;
// a mismatch between analysis and replay layouts refuses replay. The harness
// source capacity is owned by the trusted harness, not by captured input.
inline constexpr unsigned long kDestinationCapacity = 32;
int decode(const char *payload, unsigned long len);
#endif
