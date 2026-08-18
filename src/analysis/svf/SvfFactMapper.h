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

#ifndef VERITAS_ANALYSIS_SVF_SVFFACTMAPPER_H_
#define VERITAS_ANALYSIS_SVF_SVFFACTMAPPER_H_

#include <string>
#include <vector>

#include "analysis/svf/SvfSession.h"
#include "analysis/svf/SvfConfig.h"

namespace veritas::analysis::pipeline {
class ProgramIr;
}  // namespace veritas::analysis::pipeline

namespace veritas::analysis::svf {

// Placeholder types for VERITAS Summary IR facts
// In real implementation these would be from include/veritas/summary/
namespace summary {

struct ValueRef {
  std::string name;
  bool operator==(const ValueRef&) const = default;
};

struct MemoryRef {
  std::string name;
  bool operator==(const MemoryRef&) const = default;
};

struct ValueFlowFact {
  ValueRef source;
  ValueRef destination;
  std::string provenance;
  bool operator==(const ValueFlowFact&) const = default;
};

struct AliasFact {
  MemoryRef left;
  MemoryRef right;
  std::string relationship;  // MUST_ALIAS, MAY_ALIAS, NO_ALIAS, UNKNOWN_ALIAS
  std::string provenance;
  bool operator==(const AliasFact&) const = default;
};

struct MemoryEffectFact {
  ValueRef operation;
  MemoryRef memory;
  std::string effect_kind;  // READ, WRITE, MAY_READ, MAY_WRITE
  std::string provenance;
  bool operator==(const MemoryEffectFact&) const = default;
};

struct CallFact {
  ValueRef callsite;
  ValueRef target;
  std::string call_kind;  // MUST_CALL, MAY_CALL, UNKNOWN_CALL
  std::string provenance;
  bool operator==(const CallFact&) const = default;
};

struct UnknownFact {
  std::string scope;
  std::string reason;
  std::string provenance;
  bool operator==(const UnknownFact&) const = default;
};

struct DependencyEdge {
  ValueRef from;
  ValueRef to;
  std::string kind;
  bool operator==(const DependencyEdge&) const = default;
};

}  // namespace summary

// AnalyzerRunContext provides identity and provenance for this analysis run
struct AnalyzerRunContext {
  std::string analyzer_run_id;
  std::string llvm_toolchain_identity;
  std::string program_module_hash;
};

// SvfFacts contains all VERITAS-normalized facts mapped from SVF results
struct SvfFacts {
  std::vector<summary::ValueFlowFact> value_flows;
  std::vector<summary::AliasFact> aliases;
  std::vector<summary::MemoryEffectFact> refined_memory_effects;
  std::vector<summary::CallFact> refined_calls;
  std::vector<summary::UnknownFact> unknowns;
  std::vector<summary::DependencyEdge> dependencies;
};

// SvfMappingCompletion indicates whether mapping completed fully or with unknowns
enum class SvfMappingCompletion {
  kComplete,
  kCompleteWithUnknowns,
};

// SvfMappingResult packages the completion status with mapped facts
struct SvfMappingResult {
  SvfMappingCompletion completion;
  SvfFacts facts;
};

// MapSvfFacts translates SVF analysis results into VERITAS Summary IR facts.
//
// Resolves SVF values through LLVM values and M4 origin maps. Maps value-flow
// edges, alias relationships, memory effects, and indirect call targets.
// Attaches complete analyzer provenance to every fact.
//
// Returns kComplete when all SVF results mapped successfully, or
// kCompleteWithUnknowns when some results could not be resolved (unmapped
// SVF nodes become scoped UnknownFacts).
Status MapSvfFacts(const pipeline::ProgramIr& program_ir,
                   const SvfSessionView& view,
                   const AnalyzerRunContext& run_context,
                   const SvfConfig& config,
                   SvfMappingResult* result);

}  // namespace veritas::analysis::svf

#endif  // VERITAS_ANALYSIS_SVF_SVFFACTMAPPER_H_
