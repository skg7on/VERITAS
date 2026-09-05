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

#include "SvfFactMapper.h"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "analysis/svf/SvfBudget.h"

#include <SVF-LLVM/LLVMModule.h>
#include <SVFIR/SVFIR.h>
#include <WPA/Andersen.h>
#include <Graphs/SVFG.h>
#include <Graphs/SVFGNode.h>
#include <Graphs/CallGraph.h>
#include <Util/Casting.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>

#include "analysis/llvm/AbstractMemoryBuilder.h"
#include "analysis/llvm/StableValueMapper.h"
#include "analysis/pipeline/ProgramIr.h"

namespace veritas::analysis::svf {
namespace {

namespace semantic = veritas::analysis::semantic;
using veritas::core::StableId;

// Resolve an SVF value to its underlying LLVM value, or nullptr when there is
// no underlying LLVM value (e.g. a pure memory-region node).
const ::llvm::Value* ResolveLlvmValue(const SVF::SVFValue* svf_value,
                                      const SvfSessionView& view) {
  if (!svf_value || !view.module_set) return nullptr;
  if (!view.module_set->hasLLVMValue(svf_value)) return nullptr;
  return view.module_set->getLLVMValue(svf_value);
}

// Resolve an SVF value to its stable kValueRef identity, or nullopt when it
// has no underlying LLVM value.
std::optional<StableId> ResolveValueId(const SVF::SVFValue* svf_value,
                                       const SvfSessionView& view,
                                       const llvm::StableValueMapper& values) {
  const ::llvm::Value* llvm_value = ResolveLlvmValue(svf_value, view);
  if (!llvm_value) return std::nullopt;
  auto id = values.IdFor(*llvm_value);
  if (!id.ok()) return std::nullopt;
  return *id;
}

// True when the SVF value resolves to an LLVM value of pointer type.
bool IsPointer(const SVF::SVFValue* var, const SvfSessionView& view) {
  const ::llvm::Value* llvm_value = ResolveLlvmValue(var, view);
  return llvm_value && llvm_value->getType()->isPointerTy();
}

// At -O0, Clang commonly spills a function-pointer parameter into a stack slot
// and reloads it before the indirect call. Recover the single stored value so
// callback classification keys off the stable origin, but stop as soon as the
// slot shape admits multiple or non-store writers.
const ::llvm::Value* RecoverSingleStoreSlotValue(const ::llvm::Value* value) {
  const auto* load = ::llvm::dyn_cast<::llvm::LoadInst>(value);
  if (!load) {
    return value;
  }
  const auto* slot = ::llvm::dyn_cast<::llvm::AllocaInst>(
      load->getPointerOperand()->stripPointerCasts());
  if (!slot) {
    return value;
  }

  const ::llvm::Value* stored = nullptr;
  for (const ::llvm::User* user : slot->users()) {
    if (const auto* slot_load = ::llvm::dyn_cast<::llvm::LoadInst>(user)) {
      if (slot_load->getPointerOperand()->stripPointerCasts() != slot) {
        return value;
      }
      continue;
    }
    const auto* store = ::llvm::dyn_cast<::llvm::StoreInst>(user);
    if (!store || store->getPointerOperand()->stripPointerCasts() != slot) {
      return value;
    }
    if (stored != nullptr) {
      return value;
    }
    stored = store->getValueOperand();
  }
  return stored != nullptr ? stored : value;
}

const ::llvm::DIType* StripDebugTypeAliases(const ::llvm::DIType* type) {
  while (const auto* derived =
             ::llvm::dyn_cast_or_null<::llvm::DIDerivedType>(type)) {
    switch (derived->getTag()) {
      case ::llvm::dwarf::DW_TAG_typedef:
      case ::llvm::dwarf::DW_TAG_const_type:
      case ::llvm::dwarf::DW_TAG_volatile_type:
      case ::llvm::dwarf::DW_TAG_restrict_type:
      case ::llvm::dwarf::DW_TAG_atomic_type:
        type = derived->getBaseType();
        continue;
      default:
        return type;
    }
  }
  return type;
}

bool IsFunctionPointerDebugType(const ::llvm::DIType* type) {
  type = StripDebugTypeAliases(type);
  const auto* pointer = ::llvm::dyn_cast_or_null<::llvm::DIDerivedType>(type);
  if (!pointer || pointer->getTag() != ::llvm::dwarf::DW_TAG_pointer_type) {
    return false;
  }
  return ::llvm::isa_and_nonnull<::llvm::DISubroutineType>(
      StripDebugTypeAliases(pointer->getBaseType()));
}

bool IsFunctionPointerFormal(const ::llvm::Argument& argument) {
  const ::llvm::DISubprogram* subprogram =
      argument.getParent()->getSubprogram();
  if (!subprogram || !subprogram->getType()) {
    return false;
  }
  const auto parameter_types = subprogram->getType()->getTypeArray();
  const unsigned parameter_index = argument.getArgNo() + 1;
  return parameter_index < parameter_types.size() &&
         IsFunctionPointerDebugType(parameter_types[parameter_index]);
}

// A callback is an indirect call whose stable LLVM origin is a formal whose
// source-level debug type is a pointer to a subroutine. Checking the declared
// formal type is required with opaque LLVM pointers: merely finding an
// Argument would also misclassify a data pointer explicitly cast at the call.
// Loads from globals, tables, and local forwarding slots intentionally remain
// ordinary indirect calls. Virtual-call classification is handled
// independently by CallICFGNode::isVirtualCall at the call site.
bool IsFormalFunctionPointerCall(const ::llvm::CallBase& call) {
  const ::llvm::Value* origin =
      RecoverSingleStoreSlotValue(call.getCalledOperand()->stripPointerCasts());
  origin = origin->stripPointerCasts();
  const auto* argument = ::llvm::dyn_cast<::llvm::Argument>(origin);
  return argument && argument->getParent() == call.getFunction() &&
         IsFunctionPointerFormal(*argument);
}

// An alias observation couples a semantic AliasKind with an independent
// epistemic state. MustAlias/NoAlias are definite (kMust); MayAlias is a
// may-result (kMay).
struct AliasObservation {
  semantic::AliasKind kind;
  semantic::EpistemicState epistemic;
};

std::string MakeProvenance(const AnalyzerRunContext& run_context,
                           const SvfConfig& config) {
  return "analyzer=" + run_context.analyzer_run_id +
         ";config=" + config.CanonicalAnalyzerConfig();
}

// A pointer admitted as an alias candidate, carrying its stable value identity
// and its resolved memory location beside the SVF var (for Andersen queries).
struct AliasPointer {
  const SVF::SVFVar* var;
  StableId value_id;  // kValueRef, the dedup/sort key
  semantic::MemoryLocation location;
};

// Andersen normally reports overlap as MAY even when both pointer values have
// the same one-element points-to set. That one resolved, non-black-hole
// object is a proof stronger than overlap alone: both values name the same
// abstract object in this analysis run. Empty, multi-object, and black-hole
// sets remain non-proofs and therefore cannot be promoted.
bool ResolvesToOneProvenObject(const AliasPointer& left,
                               const AliasPointer& right,
                               const SvfSessionView& view) {
  const SVF::PointsTo& left_points = view.andersen->getPts(left.var->getId());
  const SVF::PointsTo& right_points =
      view.andersen->getPts(right.var->getId());
  if (left_points.count() != 1 || right_points.count() != 1 ||
      left_points != right_points) {
    return false;
  }
  const SVF::NodeID object = *left_points.begin();
  return object != view.svf_ir->getBlackHoleNode();
}

// Resolve an SVF pointer var into an AliasPointer, or nullopt when it is not a
// pointer or cannot be mapped to a stable value/location identity.
std::optional<AliasPointer> ResolveAliasPointer(
    const SVF::SVFVar* var, const SvfSessionView& view,
    const llvm::StableValueMapper& values,
    const llvm::AbstractMemoryBuilder& memory) {
  const ::llvm::Value* llvm_value = ResolveLlvmValue(var, view);
  if (!llvm_value || !llvm_value->getType()->isPointerTy()) return std::nullopt;
  auto id = values.IdFor(*llvm_value);
  if (!id.ok()) return std::nullopt;
  auto location = memory.LocationFor(*llvm_value, std::nullopt);
  if (!location.ok()) return std::nullopt;
  return AliasPointer{var, *id, std::move(*location)};
}

// Two ascending-sorted NodeID sequences intersect iff they share an element.
// A two-pointer scan is O(|a| + |b|) and cache-friendly, replacing SVF's
// linked-list SparseBitVector intersection in the alias hot loop.
bool IntersectsSorted(const std::vector<SVF::NodeID>& a,
                      const std::vector<SVF::NodeID>& b) {
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < a.size() && j < b.size()) {
    if (a[i] < b[j]) {
      ++i;
    } else if (b[j] < a[i]) {
      ++j;
    } else {
      return true;
    }
  }
  return false;
}

// Sort + unique a vector of fully-orderable facts (operator<=>/==).
template <typename Vec>
void Canonicalize(Vec& facts) {
  std::sort(facts.begin(), facts.end());
  facts.erase(std::unique(facts.begin(), facts.end()), facts.end());
}

}  // namespace

