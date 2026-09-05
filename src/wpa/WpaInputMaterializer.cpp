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

#include "veritas/wpa/WpaInputMaterializer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "veritas/core/Hash.h"
#include "veritas/facts/RelationSchema.h"
#include "veritas/wpa/CallGraph.h"
#include "veritas/wpa/SccGraph.h"

namespace veritas::wpa {
namespace {

namespace v1 = summary::v1;
namespace v2 = summary::v2;
namespace sem = analysis::semantic;

// Canonical, injective, length-prefixed field encoding. Every field is
// self-delimiting so concatenation cannot be ambiguous.
void AppendField(std::string* out, std::string_view value) {
  out->append(std::to_string(value.size()));
  out->push_back(':');
  out->append(value);
}

void AppendUInt(std::string* out, std::uint64_t value) {
  out->append(std::to_string(value));
  out->push_back(';');
}

sem::EpistemicState ToSemantic(v1::EpistemicState epistemic) {
  switch (epistemic) {
  case v1::EPISTEMIC_STATE_MUST:
    return sem::EpistemicState::kMust;
  case v1::EPISTEMIC_STATE_MAY:
    return sem::EpistemicState::kMay;
  case v1::EPISTEMIC_STATE_MUST_NOT:
    return sem::EpistemicState::kMustNot;
  case v1::EPISTEMIC_STATE_INFERRED:
    return sem::EpistemicState::kInferred;
  case v1::EPISTEMIC_STATE_ASSUMED:
    return sem::EpistemicState::kAssumed;
  default:
    return sem::EpistemicState::kUnknown;
  }
}

sem::DispatchKind ToSemantic(v2::DispatchKind dispatch) {
  switch (dispatch) {
  case v2::DISPATCH_KIND_DIRECT:
    return sem::DispatchKind::kDirect;
  case v2::DISPATCH_KIND_INDIRECT:
    return sem::DispatchKind::kIndirect;
  case v2::DISPATCH_KIND_VIRTUAL:
    return sem::DispatchKind::kVirtual;
  case v2::DISPATCH_KIND_CALLBACK:
    return sem::DispatchKind::kCallback;
  case v2::DISPATCH_KIND_EXTERNAL:
    return sem::DispatchKind::kExternal;
  default:
    return sem::DispatchKind::kUnknown;
  }
}

// A call read from either schema version, with the schema's own admission
// rule already applied. V1 carries no dispatch, so it projects as kUnknown
// rather than being silently reported as a direct call.
struct NormalizedCall {
  std::string call_site_id;
  std::string callee_symbol;
  std::string resolved_callee;
  sem::DispatchKind dispatch;
  sem::EpistemicState epistemic;
  bool admissible;
};

struct NormalizedWrite {
  std::string memory_location_id;
  sem::ByteRangeKind range_kind;
  std::int64_t offset;
  std::uint64_t size;
  sem::EpistemicState epistemic;
  bool is_write;
};

bool IsEdgeAdmissible(v1::EpistemicState epistemic) {
  return epistemic == v1::EPISTEMIC_STATE_MUST ||
         epistemic == v1::EPISTEMIC_STATE_MAY ||
         epistemic == v1::EPISTEMIC_STATE_INFERRED ||
         epistemic == v1::EPISTEMIC_STATE_ASSUMED;
}

std::vector<NormalizedCall> CallsOf(const summary::SummaryArtifact &artifact) {
  std::vector<NormalizedCall> calls;
  if (const auto *legacy = std::get_if<v1::FunctionSummary>(&artifact)) {
    for (const auto &call : legacy->calls()) {
      const bool positive = call.epistemic() == v1::EPISTEMIC_STATE_MUST ||
                            call.epistemic() == v1::EPISTEMIC_STATE_MAY;
      calls.push_back(NormalizedCall{
          .call_site_id = call.call_site_anchor_id(),
          .callee_symbol = call.callee_symbol(),
          .resolved_callee = call.resolved_callee_function_variant_id(),
          .dispatch = sem::DispatchKind::kUnknown,
          .epistemic = ToSemantic(call.epistemic()),
          .admissible = positive});
    }
    return calls;
  }
  const auto &current = std::get<v2::FunctionSummary>(artifact);
  for (const auto &call : current.calls()) {
    calls.push_back(NormalizedCall{
        .call_site_id = call.call_site_id(),
        .callee_symbol = call.callee_symbol(),
        .resolved_callee = call.resolved_callee_function_variant_id(),
        .dispatch = ToSemantic(call.dispatch()),
        .epistemic = ToSemantic(call.epistemic()),
        .admissible = IsEdgeAdmissible(call.epistemic())});
  }
  return calls;
}

std::vector<NormalizedWrite>
MemoryEffectsOf(const summary::SummaryArtifact &artifact) {
  std::vector<NormalizedWrite> effects;
  const auto *current = std::get_if<v2::FunctionSummary>(&artifact);
  if (current == nullptr) {
    // A tagged V1 projection carries no abstract-memory identity, so it
    // supplies no memory rows rather than fabricating them.
    return effects;
  }
  for (const auto &effect : current->memory_effects()) {
    const bool is_write = effect.kind() == v1::EFFECT_KIND_WRITE;
    if (!is_write && effect.kind() != v1::EFFECT_KIND_READ)
      continue;
    const auto &range = effect.location().byte_range();
    const bool known = range.offset_known() && range.size_known();
    effects.push_back(NormalizedWrite{
        .memory_location_id = effect.location().memory_location_id(),
        .range_kind =
            known ? sem::ByteRangeKind::kKnown : sem::ByteRangeKind::kUnknown,
        .offset = known ? range.offset() : 0,
        .size = known ? range.size() : 0,
        .epistemic = ToSemantic(effect.epistemic()),
        .is_write = is_write});
  }
  return effects;
}

// Canonical text for a model effect kind. This becomes the ModeledEffect
// effect_kind cell, so the tokens are part of the relation's content.
std::string_view ModelEffectName(sem::ModelEffectKind kind) {
  switch (kind) {
  case sem::ModelEffectKind::kRead:
    return "read";
  case sem::ModelEffectKind::kWrite:
    return "write";
  case sem::ModelEffectKind::kAllocate:
    return "allocate";
  case sem::ModelEffectKind::kDeallocate:
    return "deallocate";
  case sem::ModelEffectKind::kUnknown:
    return "unknown";
  }
  return "unknown";
}

// ModeledEffect admits only MUST or ASSUMED, while a bundle may state any
// epistemic value -- the shipped models are all `may`. A model is an external
// assumption that nothing in this run verified, so anything short of MUST
// enters as ASSUMED. Dropping the row instead would lose the model silently,
// and promoting it to MUST would claim a warrant the bundle never gave.
sem::EpistemicState ModeledEpistemic(sem::EpistemicState stated) {
  return stated == sem::EpistemicState::kMust ? sem::EpistemicState::kMust
                                              : sem::EpistemicState::kAssumed;
}

// Model bundles are keyed on library names, but Clang lowers calls it
// recognizes as builtins to intrinsics: a call to memcpy reaches the summary
// as llvm.memcpy.p0.p0.i64. An exact lookup then finds nothing and the model
// contributes no rows, silently, with no error to notice.
//
// The fallback strips the llvm. prefix and the overload suffixes, leaving the
// intrinsic's base name. It applies only to llvm.-prefixed symbols, so an
// ordinary function is never rewritten into a model match; and a multi-segment
// intrinsic such as llvm.lifetime.start reduces to "lifetime", which no
// library model claims.
std::string_view IntrinsicBaseName(std::string_view symbol) {
  constexpr std::string_view kPrefix = "llvm.";
  if (!symbol.starts_with(kPrefix))
    return {};
  const std::string_view rest = symbol.substr(kPrefix.size());
  const auto end = rest.find('.');
  return end == std::string_view::npos ? rest : rest.substr(0, end);
}

// Models applicable to a callee, matched on the symbol the bundle records and
// falling back to the intrinsic base name.
std::span<const sem::FunctionModel> ModelsForCallee(
    const sem::ModelBundle &models, std::string_view callee_symbol) {
  auto direct = models.Lookup(callee_symbol);
  if (!direct.empty())
    return direct;
  const std::string_view base = IntrinsicBaseName(callee_symbol);
  return base.empty() ? direct : models.Lookup(base);
}

// The support relation that carries a successor result for this component.
facts::RelationId SupportRelationFor(WpaComponentKind component) {
  return component == WpaComponentKind::kReachability
             ? facts::RelationId::kSupportReachableCall
             : facts::RelationId::kSupportMayWrite;
}

// The derived relation whose successor results this component may cite.
facts::RelationId DerivedRelationFor(WpaComponentKind component) {
  return component == WpaComponentKind::kReachability
             ? facts::RelationId::kReachableCall
             : facts::RelationId::kMayWrite;
}

// Canonical encoding of a semantic row. Used both to order the EDB and to hash
// it, so ordering and hashing can never disagree.
std::string EncodeSemanticRow(const facts::SemanticRow &row) {
  std::string bytes;
  AppendField(&bytes, facts::RelationsV2().Get(row.relation).name);
  AppendUInt(&bytes, row.cells.size());
  for (const auto &cell : row.cells) {
    std::visit(
        [&](const auto &value) {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, core::StableId>) {
            bytes.push_back('I');
            AppendField(&bytes, core::ToString(value));
          } else if constexpr (std::is_same_v<T, std::string>) {
            bytes.push_back('S');
            AppendField(&bytes, value);
          } else if constexpr (std::is_same_v<T, std::int64_t>) {
            bytes.push_back('N');
            AppendField(&bytes, std::to_string(value));
          } else if constexpr (std::is_same_v<T, std::uint64_t>) {
            bytes.push_back('U');
            AppendField(&bytes, std::to_string(value));
          } else {
            bytes.push_back('E');
            AppendUInt(&bytes, static_cast<std::uint64_t>(value));
          }
        },
        cell);
  }
  return bytes;
}

} // namespace

