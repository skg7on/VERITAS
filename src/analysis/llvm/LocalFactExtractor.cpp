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

#include <span>
#include <string>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include "analysis/llvm/MemoryAccessExtractor.h"
#include "analysis/llvm/OriginMap.h"
#include "analysis/llvm/ValueFlowExtractor.h"
#include "analysis/pipeline/ProgramIr.h"
#include "veritas/core/Ids.h"

namespace veritas::analysis::llvm {
namespace {

namespace v1 = veritas::summary::v1;

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

} // namespace veritas::analysis::llvm