Status MapSvfFacts(const pipeline::ProgramIr& program_ir,
                   const SvfSessionView& view,
                   const AnalyzerRunContext& run_context,
                   const SvfConfig& config,
                   SvfMappingResult* result) {
  if (!result) return Status::Internal("result is null");
  if (!view.svf_ir || !view.andersen || !view.svfg || !view.module_set) {
    return Status::Internal("incomplete SVF session view");
  }
  const ::llvm::Module* module = program_ir.GetModule();
  if (!module) return Status::Internal("ProgramIr has no module");

  llvm::StableValueMapper values(*module, program_ir.origin_map());
  llvm::AbstractMemoryBuilder memory(module->getDataLayout(), values,
                                     program_ir.origin_map());

  semantic::NormalizedAnalysisFacts facts;
  const std::string provenance = MakeProvenance(run_context, config);
  SvfBudget budget(config);
  bool truncated = false;
  int unmapped_count = 0;

  // Alias candidates, deduplicated by value identity. A std::map keyed by the
  // value id gives deterministic, insertion-independent uniqueness and order.
  std::map<StableId, AliasPointer> alias_pointers;

  // ---- Pass 1: value flows, memory effects, and alias pointers ----
  for (auto it = view.svfg->begin(); it != view.svfg->end(); ++it) {
    const SVF::SVFGNode* node = it->second;
    if (!node) continue;

    if (const auto* load = SVF::SVFUtil::dyn_cast<SVF::LoadVFGNode>(node)) {
      const SVF::SVFVar* pointer = load->getSrcNode();
      auto location = ResolveAliasPointer(pointer, view, values, memory);
      if (!location) {
        ++unmapped_count;
        facts.unknowns.push_back(semantic::NormalizedUnknown{
            .scope = "memory_effect",
            .reason = "unmapped_load_pointer",
            .epistemic = semantic::EpistemicState::kUnknown,
            .provenance_ref = provenance,
        });
      } else {
        alias_pointers.emplace(location->value_id, *location);
        auto operation = ResolveValueId(node->getValue(), view, values);
        if (budget.TryEmit() && operation) {
          facts.memory_effects.push_back(semantic::NormalizedMemoryEffect{
              .operation = *operation,
              .location = location->location,
              .kind = semantic::MemoryEffectKind::kMayRead,
              .epistemic = semantic::EpistemicState::kMay,
              .provenance_ref = provenance,
          });
        } else if (!operation) {
          ++unmapped_count;
          facts.unknowns.push_back(semantic::NormalizedUnknown{
              .scope = "memory_effect",
              .reason = "unmapped_load_operation",
              .epistemic = semantic::EpistemicState::kUnknown,
              .provenance_ref = provenance,
          });
        } else {
          truncated = true;
        }
      }
    } else if (const auto* store = SVF::SVFUtil::dyn_cast<SVF::StoreVFGNode>(node)) {
      const SVF::SVFVar* pointer = store->getDstNode();
      auto location = ResolveAliasPointer(pointer, view, values, memory);
      if (!location) {
        ++unmapped_count;
        facts.unknowns.push_back(semantic::NormalizedUnknown{
            .scope = "memory_effect",
            .reason = "unmapped_store_pointer",
            .epistemic = semantic::EpistemicState::kUnknown,
            .provenance_ref = provenance,
        });
      } else {
        alias_pointers.emplace(location->value_id, *location);
        auto operation = ResolveValueId(node->getValue(), view, values);
        if (budget.TryEmit() && operation) {
          facts.memory_effects.push_back(semantic::NormalizedMemoryEffect{
              .operation = *operation,
              .location = location->location,
              .kind = semantic::MemoryEffectKind::kMayWrite,
              .epistemic = semantic::EpistemicState::kMay,
              .provenance_ref = provenance,
          });
        } else if (!operation) {
          ++unmapped_count;
          facts.unknowns.push_back(semantic::NormalizedUnknown{
              .scope = "memory_effect",
              .reason = "unmapped_store_operation",
              .epistemic = semantic::EpistemicState::kUnknown,
              .provenance_ref = provenance,
          });
        } else {
          truncated = true;
        }
      }
    }

    // Value flows along out-edges (matching the pre-normalization mapper: any
    // edge whose endpoints both resolve to LLVM values is a value flow).
    for (auto edge_it = node->OutEdgeBegin(); edge_it != node->OutEdgeEnd();
         ++edge_it) {
      const SVF::SVFGEdge* edge = *edge_it;
      if (!edge) continue;

      const SVF::SVFVar* source_var =
          edge->getSrcNode() ? edge->getSrcNode()->getValue() : nullptr;
      const SVF::SVFVar* dest_var =
          edge->getDstNode() ? edge->getDstNode()->getValue() : nullptr;
      auto source = ResolveValueId(source_var, view, values);
      auto dest = ResolveValueId(dest_var, view, values);

      if (!source || !dest) {
        ++unmapped_count;
        facts.unknowns.push_back(semantic::NormalizedUnknown{
            .scope = "value_flow_edge",
            .reason = "unmapped_svf_node",
            .epistemic = semantic::EpistemicState::kUnknown,
            .provenance_ref = provenance,
        });
        continue;
      }

      if (budget.TryEmit()) {
        facts.value_flows.push_back(semantic::NormalizedValueFlow{
            .source_value_id = *source,
            .destination_value_id = *dest,
            .epistemic = semantic::EpistemicState::kMay,
            .provenance_ref = provenance,
        });
      } else {
        truncated = true;
      }

      // Pointer-pointer value-flow edges also admit alias candidates.
      if (IsPointer(source_var, view) && IsPointer(dest_var, view)) {
        if (auto candidate =
                ResolveAliasPointer(source_var, view, values, memory)) {
          alias_pointers.emplace(candidate->value_id, *candidate);
        }
        if (auto candidate =
                ResolveAliasPointer(dest_var, view, values, memory)) {
          alias_pointers.emplace(candidate->value_id, *candidate);
        }
      }
    }
  }

  // ---- Pass 2: alias relationships over all admitted pointer pairs ----
  {
    std::vector<const AliasPointer*> ordered;
    ordered.reserve(alias_pointers.size());
    for (const auto& entry : alias_pointers) ordered.push_back(&entry.second);
    // std::map iteration is already key-ordered (by value id), so `ordered`
    // is sorted and the cross-product below is deterministic.

    // SVF's BVDataPTAImpl::alias re-expands both points-to sets on every call,
    // so the O(N^2) cross-product below paid that cost once per pair. Hoist
    // the field-insensitive expansion out of the loop: compute, once per
    // pointer, the expanded points-to set as an ascending vector (with the
    // black-hole object tracked separately) and answer MayAlias/NoAlias with a
    // cheap two-pointer intersection. This preserves alias()'s exact result --
    // MayAlias iff either side reaches the black hole or the expanded sets
    // intersect -- while removing the dominant per-pair cost.
    struct ExpandedPointsTo {
      std::vector<SVF::NodeID> objects;  // ascending, black-hole node excluded
      bool has_black_hole = false;
    };
    std::vector<ExpandedPointsTo> expanded(ordered.size());
    const SVF::NodeID black_hole = view.svf_ir->getBlackHoleNode();
    for (std::size_t i = 0; i < ordered.size(); ++i) {
      SVF::PointsTo pts;
      view.andersen->expandFIObjs(
          view.andersen->getPts(ordered[i]->var->getId()), pts);
      ExpandedPointsTo& info = expanded[i];
      info.objects.reserve(pts.count());
      for (SVF::NodeID obj : pts) {
        if (obj == black_hole) {
          info.has_black_hole = true;
        } else {
          info.objects.push_back(obj);
        }
      }
      // SparseBitVector iterates in ascending order, but sort defensively so
      // the two-pointer scan below never depends on that internal ordering.
      std::sort(info.objects.begin(), info.objects.end());
    }

    bool alias_truncated = false;
    for (std::size_t i = 0; i < ordered.size() && !alias_truncated; ++i) {
      for (std::size_t j = i + 1; j < ordered.size(); ++j) {
        // Bound the cross-product by the alias-pair budget (and soft time
        // budget) before doing any per-pair work.
        if (!budget.TryAliasQuery()) {
          alias_truncated = true;
          break;
        }
        const AliasPointer& left = *ordered[i];
        const AliasPointer& right = *ordered[j];
        const ExpandedPointsTo& lhs = expanded[i];
        const ExpandedPointsTo& rhs = expanded[j];
        const bool may_alias = lhs.has_black_hole || rhs.has_black_hole ||
                               IntersectsSorted(lhs.objects, rhs.objects);
        AliasObservation observation =
            may_alias
                ? AliasObservation{semantic::AliasKind::kMayAlias,
                                   semantic::EpistemicState::kMay}
                : AliasObservation{semantic::AliasKind::kNoAlias,
                                   semantic::EpistemicState::kMust};
        if (observation.kind == semantic::AliasKind::kMayAlias &&
            ResolvesToOneProvenObject(left, right, view)) {
          observation = {semantic::AliasKind::kMustAlias,
                         semantic::EpistemicState::kMust};
        }
        if (budget.TryEmit()) {
          facts.aliases.push_back(semantic::NormalizedAlias{
              .left = left.location,
              .right = right.location,
              .kind = observation.kind,
              .epistemic = observation.epistemic,
              .provenance_ref = provenance,
          });
        } else {
          truncated = true;
          break;
        }
      }
    }
    if (alias_truncated) {
      truncated = true;
    }
  }

  // ---- Pass 3: indirect and virtual call targets ----
  {
    const SVF::CallGraph* callgraph = view.andersen->getCallGraph();

    // Resolve each indirect call site to stable identities first, then sort by
    // (call_site, caller, callee) so emission never depends on SVF's
    // pointer-ordered maps.
    struct ResolvedCall {
      StableId call_site;
      StableId caller;
      std::optional<StableId> callee;
      semantic::DispatchKind dispatch;
      std::string diagnostic_symbol;
    };
    std::vector<ResolvedCall> resolved_calls;

    for (const auto& entry : view.svf_ir->getIndirectCallsites()) {
      const SVF::CallICFGNode* call_block = entry.first;
      const ::llvm::Value* call_value = ResolveLlvmValue(call_block, view);
      const auto* call = ::llvm::dyn_cast<::llvm::CallBase>(call_value);
      if (!call || !call->getFunction()) {
        ++unmapped_count;
        facts.unknowns.push_back(semantic::NormalizedUnknown{
            .scope = "indirect_call",
            .reason = "unmapped_call_site",
            .epistemic = semantic::EpistemicState::kUnknown,
            .provenance_ref = provenance,
        });
        continue;
      }

      auto call_site_id = values.CallSiteIdFor(*call);
      auto caller_id = values.IdFor(*call->getFunction());
      if (!call_site_id.ok() || !caller_id.ok()) {
        ++unmapped_count;
        facts.unknowns.push_back(semantic::NormalizedUnknown{
            .scope = "indirect_call",
            .reason = "unmapped_call_site",
            .epistemic = semantic::EpistemicState::kUnknown,
            .provenance_ref = provenance,
        });
        continue;
      }

      const semantic::DispatchKind dispatch =
          call_block->isVirtualCall()
              ? semantic::DispatchKind::kVirtual
              : (IsFormalFunctionPointerCall(*call)
                     ? semantic::DispatchKind::kCallback
                     : semantic::DispatchKind::kIndirect);

      const SVF::CallGraph::FunctionSet* candidates = nullptr;
      if (callgraph && callgraph->hasIndCSCallees(call_block)) {
        candidates = &callgraph->getIndCSCallees(call_block);
      }
      if (!candidates || candidates->empty()) {
        ++unmapped_count;
        facts.unknowns.push_back(semantic::NormalizedUnknown{
            .scope = "indirect_call",
            .reason = "no_resolved_targets",
            .epistemic = semantic::EpistemicState::kUnknown,
            .provenance_ref = provenance,
        });
        continue;
      }

      for (const SVF::FunObjVar* candidate : *candidates) {
        const ::llvm::Value* candidate_value =
            ResolveLlvmValue(candidate, view);
        const auto* callee_fn =
            ::llvm::dyn_cast<::llvm::Function>(candidate_value);
        auto callee_id = ResolveValueId(candidate, view, values);
        if (!callee_fn || !callee_id) {
          ++unmapped_count;
          facts.unknowns.push_back(semantic::NormalizedUnknown{
              .scope = "indirect_call",
              .reason = "unmapped_callee",
              .epistemic = semantic::EpistemicState::kUnknown,
              .provenance_ref = provenance,
          });
          continue;
        }
        resolved_calls.push_back(ResolvedCall{
            .call_site = *call_site_id,
            .caller = *caller_id,
            .callee = *callee_id,
            .dispatch = dispatch,
            .diagnostic_symbol = call->getFunction()->getName().str(),
        });
      }
    }

    std::sort(resolved_calls.begin(), resolved_calls.end(),
              [](const ResolvedCall& a, const ResolvedCall& b) {
                if (a.call_site != b.call_site) return a.call_site < b.call_site;
                if (a.caller != b.caller) return a.caller < b.caller;
                if (a.callee != b.callee) return a.callee < b.callee;
                return a.dispatch < b.dispatch;
              });

    for (const ResolvedCall& call : resolved_calls) {
      if (!budget.TryEmit()) {
        truncated = true;
        break;
      }
      facts.calls.push_back(semantic::NormalizedCallTarget{
          .call_site = call.call_site,
          .caller = call.caller,
          .callee = call.callee,
          .dispatch = call.dispatch,
          .epistemic = semantic::EpistemicState::kMay,
          .diagnostic_symbol = call.diagnostic_symbol,
          .provenance_ref = provenance,
      });
    }
  }

  // ---- Pass 4: budget truncation becomes a scoped unknown ----
  if (truncated) {
    facts.unknowns.push_back(semantic::NormalizedUnknown{
        .scope = "analysis_truncated",
        .reason = BudgetReasonName(budget.state().reason),
        .epistemic = semantic::EpistemicState::kUnknown,
        .provenance_ref = provenance,
    });
  }

  Canonicalize(facts.value_flows);
  Canonicalize(facts.aliases);
  Canonicalize(facts.memory_effects);
  Canonicalize(facts.calls);
  Canonicalize(facts.unknowns);
  Canonicalize(facts.dependencies);

  const auto completion = (unmapped_count > 0 || truncated)
                              ? SvfMappingCompletion::kCompleteWithUnknowns
                              : SvfMappingCompletion::kComplete;
  *result = SvfMappingResult{.completion = completion, .facts = std::move(facts)};
  return Status::Ok();
}

}  // namespace veritas::analysis::svf