std::string_view ComponentKindName(WpaComponentKind component) {
  return component == WpaComponentKind::kReachability ? "reachability"
                                                      : "memory-effects";
}

StatusOr<WpaLogicalComponentInput>
WpaInputMaterializer::Build(const WpaMaterializationRequest &request) {
  // 1. Recover SCC membership. The orchestrator supplies the pre-built SCC
  // decomposition so it is not rebuilt for every component; standalone callers
  // fall back to deriving it from the summaries.
  const SccGraph* scc_graph = request.scc_graph;
  std::optional<SccGraph> owned_scc_graph;
  if (scc_graph == nullptr) {
    auto call_graph = CallGraph::FromSummaries(request.summaries);
    if (!call_graph.ok())
      return call_graph.status();
    auto built = SccGraph::Build(*call_graph);
    if (!built.ok())
      return built.status();
    owned_scc_graph = std::move(*built);
    scc_graph = &*owned_scc_graph;
  }
  auto members = scc_graph->Members(request.scc_id);
  if (!members.ok())
    return members.status();
  const std::set<core::StableId> member_set(members->begin(), members->end());

  // 2. Index the supplied summaries by function variant.
  std::map<core::StableId, const summary::SummaryArtifact *> by_function;
  for (const auto &artifact : request.summaries) {
    auto function_id =
        core::ParseStableId(summary::Identity(artifact).function_variant_id());
    if (!function_id.ok())
      return Status::InvalidArgument("invalid summary identity");
    by_function.emplace(*function_id, &artifact);
  }

  // 3. Build the semantic EDB for this component from member summaries only.
  // A component publishes facts owned by its own SCC, so a non-member's local
  // facts never enter here.
  const bool memory_component =
      request.component == WpaComponentKind::kMemoryEffects;
  std::vector<facts::SemanticRow> semantic_edb;
  // Base facts owned by this SCC's members. They root every witness chain a
  // locally derived result can produce, so they are collected as they are
  // emitted. Identity map relations are plumbing, not facts, and never appear
  // here.
  std::vector<facts::SemanticRow> local_base_rows;
  std::vector<core::StableId> function_ids(members->begin(), members->end());
  std::vector<core::StableId> call_site_ids;
  std::vector<core::StableId> memory_ids;

  for (const auto &member : *members) {
    const auto it = by_function.find(member);
    if (it == by_function.end())
      return Status::NotFound("SCC member has no summary");

    for (const auto &call : CallsOf(*it->second)) {
      auto call_site = core::ParseStableId(call.call_site_id);
      if (!call_site.ok() || call_site->kind != core::IdKind::kCallSite)
        return Status::InvalidArgument("invalid call-site identity");
      call_site_ids.push_back(*call_site);

      core::StableId callee;
      bool resolved = false;
      if (call.admissible && !call.resolved_callee.empty()) {
        auto parsed = core::ParseStableId(call.resolved_callee);
        if (!parsed.ok() || parsed->kind != core::IdKind::kFunctionVariant)
          return Status::InvalidArgument("invalid resolved callee identity");
        // The callee needs a dense id even when it lives in a successor SCC.
        callee = *parsed;
        resolved = true;
        function_ids.push_back(callee);
      }

      facts::SemanticRow row;
      if (resolved) {
        row.relation = facts::RelationId::kDirectCall;
        row.cells = {*call_site, member, callee, call.dispatch, call.epistemic};
      } else {
        // An unresolved target stays scoped to its call site. It never becomes
        // an edge to every function.
        row.relation = facts::RelationId::kUnknownCall;
        row.cells = {*call_site, member,
                     call.callee_symbol.empty() ? std::string("<unknown>")
                                                : call.callee_symbol,
                     call.epistemic};
      }
      local_base_rows.push_back(row);
      semantic_edb.push_back(std::move(row));

      // Applicable models. A model describes an external function, which has
      // no summary and therefore no dense function id, so the relation's
      // FunctionId column names the member that invokes it -- the same reason
      // the relation carries no call-site column. Models are matched on the
      // callee symbol exactly as the bundle records it; a symbol the bundle
      // does not know contributes nothing rather than guessing.
      if (request.models == nullptr)
        continue;
      for (const auto &model :
           ModelsForCallee(*request.models, call.callee_symbol)) {
        facts::SemanticRow modeled;
        modeled.relation = facts::RelationId::kModeledEffect;
        modeled.cells = {model.model_id, member,
                         std::string(ModelEffectName(model.effect)),
                         model.subject, ModeledEpistemic(model.epistemic)};
        semantic_edb.push_back(std::move(modeled));
      }
    }

    if (!memory_component)
      continue;
    for (const auto &effect : MemoryEffectsOf(*it->second)) {
      auto memory = core::ParseStableId(effect.memory_location_id);
      if (!memory.ok() || memory->kind != core::IdKind::kMemoryRef)
        return Status::InvalidArgument("invalid memory-location identity");
      memory_ids.push_back(*memory);
      facts::SemanticRow row;
      row.relation = effect.is_write ? facts::RelationId::kDirectWrite
                                     : facts::RelationId::kDirectRead;
      row.cells = {member,        *memory,     effect.range_kind,
                   effect.offset, effect.size, effect.epistemic};
      local_base_rows.push_back(row);
      semantic_edb.push_back(std::move(row));
    }
  }

  // 4. Successor results enter as explicit support rows carrying stable
  // support-fact identities. They are inputs, never results of this component.
  const facts::RelationId derived = DerivedRelationFor(request.component);
  const facts::RelationId support = SupportRelationFor(request.component);
  std::vector<RootedInputFact> successor_roots;
  std::vector<core::StableId> fact_ids;
  for (const auto &fact : request.successor_support) {
    if (fact.row.relation != derived) {
      return Status::FailedPrecondition(
          "successor support fact does not belong to this component");
    }
    facts::SemanticRow row;
    row.relation = support;
    row.cells = fact.row.cells;
    for (const auto &cell : row.cells) {
      if (const auto *id = std::get_if<core::StableId>(&cell)) {
        if (id->kind == core::IdKind::kFunctionVariant)
          function_ids.push_back(*id);
        else if (id->kind == core::IdKind::kMemoryRef)
          memory_ids.push_back(*id);
      }
    }
    // A witness cites the support relation, not the predecessor's derived
    // relation. Root the projected row so every cross-SCC witness terminates
    // at the exact semantic key it names.
    auto support_fact = facts::MakeFact(row);
    if (!support_fact.ok())
      return support_fact.status();
    semantic_edb.push_back(std::move(row));
    fact_ids.push_back(fact.fact_id);
    successor_roots.push_back(RootedInputFact{
        .fact = std::move(*support_fact), .provenance_ref = "wpa:successor"});
  }

  // 5. Build the dense maps. Build() sorts and de-duplicates, so dense numbers
  // depend only on the set of stable ids, never on the order they were found.
  auto functions = facts::FunctionDenseMap::Build(std::move(function_ids));
  if (!functions.ok())
    return functions.status();
  auto memories = facts::MemoryDenseMap::Build(std::move(memory_ids));
  if (!memories.ok())
    return memories.status();
  auto call_sites = facts::CallSiteDenseMap::Build(std::move(call_site_ids));
  if (!call_sites.ok())
    return call_sites.status();
  auto values = facts::ValueDenseMap::Build({});
  if (!values.ok())
    return values.status();
  auto fact_map =
      facts::DenseIdMap<facts::FactId, core::IdKind::kFact>::Build(
          std::move(fact_ids));
  if (!fact_map.ok())
    return fact_map.status();

  // 6. Emit the dual-identity map relations so an evaluator can reconstruct
  // semantic keys without dense ids ever escaping the run.
  for (const auto &stable : functions->StableIds()) {
    semantic_edb.push_back(facts::SemanticRow{
        facts::RelationId::kFunctionMap, {stable, core::ToString(stable)}});
  }
  for (const auto &stable : memories->StableIds()) {
    semantic_edb.push_back(facts::SemanticRow{
        facts::RelationId::kMemoryMap, {stable, core::ToString(stable)}});
  }
  for (const auto &stable : call_sites->StableIds()) {
    semantic_edb.push_back(facts::SemanticRow{
        facts::RelationId::kCallSiteMap, {stable, core::ToString(stable)}});
  }
  for (const auto &stable : fact_map->StableIds()) {
    semantic_edb.push_back(facts::SemanticRow{facts::RelationId::kFactMap,
                                              {stable, core::ToString(stable)}});
  }

  // 7. Canonically order the semantic EDB. Ordering uses the same encoding as
  // the hash, so a reordering can never change one without the other.
  std::vector<std::pair<std::string, const facts::SemanticRow *>> encoded;
  encoded.reserve(semantic_edb.size());
  for (const auto &row : semantic_edb) {
    encoded.emplace_back(EncodeSemanticRow(row), &row);
  }
  std::ranges::sort(encoded, {}, &std::pair<std::string, const facts::SemanticRow *>::first);

  // 8. Convert canonical semantic rows to execution rows through the maps.
  WpaLogicalComponentInput input;
  input.edb.reserve(encoded.size());
  for (const auto &[key, row] : encoded) {
    facts::ExecutionRow execution;
    execution.relation = row->relation;
    const auto &schema = facts::RelationsV2().Get(row->relation);
    if (row->cells.size() != schema.columns.size())
      return Status::InvalidArgument("row does not match its relation schema");
    for (std::size_t i = 0; i < row->cells.size(); ++i) {
      const auto &cell = row->cells[i];
      const auto domain = schema.columns[i].domain;
      if (const auto *id = std::get_if<core::StableId>(&cell)) {
        switch (domain) {
        case facts::ColumnDomain::kFunctionId: {
          auto dense = functions->ToDense(*id);
          if (!dense.ok())
            return dense.status();
          execution.cells.push_back(*dense);
          break;
        }
        case facts::ColumnDomain::kMemoryId: {
          auto dense = memories->ToDense(*id);
          if (!dense.ok())
            return dense.status();
          execution.cells.push_back(*dense);
          break;
        }
        case facts::ColumnDomain::kCallSiteId: {
          auto dense = call_sites->ToDense(*id);
          if (!dense.ok())
            return dense.status();
          execution.cells.push_back(*dense);
          break;
        }
        case facts::ColumnDomain::kFactId: {
          auto dense = fact_map->ToDense(*id);
          if (!dense.ok())
            return dense.status();
          execution.cells.push_back(*dense);
          break;
        }
        case facts::ColumnDomain::kModelId:
          // Models are not dense-mapped; the execution projection carries the
          // model id as canonical text.
          execution.cells.push_back(core::ToString(*id));
          break;
        default:
          return Status::InvalidArgument("stable id in a non-id column");
        }
        continue;
      }
      std::visit(
          [&](const auto &value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (!std::is_same_v<T, core::StableId>) {
              execution.cells.push_back(value);
            }
          },
          cell);
    }
    auto valid = facts::ValidateExecutionRow(execution);
    if (!valid.ok())
      return valid;
    input.edb.push_back(std::move(execution));
  }

  // Root every member base fact. MakeFact derives a witness-independent id
  // from the semantic row alone, so a root's identity does not depend on how
  // the component was executed.
  std::vector<RootedInputFact> local_roots;
  local_roots.reserve(local_base_rows.size());
  for (const auto &row : local_base_rows) {
    auto fact = facts::MakeFact(row);
    if (!fact.ok())
      return fact.status();
    local_roots.push_back(
        RootedInputFact{.fact = std::move(*fact), .provenance_ref = "wpa:local"});
  }

  // 9. Derive the logical input hash. It covers the semantic configuration,
  // the component, the SCC members, the canonical semantic EDB, and the model
  // bundle. It excludes revision, RunId, engine identity, dense-number
  // assignment, and the order rows or summaries were discovered in.
  std::string hash_bytes;
  AppendField(&hash_bytes, "veritas.wpa.logical-input.v1");
  AppendField(&hash_bytes, request.semantics.summary_schema_version);
  AppendField(&hash_bytes, request.semantics.relation_schema_version);
  AppendField(&hash_bytes, request.semantics.rule_bundle_version);
  AppendField(&hash_bytes, request.semantics.model_bundle_version);
  AppendField(&hash_bytes, request.semantics.svf_configuration_hash);
  AppendField(&hash_bytes, request.semantics.wpa_configuration_hash);
  AppendField(&hash_bytes, core::ToString(request.semantics.build_variant_id));
  AppendField(&hash_bytes, ComponentKindName(request.component));
  AppendField(&hash_bytes, core::ToString(request.scc_id));
  AppendUInt(&hash_bytes, members->size());
  for (const auto &member : *members) {
    AppendField(&hash_bytes, core::ToString(member));
  }
  AppendUInt(&hash_bytes, encoded.size());
  for (const auto &[key, row] : encoded) {
    AppendField(&hash_bytes, key);
  }
  AppendField(&hash_bytes,
              request.models == nullptr ? "" : request.models->hash());

  const auto digest = core::ComputeSHA256(
      std::as_bytes(std::span(hash_bytes.data(), hash_bytes.size())));

  input.scc_id = request.scc_id;
  input.component = request.component;
  input.mappings.functions = std::move(*functions);
  input.mappings.values = std::move(*values);
  input.mappings.memories = std::move(*memories);
  input.mappings.call_sites = std::move(*call_sites);
  input.mappings.facts = std::move(*fact_map);
  input.local_roots = std::move(local_roots);
  input.successor_roots = std::move(successor_roots);
  input.logical_input_hash = core::DigestToHex(digest);
  return input;
}

} // namespace veritas::wpa
