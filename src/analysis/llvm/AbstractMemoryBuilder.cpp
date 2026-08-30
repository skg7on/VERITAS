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

#include "analysis/llvm/AbstractMemoryBuilder.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>

#include "analysis/llvm/OriginMap.h"
#include "analysis/llvm/StableValueMapper.h"
#include "veritas/core/Ids.h"

namespace veritas::analysis::llvm {
namespace {

namespace semantic = veritas::analysis::semantic;

// Canonical byte encoding, matching the convention in StableValueMapper.
void AppendTag(std::string* out, char tag) { out->push_back(tag); }

void AppendString(std::string* out, std::string_view value) {
  out->append(std::to_string(value.size()));
  out->push_back(':');
  out->append(value);
}

void AppendUInt(std::string* out, std::uint64_t value) {
  out->append(std::to_string(value));
  out->push_back(';');
}

void AppendInt(std::string* out, std::int64_t value) {
  out->append(std::to_string(value));
  out->push_back(';');
}

struct GepSummary {
  std::vector<semantic::AccessPathSegment> segments;
  bool constant_offset = true;
  std::int64_t offset = 0;
};

// Reconstructs the access path and (when every index is a compile-time
// constant) the byte offset of a single GEP. The walk mirrors LLVM's
// gep_type_iterator (GetElementPtrTypeIterator.h): the FIRST index is a
// sequential "outer" index over the source element type (advancing over an
// unbounded array of that type, so `i32 0` on a single struct is a redundant
// leading array index, not a field), and each subsequent index applies to the
// type reached so far — a StructType index is a field, an array/vector/scalar
// index is a sequential (kArrayIndex) step. Any non-constant or unresolvable
// index appends kUnknown and marks the offset unknown.
GepSummary SummarizeGep(const ::llvm::GEPOperator& gep,
                        const ::llvm::DataLayout& layout) {
  GepSummary summary;
  bool in_struct = false;
  ::llvm::StructType* struct_type = nullptr;
  ::llvm::Type* flat_type = gep.getSourceElementType();

  for (const ::llvm::Use& index_use : gep.indices()) {
    const ::llvm::Value* index = index_use.get();
    const auto* constant = ::llvm::dyn_cast<::llvm::ConstantInt>(index);
    const bool is_constant = constant != nullptr;
    const std::int64_t value = is_constant ? constant->getSExtValue() : 0;

    if (in_struct) {
      // Field index within struct_type.
      if (!is_constant) {
        summary.segments.push_back(
            {semantic::AccessPathSegment::Kind::kUnknown, 0, 0});
        summary.constant_offset = false;
        return summary;
      }
      const auto field = static_cast<unsigned>(value);
      if (field >= struct_type->getNumElements()) {
        summary.constant_offset = false;
        return summary;
      }
      summary.segments.push_back(
          {semantic::AccessPathSegment::Kind::kField, value, value});
      summary.offset += static_cast<std::int64_t>(
          layout.getStructLayout(struct_type)
              ->getElementOffset(field)
              .getFixedValue());
      ::llvm::Type* next = struct_type->getElementType(field);
      if (auto* array = ::llvm::dyn_cast<::llvm::ArrayType>(next)) {
        // An array field flattens to its element type, so a subsequent
        // sequential index strides by the element size (mirrors
        // gep_type_iterator::operator++).
        in_struct = false;
        struct_type = nullptr;
        flat_type = array->getElementType();
      } else if (auto* vector = ::llvm::dyn_cast<::llvm::VectorType>(next)) {
        // A vector field is kept as the vector; the sequential branch steps by
        // the element type.
        in_struct = false;
        struct_type = nullptr;
        flat_type = vector;
      } else if (auto* st = ::llvm::dyn_cast<::llvm::StructType>(next)) {
        in_struct = true;
        struct_type = st;
        flat_type = nullptr;
      } else {
        in_struct = false;
        struct_type = nullptr;
        flat_type = next;
      }
    } else {
      // Sequential index over flat_type (outer array-of-source-type, or an
      // array/vector/scalar element reached by a previous index).
      if (!is_constant) {
        summary.segments.push_back(
            {semantic::AccessPathSegment::Kind::kUnknown, 0, 0});
        summary.constant_offset = false;
        return summary;
      }
      summary.segments.push_back(
          {semantic::AccessPathSegment::Kind::kArrayIndex, value, value});
      ::llvm::Type* step = flat_type;
      if (auto* vector = ::llvm::dyn_cast<::llvm::VectorType>(step))
        step = vector->getElementType();
      if (step) {
        summary.offset += value * static_cast<std::int64_t>(
            layout.getTypeAllocSize(step).getFixedValue());
      } else {
        summary.constant_offset = false;
      }

      ::llvm::Type* ty = flat_type;
      if (auto* array = ::llvm::dyn_cast<::llvm::ArrayType>(ty)) {
        flat_type = array->getElementType();
      } else if (auto* vector = ::llvm::dyn_cast<::llvm::VectorType>(ty)) {
        // The reference keeps the vector; its next indexed type is the
        // element type (vectors are bit-packed, but this is a rare corner).
        flat_type = vector;
      } else if (auto* st = ::llvm::dyn_cast<::llvm::StructType>(ty)) {
        in_struct = true;
        struct_type = st;
        flat_type = nullptr;
      } else {
        // Scalar / opaque pointer: a further index would be invalid IR.
        flat_type = nullptr;
      }
    }
  }
  return summary;
}

semantic::AbstractObjectKind KindForBase(const ::llvm::Value& base) {
  if (::llvm::isa<::llvm::GlobalVariable>(base))
    return semantic::AbstractObjectKind::kGlobal;
  if (::llvm::isa<::llvm::AllocaInst>(base))
    return semantic::AbstractObjectKind::kStack;
  if (::llvm::isa<::llvm::Argument>(base))
    return semantic::AbstractObjectKind::kArgument;
  if (::llvm::isa<::llvm::Function>(base))
    return semantic::AbstractObjectKind::kFunction;
  return semantic::AbstractObjectKind::kUnknown;
}

const ::llvm::Function* OwnerFunctionOf(const ::llvm::Value& base) {
  if (const auto* inst = ::llvm::dyn_cast<::llvm::Instruction>(&base))
    return inst->getFunction();
  if (const auto* arg = ::llvm::dyn_cast<::llvm::Argument>(&base))
    return arg->getParent();
  return nullptr;
}

std::optional<std::uint64_t> AllocationSizeFor(const ::llvm::Value& base,
                                               const ::llvm::DataLayout& layout) {
  if (const auto* alloca = ::llvm::dyn_cast<::llvm::AllocaInst>(&base))
    return layout.getTypeAllocSize(alloca->getAllocatedType()).getFixedValue();
  if (const auto* gv = ::llvm::dyn_cast<::llvm::GlobalVariable>(&base))
    return layout.getTypeAllocSize(gv->getValueType()).getFixedValue();
  return std::nullopt;
}

std::string SemanticAnchorFor(semantic::AbstractObjectKind kind) {
  switch (kind) {
    case semantic::AbstractObjectKind::kGlobal:
      return "global";
    case semantic::AbstractObjectKind::kStack:
      return "stack";
    case semantic::AbstractObjectKind::kHeap:
      return "heap";
    case semantic::AbstractObjectKind::kArgument:
      return "argument";
    case semantic::AbstractObjectKind::kFunction:
      return "function";
    case semantic::AbstractObjectKind::kExternal:
      return "external";
    case semantic::AbstractObjectKind::kUnknown:
      return "unknown";
    case semantic::AbstractObjectKind::kLegacyOpaque:
      return "legacy-opaque";
  }
  return "unknown";
}

}  // namespace

AbstractMemoryBuilder::AbstractMemoryBuilder(const ::llvm::DataLayout& layout,
                                             const StableValueMapper& values,
                                             const OriginMap& origins)
    : layout_(layout), values_(values), origins_(origins) {}

StatusOr<semantic::MemoryLocation> AbstractMemoryBuilder::LocationFor(
    const ::llvm::Value& pointer, std::optional<std::uint64_t> access_size) const {
  // 1. Collect the GEP chain (outermost first) and locate the base object.
  std::vector<const ::llvm::GEPOperator*> geps;
  const ::llvm::Value* base = &pointer;
  while (true) {
    if (const auto* gep = ::llvm::dyn_cast<::llvm::GEPOperator>(base)) {
      geps.push_back(gep);
      base = gep->getPointerOperand();
    } else if (const auto* bitcast = ::llvm::dyn_cast<::llvm::BitCastOperator>(base)) {
      base = bitcast->getOperand(0);
    } else if (const auto* addrspace =
                   ::llvm::dyn_cast<::llvm::AddrSpaceCastOperator>(base)) {
      base = addrspace->getOperand(0);
    } else {
      break;
    }
  }

  // 2. Determine the abstract object kind and base value fingerprint.
  const semantic::AbstractObjectKind kind = KindForBase(*base);
  auto base_id = values_.IdFor(*base);
  if (!base_id.ok())
    return base_id.status();

  // 3. Resolve the owner function variant (raw text and typed StableId).
  const ::llvm::Function* owner_function_ptr = OwnerFunctionOf(*base);
  std::string owner_variant_text;
  std::optional<core::StableId> owner_function;
  if (owner_function_ptr) {
    owner_variant_text =
        origins_.GetSymbolId(owner_function_ptr).value_or(
            owner_function_ptr->getName().str());
    const auto parsed = core::ParseStableId(owner_variant_text);
    if (parsed.ok() && parsed->kind == core::IdKind::kFunctionVariant)
      owner_function = *parsed;
  }

  // 4. Build the abstract object (kAbstractObject) identity from the owner,
  // kind, allocation-instruction fingerprint, and allocation size.
  std::string object_bytes;
  AppendTag(&object_bytes, 'O');
  AppendString(&object_bytes, owner_variant_text);
  AppendInt(&object_bytes, static_cast<std::int64_t>(kind));
  AppendString(&object_bytes, core::ToString(*base_id));
  if (const auto alloc_size = AllocationSizeFor(*base, layout_)) {
    AppendTag(&object_bytes, 'S');
    AppendUInt(&object_bytes, *alloc_size);
  } else {
    AppendTag(&object_bytes, 'N');
  }

  semantic::AbstractObject object;
  object.id = core::MakeStableId(
      core::IdKind::kAbstractObject,
      std::as_bytes(std::span(object_bytes.data(), object_bytes.size())));
  object.kind = kind;
  object.owner_function = owner_function;
  object.semantic_anchor = SemanticAnchorFor(kind);
  object.diagnostic_name =
      base->hasName() ? base->getName().str() : std::string{};

  // 5. Reconstruct the access path and byte range from the GEP chain, base to
  // pointer.
  std::vector<semantic::AccessPathSegment> access_path;
  std::int64_t total_offset = 0;
  bool known_offset = true;
  for (auto it = geps.rbegin(); it != geps.rend(); ++it) {
    GepSummary summary = SummarizeGep(**it, layout_);
    access_path.insert(access_path.end(), summary.segments.begin(),
                       summary.segments.end());
    if (!summary.constant_offset)
      known_offset = false;
    else
      total_offset += summary.offset;
  }

  semantic::ByteRange byte_range;
  if (known_offset && access_size.has_value())
    byte_range = semantic::ByteRange::Known(total_offset, *access_size);
  else
    byte_range = semantic::ByteRange::Unknown();

  // 6. Build the memory location (kMemoryRef) identity from the object and the
  // access path only. The byte range is deliberately excluded: a constant byte
  // offset is already a function of the access path, and the access size
  // describes the accessing instruction rather than the object. Folding the
  // range into identity split one object into a known-range identity (local
  // extraction, which knows the load/store size) and an unknown-range identity
  // (SVF, which does not), and split MayWrite by offset. The range is still
  // carried on the location and hashed independently by ComponentHash.
  std::string location_bytes;
  AppendTag(&location_bytes, 'M');
  AppendString(&location_bytes, core::ToString(object.id));
  AppendUInt(&location_bytes, access_path.size());
  for (const auto& segment : access_path) {
    AppendInt(&location_bytes, static_cast<std::int64_t>(segment.kind));
    AppendInt(&location_bytes, segment.first);
    AppendInt(&location_bytes, segment.last);
  }

  semantic::MemoryLocation location;
  location.id = core::MakeStableId(
      core::IdKind::kMemoryRef,
      std::as_bytes(std::span(location_bytes.data(), location_bytes.size())));
  location.object = std::move(object);
  location.access_path = std::move(access_path);
  location.byte_range = byte_range;
  return location;
}

}  // namespace veritas::analysis::llvm
