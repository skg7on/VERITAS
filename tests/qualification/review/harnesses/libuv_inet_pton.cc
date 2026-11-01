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

// Trusted operator-registered harness for the libuv scoped-IPv6 corpus case.
// It supplies the captured C entry point with exactly the registered payload
// bytes. The replay producer compiles it separately from the captured
// translation unit and never lets model input choose the harness, compiler or
// payload.
//
// The replay producer's argv contract is {program, payload_path,
// requested_bytes}; the same contract as the integration fixture harness.
// The harness re-checks that the payload's scope-id prefix length equals the
// requested length, so a request cannot be attributed to a different copy
// size.
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>

extern "C" int uv_inet_pton(int af, const char *src, void *dst);

int main(int argc, char **argv) {
  if (argc != 3)
    return 3;
  char *end = nullptr;
  const unsigned long requested = std::strtoul(argv[2], &end, 10);
  if (!end || *end)
    return 3;
  std::FILE *file = std::fopen(argv[1], "rb");
  if (!file)
    return 3;
  char text[4096];
  const std::size_t got = std::fread(text, 1, sizeof(text) - 1, file);
  std::fclose(file);
  if (got == 0)
    return 3;
  text[got] = '\0';
  const char *percent = std::strchr(text, '%');
  const char *colon = std::strrchr(text, ':');
  if (!percent || !colon || colon >= percent)
    return 3;
  if (static_cast<unsigned long>(percent - colon - 1) != requested)
    return 3;
  unsigned char destination[16];
  return uv_inet_pton(AF_INET6, text, destination) == 0 ? 0 : 2;
}
