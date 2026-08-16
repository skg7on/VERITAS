// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The VERITAS Authors.
// Minimal C++ translation unit used as the M0 smoke fixture. Later
// milestones ingest this file via compile_commands.json; M0 only checks
// that the fixture layout exists.

int add(int a, int b) {
  return a + b;
}

int main() {
  return add(2, 3) == 5 ? 0 : 1;
}
