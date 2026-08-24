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

#include "analysis/llvm/LocalFactExtractor.h"

#include <optional>
#include <span>
#include <string>

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include "analysis/llvm/AbstractMemoryBuilder.h"
#include "analysis/llvm/MemoryAccessExtractor.h"
#include "analysis/llvm/OriginMap.h"
#include "analysis/llvm/StableValueMapper.h"
#include "analysis/llvm/ValueFlowExtractor.h"
#include "analysis/pipeline/ProgramIr.h"
#include "veritas/core/Ids.h"

namespace veritas::analysis::llvm {
namespace {

namespace v1 = veritas::summary::v1;
namespace semantic = veritas::analysis::semantic;

v1::EffectKind ToEffectKind(MemoryAccessExtractor::AccessKind kind) {
  switch (kind) {
  case MemoryAccessExtractor::AccessKind::kRead:
    return v1::EFFECT_KIND_READ;
  case MemoryAccessExtractor::AccessKind::kWrite:
    return v1::EFFECT_KIND_WRITE;
  case MemoryAccessExtractor::AccessKind::kReadWrite:
    return v1::EFFECT_KIND_UNKNOWN;
  }
  return v1::EFFECT_KIND_UNKNOWN;
}

// Emit a direct call, or an unknown call plus an explicit UnknownFact for an
// unresolved indirect target. Local facts never expand callees.
void ExtractCalls(const ::llvm::Function &function, const OriginMap &origin_map,
                  summary::FunctionLocalFacts *facts) {
  std::size_t call_ordinal = 0;
  for (const auto &block : function) {
    for (const auto &inst : block) {
      const auto *call = ::llvm::dyn_cast<::llvm::CallBase>(&inst);
      if (!call)
        continue;

      v1::Call fact;
      std::string call_site_key = facts->function_variant_id;
      call_site_key.push_back('\0');
      call_site_key.append(std::to_string(call_ordinal++));
      fact.set_call_site_anchor_id(core::ToString(
          core::MakeStableId(core::IdKind::kCallSite,
                             std::as_bytes(std::span(call_site_key.data(),
                                                     call_site_key.size())))));
      if (const auto *callee = call->getCalledFunction()) {
        fact.set_callee_symbol(callee->getName().str());
        fact.set_epistemic(v1::EPISTEMIC_STATE_MUST);
        if (auto id = origin_map.GetSymbolId(callee)) {
          fact.set_resolved_callee_function_variant_id(*id);
        }
      } else {
        fact.set_callee_symbol("<unknown>");
        fact.set_epistemic(v1::EPISTEMIC_STATE_UNKNOWN);
        v1::Unknown unknown;
        unknown.set_kind("unresolved_call");
        unknown.set_reason("indirect call target not resolvable locally");
        unknown.set_scope(function.getName().str());
        facts->unknowns.push_back(std::move(unknown));
      }
      facts->calls.push_back(std::move(fact));
    }
  }
}

void ExtractMemoryEffects(const ::llvm::Function &function,
                          const MemoryAccessExtractor &extractor,
                          summary::FunctionLocalFacts *facts) {
  for (const auto &access : extractor.ExtractMemoryAccesses(&function)) {
    v1::MemoryEffect effect;
    effect.set_kind(ToEffectKind(access.kind));
    effect.set_location(access.location);
    effect.set_epistemic(v1::EPISTEMIC_STATE_MUST);
    facts->memory_effects.push_back(std::move(effect));
  }
}

void ExtractValueFlows(const ::llvm::Function &function,
                       const ValueFlowExtractor &extractor,
                       summary::FunctionLocalFacts *facts) {
  for (const auto &flow : extractor.ExtractValueFlows(&function)) {
    v1::ValueFlow fact;
    fact.set_source(flow.source);
    fact.set_sink(flow.destination);
    fact.set_epistemic(v1::EPISTEMIC_STATE_MUST);
    facts->value_flows.push_back(std::move(fact));
  }
}

void ExtractUnknowns(const ::llvm::Function &function,
                     summary::FunctionLocalFacts *facts) {
  for (const auto &block : function) {
    for (const auto &inst : block) {
      const auto *call = ::llvm::dyn_cast<::llvm::CallBase>(&inst);
      if (!call)
        continue;
      if (call->isInlineAsm() || ::llvm::isa<::llvm::CallBrInst>(&inst)) {
        v1::Unknown unknown;
        unknown.set_kind("unsupported_construct");
        unknown.set_reason("inline assembly / callbr");
        unknown.set_scope(function.getName().str());
        facts->unknowns.push_back(std::move(unknown));
      }
    }
  }
}

// ---- V2 extraction helpers (typed facts via StableValueMapper /
//      AbstractMemoryBuilder) ----

Status ExtractCallsV2(const ::llvm::Function &function,
                      const OriginMap &origin_map,
                      const StableValueMapper &values,
                      summary::FunctionLocalFactsV2 *facts) {
  for (const auto &block : function) {
    for (const auto &inst : block) {
      const auto *call = ::llvm::dyn_cast<::llvm::CallBase>(&inst);
      if (!call)
        continue;

      summary::CallFactV2 fact;
      auto call_site = values.CallSiteIdFor(*call);
      if (!call_site.ok())
        return call_site.status();
      fact.call_site_id = *call_site;

      if (const auto *callee = call->getCalledFunction()) {
        fact.callee_symbol = callee->getName().str();
        fact.dispatch = semantic::DispatchKind::kDirect;
        fact.epistemic = semantic::EpistemicState::kMust;
        if (auto id = origin_map.GetSymbolId(callee)) {
          fact.resolved_callee_function_variant_id = *id;
        }
      } else {
        fact.callee_symbol = "<unknown>";
        fact.dispatch = semantic::DispatchKind::kIndirect;
        fact.epistemic = semantic::EpistemicState::kUnknown;
        v1::Unknown unknown;
        unknown.set_kind("unresolved_call");
        unknown.set_reason("indirect call target not resolvable locally");
        unknown.set_scope(function.getName().str());
        facts->unknowns.push_back(std::move(unknown));
      }
      facts->calls.push_back(std::move(fact));
    }
  }
  return Status::Ok();
}

Status ExtractMemoryEffectsV2(const ::llvm::Function &function,
                              const ::llvm::DataLayout &layout,
                              const AbstractMemoryBuilder &builder,
                              summary::FunctionLocalFactsV2 *facts) {
  for (const auto &block : function) {
    for (const auto &inst : block) {
      const ::llvm::Value *pointer = nullptr;
      v1::EffectKind kind = v1::EFFECT_KIND_UNKNOWN;
      std::optional<std::uint64_t> access_size;

      if (const auto *load = ::llvm::dyn_cast<::llvm::LoadInst>(&inst)) {
        kind = v1::EFFECT_KIND_READ;
        pointer = load->getPointerOperand();
        access_size =
            layout.getTypeStoreSize(load->getType()).getFixedValue();
      } else if (const auto *store = ::llvm::dyn_cast<::llvm::StoreInst>(&inst)) {
        kind = v1::EFFECT_KIND_WRITE;
        pointer = store->getPointerOperand();
        access_size =
            layout.getTypeStoreSize(store->getValueOperand()->getType())
                .getFixedValue();
      } else if (const auto *rmw = ::llvm::dyn_cast<::llvm::AtomicRMWInst>(&inst)) {
        kind = v1::EFFECT_KIND_UNKNOWN;
        pointer = rmw->getPointerOperand();
        access_size =
            layout.getTypeStoreSize(rmw->getValOperand()->getType())
                .getFixedValue();
      } else if (const auto *cmpxchg =
                     ::llvm::dyn_cast<::llvm::AtomicCmpXchgInst>(&inst)) {
        kind = v1::EFFECT_KIND_UNKNOWN;
        pointer = cmpxchg->getPointerOperand();
        access_size =
            layout.getTypeStoreSize(cmpxchg->getCompareOperand()->getType())
                .getFixedValue();
      } else if (::llvm::isa<::llvm::CallBase>(inst)) {
        // Conservative: calls may read and write memory. The call instruction
        // itself is the base object (no pointer operand).
        kind = v1::EFFECT_KIND_UNKNOWN;
        pointer = &inst;
      } else {
        continue;
      }

      auto location = builder.LocationFor(*pointer, access_size);
      if (!location.ok())
        return location.status();

      summary::MemoryEffectFactV2 fact;
      fact.kind = kind;
      fact.location = std::move(location).value();
      fact.epistemic = semantic::EpistemicState::kMust;
      facts->memory_effects.push_back(std::move(fact));
    }
  }
  return Status::Ok();
}

Status ExtractValueFlowsV2(const ::llvm::Function &function,
                           const StableValueMapper &values,
                           summary::FunctionLocalFactsV2 *facts) {
  for (const auto &block : function) {
    for (const auto &inst : block) {
      for (unsigned i = 0; i < inst.getNumOperands(); ++i) {
        const auto *operand = inst.getOperand(i);
        if (!::llvm::isa<::llvm::Instruction>(operand) &&
            !::llvm::isa<::llvm::Argument>(operand)) {
          continue;
        }
        auto source = values.IdFor(*operand);
        if (!source.ok())
          return source.status();
        auto destination = values.IdFor(inst);
        if (!destination.ok())
          return destination.status();

        summary::ValueFlowFactV2 fact;
        fact.source_value_id = *source;
        fact.destination_value_id = *destination;
        fact.epistemic = semantic::EpistemicState::kMust;
        facts->value_flows.push_back(std::move(fact));
      }
    }
  }
  return Status::Ok();
}

void ExtractUnknownsV2(const ::llvm::Function &function,
                       summary::FunctionLocalFactsV2 *facts) {
  for (const auto &block : function) {
    for (const auto &inst : block) {
      const auto *call = ::llvm::dyn_cast<::llvm::CallBase>(&inst);
      if (!call)
        continue;
      if (call->isInlineAsm() || ::llvm::isa<::llvm::CallBrInst>(&inst)) {
        v1::Unknown unknown;
        unknown.set_kind("unsupported_construct");
        unknown.set_reason("inline assembly / callbr");
        unknown.set_scope(function.getName().str());
        facts->unknowns.push_back(std::move(unknown));
      }
    }
  }
}

} // namespace

veritas::StatusOr<std::vector<summary::FunctionLocalFacts>>
LocalFactExtractor::Extract(pipeline::ProgramIr &program_ir) const {
  auto *module = program_ir.GetModule();
  if (!module) {
    return veritas::Status::FailedPrecondition("ProgramIr has no module");
  }

  const OriginMap &origin_map = program_ir.origin_map();
  const MemoryAccessExtractor memory_extractor;
  const ValueFlowExtractor flow_extractor;

  std::vector<summary::FunctionLocalFacts> output;
  output.reserve(module->size());

  for (const auto &function : *module) {
    if (function.isDeclaration())
      continue;

    summary::FunctionLocalFacts facts;
    facts.function_symbol_id =
        origin_map.GetSymbolId(&function).value_or(function.getName().str());
    // V1: no template/overload specialization identity in the origin map yet,
    // so the function variant is the function symbol.
    facts.function_variant_id = facts.function_symbol_id;

    ExtractCalls(function, origin_map, &facts);
    ExtractMemoryEffects(function, memory_extractor, &facts);
    ExtractValueFlows(function, flow_extractor, &facts);
    ExtractUnknowns(function, &facts);

    output.push_back(std::move(facts));
  }

  return output;
}

veritas::StatusOr<std::vector<summary::FunctionLocalFactsV2>>
LocalFactExtractor::ExtractV2(pipeline::ProgramIr &program_ir) const {
  auto *module = program_ir.GetModule();
  if (!module) {
    return veritas::Status::FailedPrecondition("ProgramIr has no module");
  }

  const OriginMap &origin_map = program_ir.origin_map();
  const StableValueMapper values(*module, origin_map);
  const ::llvm::DataLayout &layout = module->getDataLayout();
  const AbstractMemoryBuilder memory_builder(layout, values, origin_map);

  std::vector<summary::FunctionLocalFactsV2> output;
  output.reserve(module->size());

  for (const auto &function : *module) {
    if (function.isDeclaration())
      continue;

    summary::FunctionLocalFactsV2 facts;
    facts.function_symbol_id =
        origin_map.GetSymbolId(&function).value_or(function.getName().str());
    facts.function_variant_id = facts.function_symbol_id;

    Status status = ExtractCallsV2(function, origin_map, values, &facts);
    if (!status.ok())
      return status;
    status = ExtractMemoryEffectsV2(function, layout, memory_builder, &facts);
    if (!status.ok())
      return status;
    status = ExtractValueFlowsV2(function, values, &facts);
    if (!status.ok())
      return status;
    ExtractUnknownsV2(function, &facts);

    output.push_back(std::move(facts));
  }

  return output;
}

} // namespace veritas::analysis::llvm
