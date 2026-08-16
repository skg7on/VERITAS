# License Header Policy

This policy applies to every VERITAS-authored file that carries source
code or a build script. It is enforced on every commit and every pull
request.

VERITAS is licensed under Apache-2.0. Every in-scope file must open with
an SPDX-style header identifying the license and the copyright holder so
license status is verifiable per-file, without inspecting `LICENSE` or
`git log`.

## Required header

The header must appear on the first two lines of the file, before any
other content (no leading blank line, no shebang between it and the top
of the file — put shebangs above the header when needed).

### C, C++, header, template, and JSON-with-comments files

```cpp
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The VERITAS Authors.
```

Applies to: `*.h`, `*.hpp`, `*.hh`, `*.c`, `*.cc`, `*.cpp`, `*.cxx`,
`*.cpp.in`, `*.h.in`, `*.hpp.in`, `*.inc`.

### CMake files and shell scripts

```cmake
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The VERITAS Authors.
```

Applies to: `CMakeLists.txt`, `*.cmake`, `*.cmake.in`, `*.sh`, `*.bash`,
`*.zsh`, `*.py`.

For shell / Python scripts with a shebang, the shebang stays on line 1
and the two-line header follows on lines 2–3.

## Copyright year

Use the year the file was first added to the repository. Do not update
the year on unrelated edits. When bulk-adding headers to files that
predate this policy, use the current calendar year.

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
task worktree:

```bash
missing=$(git ls-files \
  'include/**/*.h' 'include/**/*.hpp' \
  'src/**/*.c' 'src/**/*.cc' 'src/**/*.cpp' 'src/**/*.h' 'src/**/*.hpp' \
  'src/**/*.cpp.in' 'src/**/*.h.in' \
  'tests/**/*.cpp' 'tests/**/*.h' \
  'cmake/**/*.cmake' 'cmake/CMakeLists.txt' \
  'CMakeLists.txt' \
  | while read -r f; do
      head -2 "$f" | grep -q "SPDX-License-Identifier: Apache-2.0" || echo "$f"
    done)
[ -z "$missing" ] || { printf 'missing license header:\n%s\n' "$missing" >&2; exit 1; }
```

Fix any file the check reports before committing.
