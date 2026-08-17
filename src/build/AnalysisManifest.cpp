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
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "llvm/Support/Endian.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

namespace veritas::build {

namespace {

// -- length-prefixed canonical encoding ---------------------------------------
//
// AppendField writes:
//   u32 key_size (big-endian) | key bytes | u64 value_size (big-endian) | value
// Records land in the exact order the field visitor emits them, and that order
// is the source of truth for equality. Never rely on hash-map iteration.

template <typename T>
void AppendBigEndian(std::string& out, T value) {
  char bytes[sizeof(T)];
  llvm::support::endian::write<T, llvm::support::unaligned>(
      bytes, value, llvm::endianness::big);
  out.append(bytes, sizeof(T));
}

void AppendField(std::string& out, std::string_view key,
                 std::string_view value) {
  AppendBigEndian(out, static_cast<std::uint32_t>(key.size()));
  out.append(key.data(), key.size());
  AppendBigEndian(out, static_cast<std::uint64_t>(value.size()));
  out.append(value.data(), value.size());
}

std::string_view RootKindToken(PathRootKind kind) {
  switch (kind) {
    case PathRootKind::kRepository: return "repository";
    case PathRootKind::kGenerated:  return "generated";
    case PathRootKind::kExternal:   return "external";
    case PathRootKind::kToolchain:  return "toolchain";
  }
  return "unknown";
}

// -- field visitors -----------------------------------------------------------
//
// Both encoders walk fields in the same alphabetical order. Adding a field to
// ProgramContext / TranslationUnitCommand means adding one line here and
// nowhere else — encoders share the ordering by construction, so forgetting to
// update one of them is impossible.

template <typename EmitScalar>
void VisitContextFields(const ProgramContext& ctx, EmitScalar emit) {
  emit("build_variant_id", ctx.build_variant_id);
  emit("compilation_database_hash", ctx.compilation_database_hash);
  emit("compile_options_hash", ctx.compile_options_hash);
  emit("compiler_id", ctx.compiler_id);
  emit("compiler_version", ctx.compiler_version);
  emit("include_closure_hash", ctx.include_closure_hash);
  emit("macro_set_hash", ctx.macro_set_hash);
  emit("repository_id", ctx.repository_id);
  emit("revision_id", ctx.revision_id);
  emit("source_tree_hash", ctx.source_tree_hash);
  emit("target_triple", ctx.target_triple);
  emit("type_layout_hash", ctx.type_layout_hash);
  emit("vcs_kind", ctx.vcs_kind);
  emit("vcs_revision", ctx.vcs_revision);
}

// TaggedPath flattens into three scalar fields sharing a prefix so both
// encoders can emit them without a separate nested type.
template <typename EmitScalar>
void VisitTaggedPath(std::string_view prefix, const TaggedPath& path,
                     EmitScalar emit) {
  emit(std::string(prefix) + "_relative_path",
       path.relative_path.generic_string());
  emit(std::string(prefix) + "_root_id", path.root_id);
  emit(std::string(prefix) + "_root_kind", RootKindToken(path.root_kind));
}

template <typename EmitScalar, typename EmitStringArray>
void VisitTranslationUnitFields(const TranslationUnitCommand& unit,
                                EmitScalar emit,
                                EmitStringArray emit_array) {
  emit_array("arguments", unit.arguments);
  emit("build_variant_id", unit.build_variant_id);
  emit("command_hash", unit.command_hash);
  emit("preprocessor_hash", unit.preprocessor_hash);
  emit("revision_id", unit.revision_id);
  VisitTaggedPath("source", unit.source_path, emit);
  emit("translation_unit_id", unit.translation_unit_id);
  VisitTaggedPath("working_directory", unit.working_directory, emit);
}

std::string JoinNul(const std::vector<std::string>& items) {
  std::string out;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i > 0) out.push_back('\0');
    out.append(items[i]);
  }
  return out;
}

