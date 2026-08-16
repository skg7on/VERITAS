#!/usr/bin/env bash
# Copyright 2026 VERITAS Contributors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -euo pipefail

fail() {
  printf 'FAIL: %s\n' "$1" >&2
  exit 1
}

hook_under_test=${1:?"usage: $0 <pre-commit-hook>"}
[[ -f "$hook_under_test" ]] || fail "pre-commit hook is missing: $hook_under_test"

project_root=$(cd "$(dirname "$hook_under_test")/.." && pwd)
test_root=$(mktemp -d "${TMPDIR:-/tmp}/veritas-pre-commit-test.XXXXXX")
trap 'rm -rf "$test_root"' EXIT

repo="$test_root/repo"
git init -q "$repo"
git -C "$repo" config user.email "veritas-test@example.com"
git -C "$repo" config user.name "VERITAS Test"
git -C "$repo" config core.hooksPath .githooks

mkdir -p "$repo/.githooks"
cp "$hook_under_test" "$repo/.githooks/pre-commit"
chmod +x "$repo/.githooks/pre-commit"
cp "$project_root/.clang-format" "$repo/.clang-format"

# A hook that formats the working tree instead of the index would reject this
# commit or stage the developer's unrelated edit.
printf 'int kept() { return 0; }\n' >"$repo/partial.cpp"
git -C "$repo" add partial.cpp .clang-format
printf 'int kept( ){return 0;}\n' >"$repo/partial.cpp"
git -C "$repo" commit -q -m "accept formatted staged content" ||
  fail "formatted staged content was rejected"
if git -C "$repo" diff --quiet -- partial.cpp; then
  fail "the hook modified or staged an unstaged C++ edit"
fi

printf 'int bad( ){return 0;}\n' >"$repo/bad.cpp"
git -C "$repo" add bad.cpp
if git -C "$repo" commit -q -m "reject unformatted staged content" \
  >"$test_root/rejected-commit.log" 2>&1; then
  fail "unformatted staged C++ content was accepted"
fi

git -C "$repo" reset -q bad.cpp
printf 'plain text is not C++\n' >"$repo/notes.txt"
git -C "$repo" add notes.txt
git -C "$repo" commit -q -m "ignore non-C++ content" ||
  fail "non-C++ content was rejected"

printf 'PASS: pre-commit hook checks staged C++ content only\n'
