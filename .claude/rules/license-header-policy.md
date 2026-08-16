# License Header Policy

This policy applies to every VERITAS-authored file that carries source
code or a build script. It is enforced on every commit and every pull
request.

VERITAS is licensed under Apache-2.0. Every in-scope file must open with
the full Apache-2.0 license notice so license status is verifiable
per-file, without inspecting `LICENSE` or `git log`.

## Required header

The header must appear at the very top of the file, before any other
content. It uses the file's native comment syntax. For files with a
shebang, the shebang stays on line 1 and the header follows immediately.

### C, C++, header, and template files

Applies to: `*.h`, `*.hpp`, `*.hh`, `*.c`, `*.cc`, `*.cpp`, `*.cxx`,
`*.cpp.in`, `*.h.in`, `*.hpp.in`, `*.inc`.

```cpp
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
```

### CMake files, shell scripts, and Python

Applies to: `CMakeLists.txt`, `*.cmake`, `*.cmake.in`, `*.sh`, `*.bash`,
`*.zsh`, `*.py`.

```cmake
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
```

## Copyright year

Use the year the file was first added to the repository. Do not update
the year on unrelated edits. When bulk-adding headers to files that
predate this policy, use the current calendar year.

The copyright holder is always `VERITAS Contributors` — a collective
attribution that avoids per-file author lists.

## In-scope directories

- `include/`
- `src/`
- `tests/` (source, fixture code, and per-directory `CMakeLists.txt` —
  see exemptions below for fixture data)
- `cmake/`
- Top-level `CMakeLists.txt`
- Any future first-party subtree (proto definitions, Datalog rules, DSL
  sources, generator inputs)

## Exemptions

- `third_party/` — upstream code carries its own license headers; do not
  add or remove them.
- `build/` and any other generated tree — never edited by hand.
- Files without a native comment syntax: raw JSON (`*.json`), YAML with
  strict schema validators that reject comment lines, TOML files where
  the tool rejects comments. Prefer a sibling `*.LICENSE` or a note in
  the enclosing directory's `README.md` when attribution is needed.
- Markdown, reStructuredText, plain text prose, images, PDFs, and other
  documentation formats.
- The `LICENSE` file itself and `NOTICE`-style attribution files.
- `.gitignore`, `.gitmodules`, `.gitattributes`, `.editorconfig`,
  `.clang-format`, and other tool-driven dotfiles.

## Adding a new file

Every new C, C++, or CMake file lands with the header pre-populated.
Reviewers reject PRs that add in-scope files without a header.

## Verification

Before opening or updating a pull request, run a quick check from the
task worktree. The fingerprint is the Apache-2.0 grant clause, which is
unique to the boilerplate and must appear in the first 20 lines of every
in-scope file:

```bash
missing=$(git ls-files \
  'CMakeLists.txt' 'cmake' 'include' 'src' 'tests' \
  | grep -v -E '^(third_party|build)/' \
  | grep -v -E '\.(json|md|rst)$' \
  | while IFS= read -r f; do
      head -20 "$f" | grep -q 'Licensed under the Apache License, Version 2.0' \
        || echo "$f"
    done)
[ -z "$missing" ] || { printf 'missing license header:\n%s\n' "$missing" >&2; exit 1; }
```

Fix any file the check reports before committing.
