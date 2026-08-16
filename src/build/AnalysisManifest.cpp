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

#include "veritas/build/AnalysisManifest.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace veritas::build {

namespace {

// -- length-prefixed canonical encoding ---------------------------------------
//
// AppendU32 / AppendU64 write big-endian integers so the byte stream is
// endianness-independent. AppendField writes:
//   u32 key_size | key bytes | u64 value_size | value bytes
// Records are appended in the exact order the caller emits them, and that
// order is the source of truth for equality. Never rely on hash-map iteration.

void AppendU32(std::string& out, std::uint32_t value) {
  std::array<std::uint8_t, 4> bytes = {
      static_cast<std::uint8_t>((value >> 24) & 0xff),
      static_cast<std::uint8_t>((value >> 16) & 0xff),
      static_cast<std::uint8_t>((value >> 8) & 0xff),
      static_cast<std::uint8_t>(value & 0xff),
  };
  out.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void AppendU64(std::string& out, std::uint64_t value) {
  std::array<std::uint8_t, 8> bytes = {
      static_cast<std::uint8_t>((value >> 56) & 0xff),
      static_cast<std::uint8_t>((value >> 48) & 0xff),
      static_cast<std::uint8_t>((value >> 40) & 0xff),
      static_cast<std::uint8_t>((value >> 32) & 0xff),
      static_cast<std::uint8_t>((value >> 24) & 0xff),
      static_cast<std::uint8_t>((value >> 16) & 0xff),
      static_cast<std::uint8_t>((value >> 8) & 0xff),
      static_cast<std::uint8_t>(value & 0xff),
  };
  out.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void AppendField(std::string& out, std::string_view key,
                 std::string_view value) {
  AppendU32(out, static_cast<std::uint32_t>(key.size()));
  out.append(key.data(), key.size());
  AppendU64(out, static_cast<std::uint64_t>(value.size()));
  out.append(value.data(), value.size());
}

std::string RootKindToken(PathRootKind kind) {
  switch (kind) {
    case PathRootKind::kRepository:
      return "repository";
    case PathRootKind::kGenerated:
      return "generated";
    case PathRootKind::kExternal:
      return "external";
    case PathRootKind::kToolchain:
      return "toolchain";
  }
  return "unknown";
}

auto TranslationUnitSortKey(const TranslationUnitCommand& unit) {
  return std::tuple<int, std::string_view, std::string, std::string_view>{
      static_cast<int>(unit.source_path.root_kind),
      unit.source_path.root_id,
      unit.source_path.relative_path.generic_string(),
      unit.command_hash,
  };
}

std::vector<TranslationUnitCommand> OrderedTranslationUnits(
    const AnalysisManifest& manifest) {
  auto units = manifest.translation_units;
  std::sort(units.begin(), units.end(),
            [](const TranslationUnitCommand& a,
               const TranslationUnitCommand& b) {
              return TranslationUnitSortKey(a) < TranslationUnitSortKey(b);
            });
  return units;
}

}  // namespace

std::string ToCanonicalBytes(const TranslationUnitCommand& unit) {
  std::string bytes;
  AppendField(bytes, "translation_unit_id", unit.translation_unit_id);
  AppendField(bytes, "revision_id", unit.revision_id);
  AppendField(bytes, "build_variant_id", unit.build_variant_id);
  AppendField(bytes, "source_root_kind", RootKindToken(unit.source_path.root_kind));
  AppendField(bytes, "source_root_id", unit.source_path.root_id);
  AppendField(bytes, "source_path",
              unit.source_path.relative_path.generic_string());
  AppendField(bytes, "working_directory_root_kind",
              RootKindToken(unit.working_directory.root_kind));
  AppendField(bytes, "working_directory_root_id", unit.working_directory.root_id);
  AppendField(bytes, "working_directory",
              unit.working_directory.relative_path.generic_string());
  AppendU32(bytes, static_cast<std::uint32_t>(unit.arguments.size()));
  for (const auto& argument : unit.arguments) {
    AppendField(bytes, "argument", argument);
  }
  AppendField(bytes, "command_hash", unit.command_hash);
  AppendField(bytes, "preprocessor_hash", unit.preprocessor_hash);
  return bytes;
}

std::string ToCanonicalBytes(const AnalysisManifest& manifest) {
  std::string bytes;
  const auto& ctx = manifest.context;
  AppendField(bytes, "repository_id", ctx.repository_id);
  AppendField(bytes, "revision_id", ctx.revision_id);
  AppendField(bytes, "build_variant_id", ctx.build_variant_id);
  AppendField(bytes, "vcs_kind", ctx.vcs_kind);
  AppendField(bytes, "vcs_revision", ctx.vcs_revision);
  AppendField(bytes, "source_tree_hash", ctx.source_tree_hash);
  AppendField(bytes, "compilation_database_hash", ctx.compilation_database_hash);
  AppendField(bytes, "target_triple", ctx.target_triple);
  AppendField(bytes, "compiler_id", ctx.compiler_id);
  AppendField(bytes, "compiler_version", ctx.compiler_version);
  AppendField(bytes, "compile_options_hash", ctx.compile_options_hash);
  AppendField(bytes, "macro_set_hash", ctx.macro_set_hash);
  AppendField(bytes, "include_closure_hash", ctx.include_closure_hash);
  AppendField(bytes, "type_layout_hash", ctx.type_layout_hash);
  const auto ordered = OrderedTranslationUnits(manifest);
  AppendU32(bytes, static_cast<std::uint32_t>(ordered.size()));
  for (const auto& unit : ordered) {
    AppendField(bytes, "translation_unit", ToCanonicalBytes(unit));
  }
  return bytes;
}

// -- diagnostic JSON ---------------------------------------------------------

namespace {

void AppendJsonEscaped(std::string& out, std::string_view text) {
  out.push_back('"');
  for (const char raw : text) {
    const auto byte = static_cast<unsigned char>(raw);
    switch (byte) {
      case '"':  out.append("\\\""); break;
      case '\\': out.append("\\\\"); break;
      case '\b': out.append("\\b"); break;
      case '\f': out.append("\\f"); break;
      case '\n': out.append("\\n"); break;
      case '\r': out.append("\\r"); break;
      case '\t': out.append("\\t"); break;
      default:
        if (byte < 0x20) {
          char escape[8];
          std::snprintf(escape, sizeof(escape), "\\u%04x", byte);
          out.append(escape);
        } else {
          out.push_back(raw);
        }
    }
  }
  out.push_back('"');
}

void AppendJsonKey(std::string& out, std::string_view key) {
  AppendJsonEscaped(out, key);
  out.append(": ");
}

void AppendJsonString(std::string& out, std::string_view key,
                      std::string_view value) {
  AppendJsonKey(out, key);
  AppendJsonEscaped(out, value);
}

std::string ToJsonTaggedPath(const TaggedPath& path) {
  std::string out;
  out.append("{");
  AppendJsonString(out, "root_kind", RootKindToken(path.root_kind));
  out.append(", ");
  AppendJsonString(out, "root_id", path.root_id);
  out.append(", ");
  AppendJsonString(out, "relative_path", path.relative_path.generic_string());
  out.append("}");
  return out;
}

std::string ToJsonTranslationUnit(const TranslationUnitCommand& unit) {
  std::string out;
  out.append("{\n");
  out.append("      ");
  AppendJsonString(out, "translation_unit_id", unit.translation_unit_id);
  out.append(",\n      ");
  AppendJsonString(out, "revision_id", unit.revision_id);
  out.append(",\n      ");
  AppendJsonString(out, "build_variant_id", unit.build_variant_id);
  out.append(",\n      ");
  AppendJsonKey(out, "source_path");
  out.append(ToJsonTaggedPath(unit.source_path));
  out.append(",\n      ");
  AppendJsonKey(out, "working_directory");
  out.append(ToJsonTaggedPath(unit.working_directory));
  out.append(",\n      ");
  AppendJsonKey(out, "arguments");
  out.append("[");
  for (std::size_t i = 0; i < unit.arguments.size(); ++i) {
    if (i > 0) out.append(", ");
    AppendJsonEscaped(out, unit.arguments[i]);
  }
  out.append("],\n      ");
  AppendJsonString(out, "command_hash", unit.command_hash);
  out.append(",\n      ");
  AppendJsonString(out, "preprocessor_hash", unit.preprocessor_hash);
  out.append("\n    }");
  return out;
}

}  // namespace

std::string ToDiagnosticJson(const AnalysisManifest& manifest) {
  std::string out;
  out.append("{\n");

  const auto& ctx = manifest.context;
  out.append("  ");
  AppendJsonKey(out, "context");
  out.append("{\n");
  auto append_ctx = [&](std::string_view key, std::string_view value,
                        bool trailing) {
    out.append("    ");
    AppendJsonString(out, key, value);
    out.append(trailing ? ",\n" : "\n");
  };
  append_ctx("build_variant_id", ctx.build_variant_id, true);
  append_ctx("compilation_database_hash", ctx.compilation_database_hash, true);
  append_ctx("compile_options_hash", ctx.compile_options_hash, true);
  append_ctx("compiler_id", ctx.compiler_id, true);
  append_ctx("compiler_version", ctx.compiler_version, true);
  append_ctx("include_closure_hash", ctx.include_closure_hash, true);
  append_ctx("macro_set_hash", ctx.macro_set_hash, true);
  append_ctx("repository_id", ctx.repository_id, true);
  append_ctx("revision_id", ctx.revision_id, true);
  append_ctx("source_tree_hash", ctx.source_tree_hash, true);
  append_ctx("target_triple", ctx.target_triple, true);
  append_ctx("type_layout_hash", ctx.type_layout_hash, true);
  append_ctx("vcs_kind", ctx.vcs_kind, true);
  append_ctx("vcs_revision", ctx.vcs_revision, false);
  out.append("  },\n");

  const auto ordered = OrderedTranslationUnits(manifest);
  out.append("  ");
  AppendJsonKey(out, "translation_units");
  out.append("[");
  for (std::size_t i = 0; i < ordered.size(); ++i) {
    out.append(i == 0 ? "\n    " : ",\n    ");
    out.append(ToJsonTranslationUnit(ordered[i]));
  }
  if (!ordered.empty()) out.append("\n  ");
  out.append("]\n");
  out.append("}\n");
  return out;
}

}  // namespace veritas::build
