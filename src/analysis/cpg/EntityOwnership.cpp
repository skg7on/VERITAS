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

#include "analysis/cpg/EntityOwnership.h"

#include <llvm/Analysis/CaptureTracking.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <map>

#include "analysis/llvm/AbstractMemoryBuilder.h"
#include "analysis/llvm/StableValueMapper.h"
#include "veritas/core/Hash.h"

namespace veritas::analysis::cpg {

StatusOr<std::vector<EntityOwner>>
CollectEntityOwners(const pipeline::ProgramIr &ir) {
  const auto *module = ir.GetModule();
  if (!module)
    return Status::FailedPrecondition("ProgramIr has no module");
  llvm::StableValueMapper values(*module, ir.origin_map());
  llvm::AbstractMemoryBuilder memory(module->getDataLayout(), values,
                                     ir.origin_map());
  std::map<core::StableId, core::StableId> owners;
  std::map<core::StableId, const ::llvm::Function *> functions;
  for (const auto &function : *module) {
    if (function.isDeclaration())
      continue;
    auto origin = ir.origin_map().GetSymbolId(&function);
    if (!origin)
      continue; // synthetic/imported definitions have no owner
    auto owner = core::ParseStableId(*origin);
    if (!owner.ok() || owner->kind != core::IdKind::kFunctionVariant ||
        !core::HexToDigest(owner->digest_hex)) {
      return Status::InvalidArgument("invalid compiler function owner");
    }
    if (!functions.emplace(*owner, &function).second) {
      return Status::FailedPrecondition("conflicting compiler function owners");
    }
    const auto add = [&](StatusOr<core::StableId> entity) -> Status {
      if (!entity.ok())
        return entity.status();
      auto [it, inserted] = owners.emplace(*entity, *owner);
      if (!inserted && it->second != *owner) {
        return Status::FailedPrecondition("conflicting compiler entity owners");
      }
      return Status::Ok();
    };
    const auto add_memory = [&](const ::llvm::Value &value) -> Status {
      if (!value.getType()->isPointerTy())
        return Status::Ok();
      const auto *base = ::llvm::getUnderlyingObject(&value);
      if (!::llvm::isa<::llvm::AllocaInst>(base) ||
          ::llvm::PointerMayBeCaptured(base, true))
        return Status::Ok();
      auto location = memory.LocationFor(value, std::nullopt);
      if (!location.ok()) {
        return location.status().code() == StatusCode::kFailedPrecondition
                   ? Status::Ok()
                   : location.status();
      }
      // RecoverSingleStoreSlotValue may find a foreign pointee. The stack slot
      // used to load that pointer does not establish ownership of its pointee.
      if (location->object.kind != semantic::AbstractObjectKind::kStack ||
          location->object.owner_function != *owner)
        return Status::Ok();
      return add(location->id);
    };
    for (const auto &arg : function.args()) {
      auto status = add(values.IdFor(arg));
      if (!status.ok())
        return status;
      status = add_memory(arg);
      if (!status.ok())
        return status;
    }
    for (const auto &block : function) {
      for (const auto &inst : block) {
        auto status = add(values.IdFor(inst));
        if (!status.ok())
          return status;
        status = add_memory(inst);
        if (!status.ok())
          return status;
        if (const auto *call = ::llvm::dyn_cast<::llvm::CallBase>(&inst)) {
          status = add(values.CallSiteIdFor(*call));
          if (!status.ok())
            return status;
        }
      }
    }
  }
  std::vector<EntityOwner> result;
  result.reserve(owners.size());
  for (const auto &[entity, owner] : owners)
    result.push_back({entity, owner});
  return result;
}

} // namespace veritas::analysis::cpg
