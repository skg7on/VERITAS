// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The VERITAS Authors.
#include <cstring>
#include <iostream>

#include "veritas/core/Version.h"

int main(int argc, char* argv[]) {
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    std::cout << veritas::FormatVersion(veritas::GetVersion()) << '\n';
    return 0;
  }

  std::cerr << "veritas-query: no analysis implemented (M0 skeleton)\n";
  return 1;
}
