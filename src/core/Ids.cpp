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

#include "veritas/core/Ids.h"

#include <sstream>
#include <string_view>
#include <unordered_map>

#include "veritas/core/Hash.h"

namespace veritas::core {

namespace {

std::string_view IdKindToString(IdKind kind) {
  switch (kind) {
    case IdKind::kRepository:
      return "repo";
    case IdKind::kRevision:
      return "rev";
    case IdKind::kBuildVariant:
      return "bv";
    case IdKind::kTranslationUnit:
      return "tu";
    case IdKind::kFunctionSymbol:
      return "funcsym";
    case IdKind::kFunctionVariant:
      return "funcvar";
    case IdKind::kFunctionBody:
      return "funcbody";
    case IdKind::kFunctionSummary:
      return "summary";
    case IdKind::kFact:
      return "fact";
    case IdKind::kValueRef:
      return "valref";
    case IdKind::kMemoryRef:
      return "memref";
    case IdKind::kCallSite:
      return "callsite";
    case IdKind::kBasicBlockSummary:
      return "bbsummary";
    case IdKind::kCpgProjection:
      return "cpgproj";
    case IdKind::kCpgEdge:
      return "edge";
    case IdKind::kUnknownNode:
      return "unknown";
  }
  return "unknown";
}

std::optional<IdKind> StringToIdKind(std::string_view str) {
  static const std::unordered_map<std::string_view, IdKind> mapping = {
      {"repo", IdKind::kRepository},
      {"rev", IdKind::kRevision},
      {"bv", IdKind::kBuildVariant},
      {"tu", IdKind::kTranslationUnit},
      {"funcsym", IdKind::kFunctionSymbol},
      {"funcvar", IdKind::kFunctionVariant},
      {"funcbody", IdKind::kFunctionBody},
      {"summary", IdKind::kFunctionSummary},
      {"fact", IdKind::kFact},
      {"valref", IdKind::kValueRef},
      {"memref", IdKind::kMemoryRef},
      {"callsite", IdKind::kCallSite},
      {"bbsummary", IdKind::kBasicBlockSummary},
      {"cpgproj", IdKind::kCpgProjection},
      {"edge", IdKind::kCpgEdge},
      {"unknown", IdKind::kUnknownNode},
  };
  auto it = mapping.find(str);
  if (it != mapping.end()) {
    return it->second;
  }
  return std::nullopt;
}

}  // namespace

StableId MakeStableId(IdKind kind, std::span<const std::byte> canonical_bytes) {
  auto digest = ComputeSHA256(canonical_bytes);
  return StableId{kind, DigestToHex(digest)};
}

std::string ToString(const StableId& id) {
  std::ostringstream oss;
  oss << IdKindToString(id.kind) << ":sha256:" << id.digest_hex;
  return oss.str();
}

StatusOr<StableId> ParseStableId(std::string_view text) {
  size_t first_colon = text.find(':');
  if (first_colon == std::string_view::npos) {
    return Status::InvalidArgument("missing colon separator in ID");
  }

  size_t second_colon = text.find(':', first_colon + 1);
  if (second_colon == std::string_view::npos) {
    return Status::InvalidArgument("missing algorithm separator in ID");
  }

  std::string_view kind_str = text.substr(0, first_colon);
  std::string_view algo_str =
      text.substr(first_colon + 1, second_colon - first_colon - 1);
  std::string_view digest_str = text.substr(second_colon + 1);

  auto kind_opt = StringToIdKind(kind_str);
  if (!kind_opt) {
    return Status::InvalidArgument("unrecognized ID kind");
  }

  if (algo_str != "sha256") {
    return Status::InvalidArgument("unsupported hash algorithm");
  }

  if (digest_str.size() != 64) {
    return Status::InvalidArgument("invalid digest length");
  }

  return StableId{*kind_opt, std::string(digest_str)};
}

}  // namespace veritas::core
