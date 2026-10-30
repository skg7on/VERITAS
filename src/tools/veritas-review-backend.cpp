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

#include "veritas/review/ReviewBackend.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <signal.h>
#include <unistd.h>
namespace {
bool ReadBounded(std::istream &in, std::string &out, size_t limit, bool line) {
  out.clear();
  char c;
  while (in.get(c)) {
    if (line && c == '\n')
      return true;
    if (out.size() >= limit)
      return false;
    out += c;
  }
  return !out.empty();
}
} // namespace
int main(int argc, char **argv) {
  using namespace veritas::review;
  // No project-controlled executable/flags are accepted. Configuration is an
  // operator argv selection and is never obtainable from a wire request.
  if (argc != 2 && argc != 4)
    return 2;
  std::string operation = argv[1];
  const std::vector<std::string> operations = {
      "handshake",      "capture",         "context", "seed-candidates",
      "local-evidence", "native-evidence", "verify",  "release"};
  if (std::find(operations.begin(), operations.end(), operation) ==
      operations.end())
    return 2;
  BackendConfig config;
  if (argc == 4) {
    if (std::string_view(argv[2]) != "--config")
      return 2;
    std::ifstream file(argv[3], std::ios::binary);
    std::string bytes;
    if (!ReadBounded(file, bytes, 1024 * 1024, false))
      return 2;
    auto decoded = DecodeBackendConfig(bytes);
    if (!decoded.ok())
      return 2;
    config = std::move(*decoded);
  }
  signal(SIGPIPE, SIG_IGN);
  ReviewBackend backend(config);
  bool first = true;
  std::string request;
  // One JSON object per bounded line; EOF also terminates a one-shot request.
  // Resident sessions preserve immutable handles until release/process exit.
  while (ReadBounded(std::cin, request, config.max_request_bytes, true)) {
    auto decoded = DecodeV2(request);
    if (!decoded.ok() || (first && decoded->operation != operation &&
                          decoded->operation != "handshake"))
      return 2;
    first = false;
    // The parent also enforces a tighter per-call deadline; this watchdog
    // bounds a standalone request, including blocking project capture.
    alarm(static_cast<unsigned>(
        std::min(config.wall_ms / 1000 + 1, uint64_t(121))));
    auto response = backend.Call(request);
    alarm(0);
    if (!response.ok())
      return 2;
    std::cout << *response << '\n' << std::flush;
  }
  return std::cin.eof() ? 0 : 2;
}
