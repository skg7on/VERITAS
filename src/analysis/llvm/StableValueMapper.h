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

#ifndef VERITAS_ANALYSIS_LLVM_STABLEVALUEMAPPER_H_
#define VERITAS_ANALYSIS_LLVM_STABLEVALUEMAPPER_H_

#include <cstdint>
#include <string>
#include <unordered_map>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"

namespace llvm {
class Argument;
class BasicBlock;
class BlockAddress;
class CallBase;
class Constant;
class Function;
class GlobalVariable;
class InlineAsm;
class Instruction;
class Module;
class Value;
}  // namespace llvm

namespace veritas::analysis::llvm {

class OriginMap;

// StableValueMapper assigns deterministic, collision-free core::StableId
// identities (kValueRef) to LLVM values and (kCallSite) to call sites within a
// single module. Identity never depends on LLVM value names, %-ordinals, or
// pointer addresses: it is derived from the owner function variant, the value
// kind, the stable source anchor when present, a normalized
// opcode/type/operand fingerprint, and a deterministic block-local structural
// index. The owner function variant and structural indices are precomputed in
// the constructor, so no reference to the (possibly short-lived) OriginMap is
// retained.
class StableValueMapper {
 public:
  StableValueMapper(const ::llvm::Module& module, const OriginMap& origin_map);
  ~StableValueMapper() = default;

  // Deterministic kValueRef identity for an LLVM value in the module.
  StatusOr<core::StableId> IdFor(const ::llvm::Value& value) const;

  // Deterministic kCallSite identity for a call instruction.
  StatusOr<core::StableId> CallSiteIdFor(const ::llvm::CallBase& call) const;

 private:
  using OwnerMap =
      std::unordered_map<const ::llvm::Function*, std::string>;
  using BlockMap =
      std::unordered_map<const ::llvm::BasicBlock*, std::uint32_t>;
  using InstMap =
      std::unordered_map<const ::llvm::Instruction*, std::uint32_t>;

  void AppendValue(std::string* out, const ::llvm::Value& value) const;
  void AppendInstruction(std::string* out,
                         const ::llvm::Instruction& inst) const;
  void AppendArgument(std::string* out, const ::llvm::Argument& arg) const;
  void AppendGlobal(std::string* out, const ::llvm::GlobalVariable& gv) const;
  void AppendFunction(std::string* out, const ::llvm::Function& fn) const;
  void AppendConstant(std::string* out, const ::llvm::Constant& c) const;
  void AppendOperand(std::string* out, const ::llvm::Value& operand) const;
  void AppendBlockAddress(std::string* out,
                          const ::llvm::BlockAddress& address) const;
  void AppendInlineAsm(std::string* out,
                       const ::llvm::InlineAsm& asm_value) const;
  void AppendOwnerVariant(std::string* out, const ::llvm::Function* fn) const;

  std::uint32_t BlockIndexOf(const ::llvm::Instruction* inst) const;
  std::uint32_t BlockIndexOf(const ::llvm::BasicBlock* block) const;
  std::uint32_t InstIndexOf(const ::llvm::Instruction* inst) const;

  OwnerMap owner_variants_;
  BlockMap block_indices_;
  InstMap inst_indices_;
};

}  // namespace veritas::analysis::llvm

#endif  // VERITAS_ANALYSIS_LLVM_STABLEVALUEMAPPER_H_
