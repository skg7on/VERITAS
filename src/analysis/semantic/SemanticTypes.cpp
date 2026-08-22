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

#include "veritas/analysis/semantic/SemanticTypes.h"

#include <string_view>

namespace veritas::analysis::semantic {

namespace {

bool ContainsControlCharacter(std::string_view text) {
  for (const unsigned char c : text) {
    if (c < 0x20 || c == 0x7f) {
      return true;
    }
  }
  return false;
}

}  // namespace

ByteRangeKind RelationRangeKind(const ByteRange& range) {
  if (range.offset.has_value() && range.size.has_value()) {
    return ByteRangeKind::kKnown;
  }
  return ByteRangeKind::kUnknown;
}

Status Validate(const ByteRange& range) {
  const bool offset_known = range.offset.has_value();
  const bool size_known = range.size.has_value();
  if (offset_known != size_known) {
    return Status::InvalidArgument("half-known byte range");
  }
  return Status::Ok();
}

Status Validate(const AbstractObject& object) {
  if (object.id.kind != core::IdKind::kAbstractObject) {
    return Status::InvalidArgument("abstract object ID has wrong kind");
  }
  if (object.owner_function.has_value() &&
      object.owner_function->kind != core::IdKind::kFunctionVariant) {
    return Status::InvalidArgument("owner function ID has wrong kind");
  }
  if (ContainsControlCharacter(object.semantic_anchor)) {
    return Status::InvalidArgument(
        "semantic anchor contains control characters");
  }
  return Status::Ok();
}

Status Validate(const MemoryLocation& location) {
  if (location.id.kind != core::IdKind::kMemoryRef) {
    return Status::InvalidArgument("memory location ID has wrong kind");
  }
  if (Status status = Validate(location.object); !status.ok()) {
    return status;
  }
  if (Status status = Validate(location.byte_range); !status.ok()) {
    return status;
  }
  return Status::Ok();
}

}  // namespace veritas::analysis::semantic
