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

#include "analysis/llvm/StableValueMapper.h"

#include <span>
#include <string>
#include <string_view>

#include <llvm/ADT/SmallString.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include "analysis/llvm/OriginMap.h"

namespace veritas::analysis::llvm {
namespace {

// Canonical byte encoding. Every string field is length-prefixed and every
// scalar is a decimal string terminated by ';', so the concatenation of fields
// in a fixed order is injective (no two distinct semantic structures produce
// the same byte string) and endian-neutral.
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

// Recursive canonical type serialization. Deterministic for a module; the
// structural payload is keyed on the TypeID so distinct type kinds can never
// collide.
void AppendTypeFingerprint(std::string* out, const ::llvm::Type* type) {
  if (!type) {
    AppendTag(out, '?');
    return;
  }
  AppendTag(out, 'T');
  AppendUInt(out, static_cast<std::uint64_t>(type->getTypeID()));
  switch (type->getTypeID()) {
    case ::llvm::Type::VoidTyID:
    case ::llvm::Type::HalfTyID:
    case ::llvm::Type::BFloatTyID:
    case ::llvm::Type::FloatTyID:
    case ::llvm::Type::DoubleTyID:
    case ::llvm::Type::X86_FP80TyID:
    case ::llvm::Type::FP128TyID:
    case ::llvm::Type::PPC_FP128TyID:
    case ::llvm::Type::LabelTyID:
    case ::llvm::Type::MetadataTyID:
    case ::llvm::Type::TokenTyID:
      break;
    case ::llvm::Type::IntegerTyID:
      AppendUInt(out,
                 ::llvm::cast<::llvm::IntegerType>(type)->getBitWidth());
      break;
    case ::llvm::Type::FunctionTyID: {
      const auto* ft = ::llvm::cast<::llvm::FunctionType>(type);
      AppendTypeFingerprint(out, ft->getReturnType());
      AppendUInt(out, ft->getNumParams());
      for (unsigned i = 0; i < ft->getNumParams(); ++i)
        AppendTypeFingerprint(out, ft->getParamType(i));
      AppendUInt(out, ft->isVarArg() ? 1u : 0u);
      break;
    }
    case ::llvm::Type::PointerTyID:
      AppendUInt(out, ::llvm::cast<::llvm::PointerType>(type)->getAddressSpace());
      break;
    case ::llvm::Type::StructTyID: {
      const auto* st = ::llvm::cast<::llvm::StructType>(type);
      if (st->hasName())
        AppendString(out, st->getName());
      AppendUInt(out, st->isPacked() ? 1u : 0u);
      AppendUInt(out, st->getNumElements());
      for (unsigned i = 0; i < st->getNumElements(); ++i)
        AppendTypeFingerprint(out, st->getElementType(i));
      break;
    }
    case ::llvm::Type::ArrayTyID:
      AppendUInt(out, ::llvm::cast<::llvm::ArrayType>(type)->getNumElements());
      AppendTypeFingerprint(out,
                            ::llvm::cast<::llvm::ArrayType>(type)->getElementType());
      break;
    case ::llvm::Type::FixedVectorTyID:
      AppendUInt(out,
                 ::llvm::cast<::llvm::FixedVectorType>(type)->getNumElements());
      AppendTypeFingerprint(
          out, ::llvm::cast<::llvm::FixedVectorType>(type)->getElementType());
      break;
    case ::llvm::Type::ScalableVectorTyID:
      AppendUInt(out,
                 ::llvm::cast<::llvm::ScalableVectorType>(type)->getMinNumElements());
      AppendTypeFingerprint(
          out, ::llvm::cast<::llvm::ScalableVectorType>(type)->getElementType());
      break;
    default:
      // Byte, AMX, target-extension, and other exotic types carry only their
      // TypeID. They never appear in local value-flow operands.
      break;
  }
}

constexpr std::uint32_t kMissingIndex = 0xFFFFFFFFu;

}  // namespace

StableValueMapper::StableValueMapper(const ::llvm::Module& module,
                                     const OriginMap& origin_map) {
  for (const auto& function : module) {
    owner_variants_[&function] =
        origin_map.GetSymbolId(&function).value_or(function.getName().str());
    std::uint32_t block_index = 0;
    for (const auto& block : function) {
      block_indices_[&block] = block_index;
      std::uint32_t inst_index = 0;
      for (const auto& inst : block) {
        inst_indices_[&inst] = inst_index++;
      }
      ++block_index;
    }
  }
}

StatusOr<core::StableId> StableValueMapper::IdFor(
    const ::llvm::Value& value) const {
  std::string bytes;
  AppendTag(&bytes, 'V');
  AppendValue(&bytes, value);
  return core::MakeStableId(core::IdKind::kValueRef,
                            std::as_bytes(std::span(bytes.data(), bytes.size())));
}

StatusOr<core::StableId> StableValueMapper::CallSiteIdFor(
    const ::llvm::CallBase& call) const {
  std::string bytes;
  AppendTag(&bytes, 'S');
  AppendOwnerVariant(&bytes, call.getFunction());
  AppendUInt(&bytes, BlockIndexOf(&call));
  AppendUInt(&bytes, InstIndexOf(&call));
  return core::MakeStableId(core::IdKind::kCallSite,
                            std::as_bytes(std::span(bytes.data(), bytes.size())));
}

void StableValueMapper::AppendValue(std::string* out,
                                    const ::llvm::Value& value) const {
  if (const auto* arg = ::llvm::dyn_cast<::llvm::Argument>(&value)) {
    AppendArgument(out, *arg);
  } else if (const auto* inst = ::llvm::dyn_cast<::llvm::Instruction>(&value)) {
    AppendInstruction(out, *inst);
  } else if (const auto* constant = ::llvm::dyn_cast<::llvm::Constant>(&value)) {
    AppendConstant(out, *constant);
  } else if (const auto* asm_value = ::llvm::dyn_cast<::llvm::InlineAsm>(&value)) {
    AppendInlineAsm(out, *asm_value);
  } else if (const auto* basic_block = ::llvm::dyn_cast<::llvm::BasicBlock>(&value)) {
    AppendTag(out, 'b');
    AppendOwnerVariant(out, basic_block->getParent());
    AppendUInt(out, BlockIndexOf(basic_block));
  } else if (::llvm::isa<::llvm::MetadataAsValue>(value)) {
    AppendTag(out, 'm');
    AppendTypeFingerprint(out, value.getType());
  } else {
    AppendTag(out, 'o');
    AppendTypeFingerprint(out, value.getType());
  }
}

void StableValueMapper::AppendInstruction(std::string* out,
                                          const ::llvm::Instruction& inst) const {
  AppendTag(out, 'I');
  AppendOwnerVariant(out, inst.getFunction());
  AppendUInt(out, inst.getOpcode());
  AppendTypeFingerprint(out, inst.getType());
  AppendUInt(out, inst.getNumOperands());
  for (unsigned i = 0; i < inst.getNumOperands(); ++i)
    AppendOperand(out, *inst.getOperand(i));

  // Stable source anchor, when present.
  if (const auto& loc = inst.getDebugLoc()) {
    AppendTag(out, 'D');
    AppendString(out, loc->getScope()->getFilename());
    AppendUInt(out, loc.getLine());
    AppendUInt(out, loc.getCol());
  }

  // Deterministic block-local structural index.
  AppendUInt(out, BlockIndexOf(&inst));
  AppendUInt(out, InstIndexOf(&inst));
}

void StableValueMapper::AppendArgument(std::string* out,
                                       const ::llvm::Argument& arg) const {
  AppendTag(out, 'A');
  AppendOwnerVariant(out, arg.getParent());
  AppendUInt(out, arg.getArgNo());
  AppendTypeFingerprint(out, arg.getType());
}

void StableValueMapper::AppendGlobal(std::string* out,
                                     const ::llvm::GlobalVariable& gv) const {
  AppendTag(out, 'G');
  AppendString(out, gv.getName());
  AppendUInt(out, static_cast<std::uint64_t>(gv.getLinkage()));
  AppendTypeFingerprint(out, gv.getValueType());
}

void StableValueMapper::AppendFunction(std::string* out,
                                       const ::llvm::Function& fn) const {
  AppendTag(out, 'f');
  AppendOwnerVariant(out, &fn);
}

void StableValueMapper::AppendConstant(std::string* out,
                                       const ::llvm::Constant& c) const {
  if (const auto* ci = ::llvm::dyn_cast<::llvm::ConstantInt>(&c)) {
    AppendTag(out, 'I');
    ::llvm::SmallString<32> buffer;
    ci->getValue().toString(buffer, 10, false);
    AppendString(out, buffer.str());
    AppendTypeFingerprint(out, ci->getType());
  } else if (const auto* cf = ::llvm::dyn_cast<::llvm::ConstantFP>(&c)) {
    AppendTag(out, 'F');
    ::llvm::SmallString<32> buffer;
    cf->getValueAPF().bitcastToAPInt().toString(buffer, 10, false);
    AppendString(out, buffer.str());
    AppendTypeFingerprint(out, cf->getType());
  } else if (::llvm::isa<::llvm::ConstantPointerNull>(c)) {
    AppendTag(out, 'N');
    AppendTypeFingerprint(out, c.getType());
  } else if (const auto* poison = ::llvm::dyn_cast<::llvm::PoisonValue>(&c)) {
    AppendTag(out, 'P');
    AppendTypeFingerprint(out, poison->getType());
  } else if (const auto* undef = ::llvm::dyn_cast<::llvm::UndefValue>(&c)) {
    AppendTag(out, 'U');
    AppendTypeFingerprint(out, undef->getType());
  } else if (const auto* zero = ::llvm::dyn_cast<::llvm::ConstantAggregateZero>(&c)) {
    AppendTag(out, 'Z');
    AppendTypeFingerprint(out, zero->getType());
  } else if (const auto* address = ::llvm::dyn_cast<::llvm::BlockAddress>(&c)) {
    AppendBlockAddress(out, *address);
  } else if (const auto* expr = ::llvm::dyn_cast<::llvm::ConstantExpr>(&c)) {
    AppendTag(out, 'E');
    AppendUInt(out, expr->getOpcode());
    AppendTypeFingerprint(out, expr->getType());
    AppendUInt(out, expr->getNumOperands());
    for (unsigned i = 0; i < expr->getNumOperands(); ++i)
      AppendConstant(out, *::llvm::cast<::llvm::Constant>(expr->getOperand(i)));
  } else if (const auto* gv = ::llvm::dyn_cast<::llvm::GlobalVariable>(&c)) {
    AppendGlobal(out, *gv);
  } else if (const auto* fn = ::llvm::dyn_cast<::llvm::Function>(&c)) {
    AppendFunction(out, *fn);
  } else if (const auto* data =
                 ::llvm::dyn_cast<::llvm::ConstantDataSequential>(&c)) {
    AppendTag(out, 'D');
    AppendTypeFingerprint(out, data->getType());
    AppendString(out, data->getRawDataValues());
  } else if (::llvm::isa<::llvm::ConstantData>(c)) {
    // ConstantTokenNone and friends: no raw payload, only kind + type.
    AppendTag(out, 'T');
    AppendTypeFingerprint(out, c.getType());
  } else if (const auto* aggregate = ::llvm::dyn_cast<::llvm::ConstantAggregate>(&c)) {
    AppendTag(out, 'A');
    AppendTypeFingerprint(out, aggregate->getType());
    AppendUInt(out, aggregate->getNumOperands());
    for (unsigned i = 0; i < aggregate->getNumOperands(); ++i)
      AppendConstant(out, *::llvm::cast<::llvm::Constant>(aggregate->getOperand(i)));
  } else {
    AppendTag(out, 'O');
    AppendTypeFingerprint(out, c.getType());
  }
}

void StableValueMapper::AppendOperand(std::string* out,
                                      const ::llvm::Value& operand) const {
  if (const auto* inst = ::llvm::dyn_cast<::llvm::Instruction>(&operand)) {
    AppendTag(out, 'i');
    AppendOwnerVariant(out, inst->getFunction());
    AppendUInt(out, BlockIndexOf(inst));
    AppendUInt(out, InstIndexOf(inst));
  } else if (const auto* arg = ::llvm::dyn_cast<::llvm::Argument>(&operand)) {
    AppendTag(out, 'a');
    AppendOwnerVariant(out, arg->getParent());
    AppendUInt(out, arg->getArgNo());
  } else if (const auto* block = ::llvm::dyn_cast<::llvm::BasicBlock>(&operand)) {
    AppendTag(out, 'b');
    AppendOwnerVariant(out, block->getParent());
    AppendUInt(out, BlockIndexOf(block));
  } else if (const auto* constant = ::llvm::dyn_cast<::llvm::Constant>(&operand)) {
    AppendTag(out, 'c');
    AppendConstant(out, *constant);
  } else if (const auto* asm_value = ::llvm::dyn_cast<::llvm::InlineAsm>(&operand)) {
    AppendTag(out, 'l');
    AppendInlineAsm(out, *asm_value);
  } else if (::llvm::isa<::llvm::MetadataAsValue>(operand)) {
    AppendTag(out, 'm');
    AppendTypeFingerprint(out, operand.getType());
  } else {
    AppendTag(out, 'o');
    AppendTypeFingerprint(out, operand.getType());
  }
}

void StableValueMapper::AppendBlockAddress(
    std::string* out, const ::llvm::BlockAddress& address) const {
  AppendTag(out, 'B');
  AppendOwnerVariant(out, address.getFunction());
  AppendUInt(out, BlockIndexOf(address.getBasicBlock()));
}

void StableValueMapper::AppendInlineAsm(std::string* out,
                                        const ::llvm::InlineAsm& asm_value) const {
  AppendTag(out, 'L');
  AppendString(out, asm_value.getAsmString());
  AppendTypeFingerprint(out, asm_value.getFunctionType());
}

void StableValueMapper::AppendOwnerVariant(std::string* out,
                                           const ::llvm::Function* fn) const {
  if (!fn) {
    AppendString(out, std::string_view{});
    return;
  }
  const auto it = owner_variants_.find(fn);
  if (it == owner_variants_.end()) {
    AppendString(out, fn->getName().str());
    return;
  }
  AppendString(out, it->second);
}

std::uint32_t StableValueMapper::BlockIndexOf(
    const ::llvm::Instruction* inst) const {
  return inst ? BlockIndexOf(inst->getParent()) : kMissingIndex;
}

std::uint32_t StableValueMapper::BlockIndexOf(
    const ::llvm::BasicBlock* block) const {
  if (!block)
    return kMissingIndex;
  const auto it = block_indices_.find(block);
  return it == block_indices_.end() ? kMissingIndex : it->second;
}

std::uint32_t StableValueMapper::InstIndexOf(
    const ::llvm::Instruction* inst) const {
  if (!inst)
    return kMissingIndex;
  const auto it = inst_indices_.find(inst);
  return it == inst_indices_.end() ? kMissingIndex : it->second;
}

}  // namespace veritas::analysis::llvm