struct TranslationUnitSortKey {
  int root_kind;
  std::string_view root_id;
  std::string relative_path;
  std::string_view command_hash;
  auto tie() const {
    return std::tie(root_kind, root_id, relative_path, command_hash);
  }
  bool operator<(const TranslationUnitSortKey& other) const {
    return tie() < other.tie();
  }
};

// Return pointers into `manifest.translation_units` sorted by (root_kind,
// root_id, relative_path, command_hash). Precomputed keys avoid re-hashing
// `generic_string()` on every comparison, and pointer output avoids deep-
// copying every TU (arguments vector alone is often 20-40 strings).
std::vector<const TranslationUnitCommand*> OrderedTranslationUnits(
    const AnalysisManifest& manifest) {
  const auto& units = manifest.translation_units;
  std::vector<std::pair<TranslationUnitSortKey, const TranslationUnitCommand*>>
      keyed;
  keyed.reserve(units.size());
  for (const auto& unit : units) {
    keyed.push_back({
        TranslationUnitSortKey{
            static_cast<int>(unit.source_path.root_kind),
            unit.source_path.root_id,
            unit.source_path.relative_path.generic_string(),
            unit.command_hash,
        },
        &unit,
    });
  }
  std::sort(keyed.begin(), keyed.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  std::vector<const TranslationUnitCommand*> ordered;
  ordered.reserve(keyed.size());
  for (const auto& entry : keyed) ordered.push_back(entry.second);
  return ordered;
}

}  // namespace

std::string ToCanonicalBytes(const TranslationUnitCommand& unit) {
  std::string bytes;
  VisitTranslationUnitFields(
      unit,
      [&](std::string_view key, std::string_view value) {
        AppendField(bytes, key, value);
      },
      [&](std::string_view key, const std::vector<std::string>& list) {
        AppendField(bytes, key, JoinNul(list));
      });
  return bytes;
}

std::string ToCanonicalBytes(const AnalysisManifest& manifest) {
  std::string bytes;
  VisitContextFields(
      manifest.context,
      [&](std::string_view key, std::string_view value) {
        AppendField(bytes, key, value);
      });
  const auto ordered = OrderedTranslationUnits(manifest);
  AppendBigEndian(bytes, static_cast<std::uint32_t>(ordered.size()));
  for (const auto* unit : ordered) {
    AppendField(bytes, "translation_unit", ToCanonicalBytes(*unit));
  }
  return bytes;
}

// -- diagnostic JSON ---------------------------------------------------------

namespace {

// Emit one TU as a JSON object using the shared field visitor. Arguments
// become a JSON array; every other field becomes an attribute. `llvm::json`
// handles quoting, escaping, and UTF-8 correctness.
void EmitTranslationUnitJson(llvm::json::OStream& j,
                             const TranslationUnitCommand& unit) {
  j.object([&] {
    VisitTranslationUnitFields(
        unit,
        [&](std::string_view key, std::string_view value) {
          j.attribute(llvm::StringRef(key.data(), key.size()),
                      llvm::StringRef(value.data(), value.size()));
        },
        [&](std::string_view key, const std::vector<std::string>& list) {
          j.attributeArray(llvm::StringRef(key.data(), key.size()), [&] {
            for (const auto& arg : list) j.value(arg);
          });
        });
  });
}

}  // namespace

std::string ToDiagnosticJson(const AnalysisManifest& manifest) {
  std::string out;
  llvm::raw_string_ostream os(out);
  llvm::json::OStream j(os, /*IndentSize=*/2);
  j.object([&] {
    j.attributeObject("context", [&] {
      VisitContextFields(manifest.context,
                         [&](std::string_view key, std::string_view value) {
                           j.attribute(llvm::StringRef(key.data(), key.size()),
                                       llvm::StringRef(value.data(),
                                                       value.size()));
                         });
    });
    j.attributeArray("translation_units", [&] {
      for (const auto* unit : OrderedTranslationUnits(manifest)) {
        EmitTranslationUnitJson(j, *unit);
      }
    });
  });
  os.flush();
  out.push_back('\n');
  return out;
}

}  // namespace veritas::build
