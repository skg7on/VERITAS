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

#include "veritas/query/QueryTypes.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "veritas/core/Hash.h"
#include "veritas/summary/ComponentHash.h"

namespace veritas::query {
namespace {

using facts::ColumnDomain;
using facts::RelationId;
using facts::SemanticCellValue;
using facts::SemanticRow;

// --- Canonical byte encoding ------------------------------------------------

void AppendU16(std::string *out, std::uint16_t value) {
  out->push_back(static_cast<char>((value >> 8) & 0xff));
  out->push_back(static_cast<char>(value & 0xff));
}

void AppendU32(std::string *out, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out->push_back(static_cast<char>((value >> shift) & 0xff));
  }
}

void AppendU64(std::string *out, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out->push_back(static_cast<char>((value >> shift) & 0xff));
  }
}

void AppendString(std::string *out, std::string_view value) {
  AppendU64(out, value.size());
  out->append(value);
}

void AppendStableId(std::string *out, const core::StableId &id) {
  AppendU16(out, static_cast<std::uint16_t>(id.kind));
  AppendString(out, id.digest_hex);
}

void AppendCell(std::string *out, const SemanticCellValue &cell) {
  AppendU16(out, static_cast<std::uint16_t>(cell.index()));
  if (const auto *id = std::get_if<core::StableId>(&cell)) {
    AppendStableId(out, *id);
  } else if (const auto *value = std::get_if<std::int64_t>(&cell)) {
    AppendU64(out, static_cast<std::uint64_t>(*value));
  } else if (const auto *value = std::get_if<std::uint64_t>(&cell)) {
    AppendU64(out, *value);
  } else if (const auto *value = std::get_if<std::string>(&cell)) {
    AppendString(out, *value);
  } else if (const auto *value =
                 std::get_if<analysis::semantic::DispatchKind>(&cell)) {
    AppendU16(out, static_cast<std::uint16_t>(*value));
  } else if (const auto *value =
                 std::get_if<analysis::semantic::AliasKind>(&cell)) {
    AppendU16(out, static_cast<std::uint16_t>(*value));
  } else if (const auto *value =
                 std::get_if<analysis::semantic::ByteRangeKind>(&cell)) {
    AppendU16(out, static_cast<std::uint16_t>(*value));
  } else {
    AppendU16(out, static_cast<std::uint16_t>(
                       std::get<analysis::semantic::EpistemicState>(cell)));
  }
}

void AppendRow(std::string *out, const SemanticRow &row) {
  AppendU16(out, static_cast<std::uint16_t>(row.relation));
  AppendU32(out, static_cast<std::uint32_t>(row.cells.size()));
  for (const auto &cell : row.cells) {
    AppendCell(out, cell);
  }
}

void AppendFact(std::string *out, const facts::AnalysisFact &fact) {
  AppendStableId(out, fact.fact_id);
  AppendRow(out, fact.row);
}

void AppendWitness(std::string *out, const facts::WitnessEdge &edge) {
  AppendRow(out, edge.result.row);
  AppendString(out, edge.rule_id);
  AppendString(out, edge.derivation_key);
  AppendRow(out, edge.input.row);
  AppendU32(out, edge.input_ordinal);
}

class Reader {
public:
  explicit Reader(std::string_view bytes) : bytes_(bytes) {}

  StatusOr<std::uint16_t> U16() {
    if (bytes_.size() - position_ < 2) {
      return Status::InvalidArgument("truncated boundary-summary encoding");
    }
    const auto high = static_cast<std::uint8_t>(bytes_[position_]);
    const auto low = static_cast<std::uint8_t>(bytes_[position_ + 1]);
    position_ += 2;
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(high) << 8) |
                                      low);
  }

  StatusOr<std::uint32_t> U32() {
    if (bytes_.size() - position_ < 4) {
      return Status::InvalidArgument("truncated boundary-summary encoding");
    }
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      value = (value << 8) | static_cast<std::uint8_t>(bytes_[position_ + i]);
    }
    position_ += 4;
    return value;
  }

  StatusOr<std::uint64_t> U64() {
    if (bytes_.size() - position_ < 8) {
      return Status::InvalidArgument("truncated boundary-summary encoding");
    }
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
      value = (value << 8) | static_cast<std::uint8_t>(bytes_[position_ + i]);
    }
    position_ += 8;
    return value;
  }

  StatusOr<std::string> String() {
    auto length = U64();
    if (!length.ok()) {
      return length.status();
    }
    if (*length > bytes_.size() - position_) {
      return Status::InvalidArgument("truncated boundary-summary string");
    }
    std::string value(
        bytes_.substr(position_, static_cast<std::size_t>(*length)));
    position_ += static_cast<std::size_t>(*length);
    return value;
  }

  StatusOr<core::StableId> StableIdValue() {
    auto kind = U16();
    if (!kind.ok()) {
      return kind.status();
    }
    if (*kind > static_cast<std::uint16_t>(core::IdKind::kEvidence)) {
      return Status::InvalidArgument("unknown stable-id kind");
    }
    auto digest = String();
    if (!digest.ok()) {
      return digest.status();
    }
    return core::StableId{static_cast<core::IdKind>(*kind), std::move(*digest)};
  }

  std::size_t Remaining() const { return bytes_.size() - position_; }

  bool Done() const { return position_ == bytes_.size(); }

private:
  std::string_view bytes_;
  std::size_t position_ = 0;
};

bool IsDigest(std::string_view text) {
  return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

Status
ValidateSemantics(const facts::AnalysisRunSemanticDescriptor &semantics) {
  if (semantics.build_variant_id.kind != core::IdKind::kBuildVariant ||
      semantics.build_variant_id.digest_hex.empty()) {
    return Status::InvalidArgument("dependency snapshot has no build variant");
  }
  if (semantics.summary_schema_version.empty() ||
      semantics.relation_schema_version.empty() ||
      semantics.rule_bundle_version.empty() ||
      semantics.model_bundle_version.empty()) {
    return Status::InvalidArgument(
        "dependency snapshot has an empty schema/bundle version");
  }
  if (!IsDigest(semantics.svf_configuration_hash) ||
      !IsDigest(semantics.wpa_configuration_hash)) {
    return Status::InvalidArgument(
        "dependency snapshot configuration hash is not lowercase 64-hex");
  }
  return Status::Ok();
}

std::string ProducerDigest(std::string_view name, std::string_view version) {
  std::string domain = "veritas.query.producer.";
  domain.append(name);
  domain.push_back('\0');
  domain.append(version);
  return core::DigestToHex(
      core::ComputeSHA256(std::as_bytes(std::span(domain))));
}

void EncodeRefs(std::string *out, std::vector<TypedRef> refs) {
  std::sort(refs.begin(), refs.end(), [](const TypedRef &a, const TypedRef &b) {
    if (a.id != b.id) {
      return a.id < b.id;
    }
    return a.kind < b.kind;
  });
  AppendU32(out, static_cast<std::uint32_t>(refs.size()));
  for (const auto &ref : refs) {
    AppendU16(out, static_cast<std::uint16_t>(ref.kind));
    AppendStableId(out, ref.id);
    AppendStableId(out, ref.enclosing_function);
  }
}

void EncodeProducers(std::string *out,
                     std::vector<review::ProviderVersion> producers) {
  std::sort(producers.begin(), producers.end(),
            [](const review::ProviderVersion &a,
               const review::ProviderVersion &b) { return a.name < b.name; });
  AppendU32(out, static_cast<std::uint32_t>(producers.size()));
  for (const auto &producer : producers) {
    AppendString(out, producer.name);
    AppendString(out, producer.version);
    AppendString(out, producer.config_digest);
  }
}

void EncodeFrontier(std::string *out,
                    const std::vector<QueryFrontierEntry> &frontier) {
  AppendU32(out, static_cast<std::uint32_t>(frontier.size()));
  for (const auto &entry : frontier) {
    AppendU16(out, static_cast<std::uint16_t>(entry.kind));
    AppendU16(out, entry.ref.has_value() ? 1 : 0);
    if (entry.ref.has_value()) {
      AppendStableId(out, *entry.ref);
    }
    AppendString(out, entry.detail);
  }
}

void EncodeStrings(std::string *out, std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  AppendU32(out, static_cast<std::uint32_t>(values.size()));
  for (const auto &value : values) {
    AppendString(out, value);
  }
}

} // namespace

// --- Predicates -------------------------------------------------------------

std::string_view PredicateName(QueryPredicate predicate) {
  switch (predicate) {
  case QueryPredicate::kReachableCall:
    return "reachable-call";
  case QueryPredicate::kGlobalFlow:
    return "global-flow";
  case QueryPredicate::kMayRead:
    return "may-read";
  case QueryPredicate::kMayWrite:
    return "may-write";
  case QueryPredicate::kUnknownEffect:
    return "unknown-effect";
  }
  return "invalid";
}

StatusOr<QueryPredicate> ParsePredicate(std::string_view name) {
  for (std::size_t i = 0; i < kQueryPredicateCount; ++i) {
    const auto predicate = static_cast<QueryPredicate>(i);
    if (PredicateName(predicate) == name) {
      return predicate;
    }
  }
  return Status::InvalidArgument("unknown query predicate");
}

facts::RelationId PredicateRelation(QueryPredicate predicate) {
  switch (predicate) {
  case QueryPredicate::kReachableCall:
    return RelationId::kReachableCall;
  case QueryPredicate::kGlobalFlow:
    return RelationId::kGlobalFlow;
  case QueryPredicate::kMayRead:
    return RelationId::kMayRead;
  case QueryPredicate::kMayWrite:
    return RelationId::kMayWrite;
  case QueryPredicate::kUnknownEffect:
    return RelationId::kUnknownEffect;
  }
  return RelationId::kReachableCall;
}

wpa::WpaComponentKind PredicateComponent(QueryPredicate predicate) {
  switch (predicate) {
  case QueryPredicate::kReachableCall:
    return wpa::WpaComponentKind::kReachability;
  case QueryPredicate::kGlobalFlow:
    return wpa::WpaComponentKind::kFlow;
  case QueryPredicate::kMayRead:
  case QueryPredicate::kMayWrite:
    return wpa::WpaComponentKind::kMemoryEffects;
  case QueryPredicate::kUnknownEffect:
    return wpa::WpaComponentKind::kEffects;
  }
  return wpa::WpaComponentKind::kReachability;
}

core::IdKind PredicateSourceKind(QueryPredicate predicate) {
  switch (predicate) {
  case QueryPredicate::kGlobalFlow:
    return core::IdKind::kValueRef;
  case QueryPredicate::kMayRead:
  case QueryPredicate::kMayWrite:
  case QueryPredicate::kReachableCall:
  case QueryPredicate::kUnknownEffect:
    return core::IdKind::kFunctionVariant;
  }
  return core::IdKind::kFunctionVariant;
}

std::optional<core::IdKind> PredicateSinkKind(QueryPredicate predicate) {
  switch (predicate) {
  case QueryPredicate::kReachableCall:
    return core::IdKind::kFunctionVariant;
  case QueryPredicate::kGlobalFlow:
    return core::IdKind::kValueRef;
  case QueryPredicate::kMayRead:
  case QueryPredicate::kMayWrite:
    return core::IdKind::kMemoryRef;
  case QueryPredicate::kUnknownEffect:
    // UnknownEffect's second column is a free-form subject string, not a
    // typed endpoint identity.
    return std::nullopt;
  }
  return std::nullopt;
}

bool PredicateIsPathLocal(QueryPredicate predicate) {
  return predicate == QueryPredicate::kReachableCall ||
         predicate == QueryPredicate::kGlobalFlow;
}

std::vector<facts::RelationId>
PredicateLocalRelations(QueryPredicate predicate) {
  switch (predicate) {
  case QueryPredicate::kReachableCall:
    return {RelationId::kDirectCall, RelationId::kUnknownCall};
  case QueryPredicate::kGlobalFlow:
    return {RelationId::kLocalFlow, RelationId::kParameterFlow,
            RelationId::kReturnFlow};
  case QueryPredicate::kMayRead:
  case QueryPredicate::kMayWrite:
    return {RelationId::kDirectRead, RelationId::kDirectWrite,
            RelationId::kAlias};
  case QueryPredicate::kUnknownEffect:
    return {RelationId::kModeledEffect, RelationId::kUnsupportedFeature,
            RelationId::kUnknownCall};
  }
  return {};
}

const std::vector<QuerySeedSpec> &RegisteredQuerySeeds() {
  static const std::vector<QuerySeedSpec> kSeeds = {
      {QueryPredicate::kReachableCall, "query-seed.v1",
       wpa::WpaComponentKind::kReachability, RelationId::kReachableCall,
       core::IdKind::kFunctionVariant, core::IdKind::kFunctionVariant,
       "logic/reachability/reachability.v2.dl"},
      {QueryPredicate::kGlobalFlow, "query-seed.v1",
       wpa::WpaComponentKind::kFlow, RelationId::kGlobalFlow,
       core::IdKind::kValueRef, core::IdKind::kValueRef,
       "logic/flow/global_flow.v2.dl"},
      {QueryPredicate::kMayRead, "query-seed.v1",
       wpa::WpaComponentKind::kMemoryEffects, RelationId::kMayRead,
       core::IdKind::kFunctionVariant, core::IdKind::kMemoryRef,
       "logic/memory_effects/memory_effects.v2.dl"},
      {QueryPredicate::kMayWrite, "query-seed.v1",
       wpa::WpaComponentKind::kMemoryEffects, RelationId::kMayWrite,
       core::IdKind::kFunctionVariant, core::IdKind::kMemoryRef,
       "logic/memory_effects/memory_effects.v2.dl"},
      {QueryPredicate::kUnknownEffect, "query-seed.v1",
       wpa::WpaComponentKind::kEffects, RelationId::kUnknownEffect,
       core::IdKind::kFunctionVariant, std::nullopt,
       "logic/effects/effects.v2.dl"},
  };
  return kSeeds;
}

const QuerySeedSpec *FindQuerySeed(QueryPredicate predicate) {
  for (const auto &seed : RegisteredQuerySeeds()) {
    if (seed.predicate == predicate) {
      return &seed;
    }
  }
  return nullptr;
}

// --- Descriptor validation and identity -------------------------------------

Status ValidateTypedRef(const TypedRef &ref) {
  if (ref.id.kind != ref.kind || ref.id.digest_hex.empty()) {
    return Status::InvalidArgument("typed ref kind disagrees with its id");
  }
  if (ref.enclosing_function.kind != core::IdKind::kFunctionVariant ||
      ref.enclosing_function.digest_hex.empty()) {
    return Status::InvalidArgument(
        "typed ref has no enclosing function identity");
  }
  if (ref.kind == core::IdKind::kFunctionVariant &&
      ref.id != ref.enclosing_function) {
    return Status::InvalidArgument(
        "function ref's enclosing function must be the ref itself");
  }
  return Status::Ok();
}

Status ValidateQueryLimits(const QueryLimits &limits) {
  if (limits.max_examined_rows == 0 || limits.max_proof_edges == 0 ||
      limits.max_functions == 0 || limits.max_sccs == 0) {
    return Status::InvalidArgument("query limits must all be positive");
  }
  return Status::Ok();
}

std::vector<review::ProviderVersion>
DefaultProducers(const facts::AnalysisRunSemanticDescriptor &semantics) {
  return {
      {"veritas.summary", semantics.summary_schema_version,
       ProducerDigest("veritas.summary", semantics.summary_schema_version)},
      {"veritas.relations", semantics.relation_schema_version,
       ProducerDigest("veritas.relations", semantics.relation_schema_version)},
      {"veritas.rules", semantics.rule_bundle_version,
       ProducerDigest("veritas.rules", semantics.rule_bundle_version)},
      {"veritas.models", semantics.model_bundle_version,
       ProducerDigest("veritas.models", semantics.model_bundle_version)},
      {"veritas.svf", "svf-analysis.v1", semantics.svf_configuration_hash},
      {"veritas.wpa", "wpa-execution.v1", semantics.wpa_configuration_hash},
  };
}

Status
ValidateProducers(const std::vector<review::ProviderVersion> &producers,
                  const facts::AnalysisRunSemanticDescriptor &semantics) {
  Status valid = ValidateSemantics(semantics);
  if (!valid.ok()) {
    return valid;
  }
  if (producers.size() != 6) {
    return Status::InvalidArgument(
        "query producers must be exactly the six registered producers");
  }
  std::map<std::string, review::ProviderVersion> by_name;
  for (const auto &producer : producers) {
    if (producer.name.empty() || producer.version.empty()) {
      return Status::InvalidArgument("query producer identity is incomplete");
    }
    if (!IsDigest(producer.config_digest)) {
      return Status::InvalidArgument(
          "query producer config digest is not lowercase 64-hex");
    }
    if (!by_name.emplace(producer.name, producer).second) {
      return Status::InvalidArgument("duplicate query producer name");
    }
  }
  for (const auto &expected : DefaultProducers(semantics)) {
    const auto found = by_name.find(expected.name);
    if (found == by_name.end()) {
      return Status::FailedPrecondition(
          "query producer set disagrees with the dependency snapshot: " +
          expected.name + " is missing");
    }
    if (found->second.version != expected.version ||
        found->second.config_digest != expected.config_digest) {
      return Status::FailedPrecondition(
          "query producer " + expected.name +
          " disagrees with the dependency snapshot");
    }
  }
  return Status::Ok();
}

Status ValidateQueryDescriptor(const QueryDescriptor &descriptor) {
  if (descriptor.predicate_version != kQueryPredicateVersion) {
    return Status::InvalidArgument("unsupported query predicate version");
  }
  if (static_cast<std::size_t>(descriptor.predicate) >= kQueryPredicateCount) {
    return Status::InvalidArgument("unknown query predicate");
  }
  if (descriptor.binding.repository_id.empty() ||
      descriptor.binding.base_revision_id.empty() ||
      descriptor.binding.working_tree_digest.empty() ||
      descriptor.binding.build_context_digest.empty()) {
    return Status::InvalidArgument("query review binding is incomplete");
  }
  if (descriptor.scope_digest.empty()) {
    return Status::InvalidArgument("query scope digest must not be empty");
  }
  Status limits = ValidateQueryLimits(descriptor.limits);
  if (!limits.ok()) {
    return limits;
  }
  if (descriptor.runtime.threads == 0) {
    return Status::InvalidArgument(
        "query runtime thread count must be positive");
  }
  if (descriptor.sources.empty()) {
    return Status::InvalidArgument("query must bind at least one source");
  }

  const core::IdKind source_kind = PredicateSourceKind(descriptor.predicate);
  const std::optional<core::IdKind> sink_kind =
      PredicateSinkKind(descriptor.predicate);

  std::set<std::pair<core::IdKind, std::string>> unique_ids;
  auto check_unique = [&unique_ids](const TypedRef &ref) -> Status {
    if (!unique_ids.insert({ref.kind, core::ToString(ref.id)}).second) {
      return Status::InvalidArgument("duplicate query endpoint");
    }
    return Status::Ok();
  };
  for (const auto &source : descriptor.sources) {
    Status valid = ValidateTypedRef(source);
    if (!valid.ok()) {
      return valid;
    }
    if (source.kind != source_kind) {
      return Status::InvalidArgument(
          "query source kind disagrees with the predicate");
    }
    Status unique = check_unique(source);
    if (!unique.ok()) {
      return unique;
    }
  }
  if (!sink_kind.has_value() && !descriptor.sinks.empty()) {
    return Status::InvalidArgument(
        "this predicate does not accept sink references");
  }
  for (const auto &sink : descriptor.sinks) {
    Status valid = ValidateTypedRef(sink);
    if (!valid.ok()) {
      return valid;
    }
    if (sink_kind.has_value() && sink.kind != *sink_kind) {
      return Status::InvalidArgument(
          "query sink kind disagrees with the predicate");
    }
    Status unique = check_unique(sink);
    if (!unique.ok()) {
      return unique;
    }
  }
  for (const auto &subject : descriptor.subjects) {
    Status valid = ValidateTypedRef(subject);
    if (!valid.ok()) {
      return valid;
    }
    if (subject.kind != core::IdKind::kFunctionVariant) {
      return Status::InvalidArgument("query subject must be a function ref");
    }
    Status unique = check_unique(subject);
    if (!unique.ok()) {
      return unique;
    }
  }
  for (const auto &id : descriptor.boundary_summaries) {
    auto parsed = ParseContentId<BoundarySummaryIdTag>(id.value());
    if (!parsed.ok()) {
      return parsed.status();
    }
  }
  return Status::Ok();
}

std::string EncodeQueryDescriptor(const QueryDescriptor &descriptor) {
  std::string out;
  AppendString(&out, kQueryDescriptorVersion);
  const auto &binding = descriptor.binding;
  AppendU16(&out, binding.snapshot_id.has_value() ? 1 : 0);
  if (binding.snapshot_id.has_value()) {
    AppendString(&out, binding.snapshot_id->value());
  }
  AppendString(&out, binding.repository_id);
  AppendString(&out, binding.base_revision_id);
  AppendString(&out, binding.working_tree_digest);
  AppendU16(&out, binding.program_scope_id.has_value() ? 1 : 0);
  if (binding.program_scope_id.has_value()) {
    AppendString(&out, binding.program_scope_id->value());
  }
  AppendString(&out, binding.build_context_digest);
  AppendU16(&out, binding.native_binding.has_value() ? 1 : 0);
  if (binding.native_binding.has_value()) {
    AppendString(&out, binding.native_binding->analysis_run_id);
    AppendString(&out, binding.native_binding->projection_id);
    AppendString(&out, binding.native_binding->fact_snapshot_id);
  }
  EncodeProducers(&out, binding.provider_versions);

  AppendU16(&out, static_cast<std::uint16_t>(descriptor.predicate));
  AppendString(&out, descriptor.predicate_version);
  EncodeRefs(&out, descriptor.sources);
  EncodeRefs(&out, descriptor.sinks);
  EncodeRefs(&out, descriptor.subjects);
  EncodeProducers(&out, descriptor.producers);

  AppendU32(&out,
            static_cast<std::uint32_t>(descriptor.boundary_summaries.size()));
  for (const auto &id : descriptor.boundary_summaries) {
    AppendString(&out, id.value());
  }

  AppendString(&out, descriptor.scope_digest);
  AppendU64(&out, descriptor.limits.max_examined_rows);
  AppendU64(&out, descriptor.limits.max_proof_edges);
  AppendU32(&out, descriptor.limits.max_functions);
  AppendU32(&out, descriptor.limits.max_sccs);
  return out;
}

StatusOr<QueryId> ComputeQueryId(const QueryDescriptor &descriptor) {
  Status valid = ValidateQueryDescriptor(descriptor);
  if (!valid.ok()) {
    return valid;
  }
  return MakeContentId<QueryIdTag>("veritas.query.descriptor.v1",
                                   EncodeQueryDescriptor(descriptor));
}

// --- Boundary summaries -----------------------------------------------------

Status ValidateBoundarySummary(const BoundarySummary &summary) {
  if (summary.schema_version != kBoundarySummaryVersion) {
    return Status::InvalidArgument("unsupported boundary-summary schema");
  }
  if (summary.function_variant_id.kind != core::IdKind::kFunctionVariant ||
      summary.function_variant_id.digest_hex.empty()) {
    return Status::InvalidArgument(
        "boundary summary has no function variant identity");
  }
  if (!IsDigest(summary.configuration_digest) ||
      !IsDigest(summary.layout_digest)) {
    return Status::InvalidArgument(
        "boundary summary configuration/layout digest is not lowercase 64-hex");
  }
  if (summary.producer_version.empty()) {
    return Status::InvalidArgument("boundary summary has no producer version");
  }
  if (summary.supported_predicates.empty()) {
    return Status::InvalidArgument("boundary summary supports no predicate");
  }
  for (std::size_t i = 0; i < summary.supported_predicates.size(); ++i) {
    const auto predicate = summary.supported_predicates[i];
    if (static_cast<std::size_t>(predicate) >= kQueryPredicateCount) {
      return Status::InvalidArgument(
          "boundary summary names an unknown predicate");
    }
    if (i > 0 && summary.supported_predicates[i - 1] >= predicate) {
      return Status::InvalidArgument(
          "boundary summary predicates are not sorted and unique");
    }
  }
  for (const auto &digest : summary.dependency_digests) {
    if (!IsDigest(digest)) {
      return Status::InvalidArgument(
          "boundary summary dependency digest is not lowercase 64-hex");
    }
  }
  for (const auto &value : summary.interface.parameter_values) {
    if (value.kind != core::IdKind::kValueRef) {
      return Status::InvalidArgument(
          "boundary interface parameter is not a value ref");
    }
  }
  for (const auto &value : summary.interface.returned_values) {
    if (value.kind != core::IdKind::kValueRef) {
      return Status::InvalidArgument(
          "boundary interface return is not a value ref");
    }
  }
  for (const auto &value : summary.interface.escaped_memory) {
    if (value.kind != core::IdKind::kMemoryRef) {
      return Status::InvalidArgument(
          "boundary interface escaped object is not a memory ref");
    }
  }
  for (const auto &site : summary.covered_call_sites) {
    if (site.kind != core::IdKind::kCallSite) {
      return Status::InvalidArgument(
          "boundary covered call site is not a call-site id");
    }
  }

  std::set<RelationId> allowed;
  for (const auto predicate : summary.supported_predicates) {
    allowed.insert(PredicateRelation(predicate));
  }
  for (const auto &fact : summary.facts) {
    auto derived = facts::DeriveFactId(fact.row);
    if (!derived.ok()) {
      return derived.status();
    }
    if (*derived != fact.fact_id) {
      return Status::InvalidArgument(
          "boundary fact id does not match its canonical row");
    }
    if (!allowed.contains(fact.row.relation)) {
      return Status::InvalidArgument(
          "boundary fact relation is not one of its supported predicates");
    }
  }
  return Status::Ok();
}

bool BoundarySupports(const BoundarySummary &summary,
                      QueryPredicate predicate) {
  if (summary.schema_version != kBoundarySummaryVersion ||
      !summary.unknown_frontier.empty()) {
    return false;
  }
  return std::binary_search(summary.supported_predicates.begin(),
                            summary.supported_predicates.end(), predicate);
}

StatusOr<std::string> SerializeBoundarySummary(const BoundarySummary &summary) {
  Status valid = ValidateBoundarySummary(summary);
  if (!valid.ok()) {
    return valid;
  }
  std::string out;
  AppendString(&out, summary.schema_version);
  AppendStableId(&out, summary.function_variant_id);
  AppendString(&out, summary.configuration_digest);
  AppendString(&out, summary.layout_digest);
  AppendU32(&out, static_cast<std::uint32_t>(
                      summary.interface.parameter_values.size()));
  for (const auto &value : summary.interface.parameter_values) {
    AppendStableId(&out, value);
  }
  AppendU32(&out, static_cast<std::uint32_t>(
                      summary.interface.returned_values.size()));
  for (const auto &value : summary.interface.returned_values) {
    AppendStableId(&out, value);
  }
  AppendU32(&out, static_cast<std::uint32_t>(
                      summary.interface.escaped_memory.size()));
  for (const auto &value : summary.interface.escaped_memory) {
    AppendStableId(&out, value);
  }
  AppendU32(&out,
            static_cast<std::uint32_t>(summary.supported_predicates.size()));
  for (const auto predicate : summary.supported_predicates) {
    AppendU16(&out, static_cast<std::uint16_t>(predicate));
  }

  std::vector<facts::AnalysisFact> sorted_facts = summary.facts;
  std::sort(sorted_facts.begin(), sorted_facts.end(),
            [](const facts::AnalysisFact &a, const facts::AnalysisFact &b) {
              return a.fact_id < b.fact_id;
            });
  AppendU32(&out, static_cast<std::uint32_t>(sorted_facts.size()));
  for (const auto &fact : sorted_facts) {
    AppendFact(&out, fact);
  }

  EncodeStrings(&out, summary.assumptions);
  EncodeStrings(&out, summary.unknown_frontier);
  EncodeStrings(&out, summary.dependency_digests);

  std::vector<core::StableId> sites = summary.covered_call_sites;
  std::sort(sites.begin(), sites.end());
  sites.erase(std::unique(sites.begin(), sites.end()), sites.end());
  AppendU32(&out, static_cast<std::uint32_t>(sites.size()));
  for (const auto &site : sites) {
    AppendStableId(&out, site);
  }
  AppendString(&out, summary.producer_version);
  return out;
}

StatusOr<BoundarySummary> ParseBoundarySummary(std::string_view bytes) {
  Reader reader(bytes);
  BoundarySummary summary;
  auto version = reader.String();
  if (!version.ok()) {
    return version.status();
  }
  if (*version != kBoundarySummaryVersion) {
    return Status::InvalidArgument("unsupported boundary-summary schema");
  }
  summary.schema_version = *version;
  auto function_id = reader.StableIdValue();
  if (!function_id.ok()) {
    return function_id.status();
  }
  summary.function_variant_id = *function_id;
  auto config = reader.String();
  if (!config.ok()) {
    return config.status();
  }
  summary.configuration_digest = *config;
  auto layout = reader.String();
  if (!layout.ok()) {
    return layout.status();
  }
  summary.layout_digest = *layout;

  auto read_ids = [&reader](std::vector<core::StableId> *out) -> Status {
    auto count = reader.U32();
    if (!count.ok()) {
      return count.status();
    }
    if (*count > reader.Remaining()) {
      return Status::InvalidArgument("implausible stable-id count");
    }
    for (std::uint32_t i = 0; i < *count; ++i) {
      auto id = reader.StableIdValue();
      if (!id.ok()) {
        return id.status();
      }
      out->push_back(*id);
    }
    return Status::Ok();
  };
  Status read_ok = read_ids(&summary.interface.parameter_values);
  if (read_ok.ok()) {
    read_ok = read_ids(&summary.interface.returned_values);
  }
  if (read_ok.ok()) {
    read_ok = read_ids(&summary.interface.escaped_memory);
  }
  if (!read_ok.ok()) {
    return read_ok;
  }

  auto predicate_count = reader.U32();
  if (!predicate_count.ok()) {
    return predicate_count.status();
  }
  if (*predicate_count > kQueryPredicateCount) {
    return Status::InvalidArgument(
        "boundary summary names too many predicates");
  }
  for (std::uint32_t i = 0; i < *predicate_count; ++i) {
    auto predicate = reader.U16();
    if (!predicate.ok()) {
      return predicate.status();
    }
    if (*predicate >= kQueryPredicateCount) {
      return Status::InvalidArgument("unknown boundary predicate");
    }
    summary.supported_predicates.push_back(
        static_cast<QueryPredicate>(*predicate));
  }

  auto fact_count = reader.U32();
  if (!fact_count.ok()) {
    return fact_count.status();
  }
  if (*fact_count > reader.Remaining()) {
    return Status::InvalidArgument("implausible boundary fact count");
  }
  for (std::uint32_t i = 0; i < *fact_count; ++i) {
    auto fact_id = reader.StableIdValue();
    if (!fact_id.ok()) {
      return fact_id.status();
    }
    if (fact_id->kind != core::IdKind::kFact) {
      return Status::InvalidArgument("boundary fact id is not a fact id");
    }
    auto relation = reader.U16();
    if (!relation.ok()) {
      return relation.status();
    }
    if (*relation >= facts::kRelationCountV2) {
      return Status::InvalidArgument("boundary fact names an unknown relation");
    }
    auto cell_count = reader.U32();
    if (!cell_count.ok()) {
      return cell_count.status();
    }
    if (*cell_count > reader.Remaining()) {
      return Status::InvalidArgument("implausible boundary cell count");
    }
    SemanticRow row;
    row.relation = static_cast<RelationId>(*relation);
    for (std::uint32_t c = 0; c < *cell_count; ++c) {
      auto tag = reader.U16();
      if (!tag.ok()) {
        return tag.status();
      }
      switch (*tag) {
      case 0: {
        auto id = reader.StableIdValue();
        if (!id.ok()) {
          return id.status();
        }
        row.cells.emplace_back(*id);
        break;
      }
      case 1: {
        auto value = reader.U64();
        if (!value.ok()) {
          return value.status();
        }
        row.cells.emplace_back(static_cast<std::int64_t>(*value));
        break;
      }
      case 2: {
        auto value = reader.U64();
        if (!value.ok()) {
          return value.status();
        }
        row.cells.emplace_back(*value);
        break;
      }
      case 3: {
        auto value = reader.String();
        if (!value.ok()) {
          return value.status();
        }
        row.cells.emplace_back(std::move(*value));
        break;
      }
      case 4: {
        auto value = reader.U16();
        if (!value.ok()) {
          return value.status();
        }
        row.cells.emplace_back(
            static_cast<analysis::semantic::DispatchKind>(*value));
        break;
      }
      case 5: {
        auto value = reader.U16();
        if (!value.ok()) {
          return value.status();
        }
        row.cells.emplace_back(
            static_cast<analysis::semantic::AliasKind>(*value));
        break;
      }
      case 6: {
        auto value = reader.U16();
        if (!value.ok()) {
          return value.status();
        }
        row.cells.emplace_back(
            static_cast<analysis::semantic::ByteRangeKind>(*value));
        break;
      }
      case 7: {
        auto value = reader.U16();
        if (!value.ok()) {
          return value.status();
        }
        row.cells.emplace_back(
            static_cast<analysis::semantic::EpistemicState>(*value));
        break;
      }
      default:
        return Status::InvalidArgument("unknown boundary cell tag");
      }
    }
    auto fact = facts::MakeFact(row);
    if (!fact.ok()) {
      return fact.status();
    }
    if (*fact_id != fact->fact_id) {
      return Status::InvalidArgument(
          "boundary fact id does not match its canonical row");
    }
    summary.facts.push_back(std::move(*fact));
  }

  auto read_strings = [&reader](std::vector<std::string> *out) -> Status {
    auto count = reader.U32();
    if (!count.ok()) {
      return count.status();
    }
    if (*count > reader.Remaining()) {
      return Status::InvalidArgument("implausible string count");
    }
    for (std::uint32_t i = 0; i < *count; ++i) {
      auto value = reader.String();
      if (!value.ok()) {
        return value.status();
      }
      out->push_back(std::move(*value));
    }
    return Status::Ok();
  };
  read_ok = read_strings(&summary.assumptions);
  if (read_ok.ok()) {
    read_ok = read_strings(&summary.unknown_frontier);
  }
  if (read_ok.ok()) {
    read_ok = read_strings(&summary.dependency_digests);
  }
  if (!read_ok.ok()) {
    return read_ok;
  }

  read_ok = read_ids(&summary.covered_call_sites);
  if (!read_ok.ok()) {
    return read_ok;
  }
  auto producer = reader.String();
  if (!producer.ok()) {
    return producer.status();
  }
  summary.producer_version = *producer;
  if (!reader.Done()) {
    return Status::InvalidArgument(
        "trailing bytes in boundary-summary encoding");
  }
  Status valid = ValidateBoundarySummary(summary);
  if (!valid.ok()) {
    return valid;
  }
  return summary;
}

StatusOr<BoundarySummaryId>
ComputeBoundarySummaryId(const BoundarySummary &summary) {
  auto bytes = SerializeBoundarySummary(summary);
  if (!bytes.ok()) {
    return bytes.status();
  }
  return MakeContentId<BoundarySummaryIdTag>(
      "veritas.query.boundary-summary.v1", *bytes);
}

// --- Dependency snapshot ----------------------------------------------------

Status ValidateDependencySnapshot(const DependencySnapshot &snapshot) {
  if (snapshot.revision_id.kind != core::IdKind::kRevision ||
      snapshot.revision_id.digest_hex.empty()) {
    return Status::InvalidArgument("dependency snapshot has no revision id");
  }
  Status semantics = ValidateSemantics(snapshot.semantics);
  if (!semantics.ok()) {
    return semantics;
  }
  std::set<std::string> functions;
  for (const auto &artifact : snapshot.summaries) {
    const auto &identity = summary::Identity(artifact);
    auto parsed = core::ParseStableId(identity.function_variant_id());
    if (!parsed.ok() || parsed->kind != core::IdKind::kFunctionVariant) {
      return Status::InvalidArgument(
          "dependency snapshot summary has an invalid function identity");
    }
    if (!functions.insert(identity.function_variant_id()).second) {
      return Status::InvalidArgument(
          "dependency snapshot contains a duplicate function summary");
    }
    auto serialized = summary::SerializeSummaryArtifact(artifact);
    if (!serialized.ok()) {
      return serialized.status();
    }
  }
  std::set<std::string> boundary_ids;
  for (const auto &boundary : snapshot.boundary_summaries) {
    Status valid = ValidateBoundarySummary(boundary);
    if (!valid.ok()) {
      return valid;
    }
    auto id = ComputeBoundarySummaryId(boundary);
    if (!id.ok()) {
      return id.status();
    }
    if (!boundary_ids.insert(id->value()).second) {
      return Status::InvalidArgument(
          "dependency snapshot contains a duplicate boundary summary");
    }
  }
  return Status::Ok();
}

std::string
ComputeDependencySnapshotDigest(const DependencySnapshot &snapshot) {
  std::string out;
  AppendString(&out, "veritas.query.dependency-snapshot.v1");
  AppendStableId(&out, snapshot.revision_id);
  AppendStableId(&out, snapshot.semantics.build_variant_id);
  AppendString(&out, snapshot.semantics.summary_schema_version);
  AppendString(&out, snapshot.semantics.relation_schema_version);
  AppendString(&out, snapshot.semantics.rule_bundle_version);
  AppendString(&out, snapshot.semantics.model_bundle_version);
  AppendString(&out, snapshot.semantics.svf_configuration_hash);
  AppendString(&out, snapshot.semantics.wpa_configuration_hash);

  std::vector<const summary::SummaryArtifact *> sorted;
  sorted.reserve(snapshot.summaries.size());
  for (const auto &artifact : snapshot.summaries) {
    sorted.push_back(&artifact);
  }
  std::sort(
      sorted.begin(), sorted.end(),
      [](const summary::SummaryArtifact *a, const summary::SummaryArtifact *b) {
        return summary::Identity(*a).function_variant_id() <
               summary::Identity(*b).function_variant_id();
      });
  AppendU32(&out, static_cast<std::uint32_t>(sorted.size()));
  static constexpr std::array<summary::v1::ComponentKind, 4> kComponents = {
      summary::v1::COMPONENT_KIND_CALLS,
      summary::v1::COMPONENT_KIND_MEMORY_EFFECTS,
      summary::v1::COMPONENT_KIND_VALUE_FLOW,
      summary::v1::COMPONENT_KIND_UNKNOWNS,
  };
  for (const auto *artifact : sorted) {
    const auto &identity = summary::Identity(*artifact);
    AppendString(&out, identity.repository_id());
    AppendString(&out, identity.revision_id());
    AppendString(&out, identity.build_variant_id());
    AppendString(&out, identity.function_variant_id());
    AppendString(&out, identity.function_body_id());
    AppendString(&out, summary::SchemaVersion(*artifact));
    for (const auto kind : kComponents) {
      const auto digest = summary::ComputeComponentDigest(kind, *artifact);
      AppendU16(&out, static_cast<std::uint16_t>(kind));
      out.append(reinterpret_cast<const char *>(digest.semantic_hash.data()),
                 digest.semantic_hash.size());
      AppendU32(&out, static_cast<std::uint32_t>(digest.item_count));
    }
  }

  std::vector<std::string> boundary_ids;
  boundary_ids.reserve(snapshot.boundary_summaries.size());
  for (const auto &boundary : snapshot.boundary_summaries) {
    auto id = ComputeBoundarySummaryId(boundary);
    // Validation above established the identity; an invalid summary cannot
    // reach here in practice, and an empty id keeps the digest deterministic.
    boundary_ids.push_back(id.ok() ? id->value() : std::string());
  }
  std::sort(boundary_ids.begin(), boundary_ids.end());
  AppendU32(&out, static_cast<std::uint32_t>(boundary_ids.size()));
  for (const auto &id : boundary_ids) {
    AppendString(&out, id);
  }
  return core::DigestToHex(core::ComputeSHA256(std::as_bytes(std::span(out))));
}

// --- Coverage, cost and artifact identity -----------------------------------

std::string_view FrontierKindName(QueryFrontierKind kind) {
  switch (kind) {
  case QueryFrontierKind::kOversizedScc:
    return "oversized-scc";
  case QueryFrontierKind::kSccBudget:
    return "scc-budget";
  case QueryFrontierKind::kUnresolvedCall:
    return "unresolved-call";
  case QueryFrontierKind::kExternalBoundary:
    return "external-boundary";
  case QueryFrontierKind::kMissingBoundary:
    return "missing-boundary";
  case QueryFrontierKind::kUnknownEffect:
    return "unknown-effect";
  case QueryFrontierKind::kRowBudget:
    return "row-budget";
  case QueryFrontierKind::kTimeout:
    return "timeout";
  case QueryFrontierKind::kProofBudget:
    return "proof-budget";
  case QueryFrontierKind::kMissingPremise:
    return "missing-premise";
  }
  return "invalid";
}

void CanonicalizeQueryFrontier(std::vector<QueryFrontierEntry> *frontier) {
  std::sort(frontier->begin(), frontier->end(),
            [](const QueryFrontierEntry &a, const QueryFrontierEntry &b) {
              if (a.kind != b.kind) {
                return a.kind < b.kind;
              }
              const std::string a_ref =
                  a.ref.has_value() ? core::ToString(*a.ref) : std::string();
              const std::string b_ref =
                  b.ref.has_value() ? core::ToString(*b.ref) : std::string();
              if (a_ref != b_ref) {
                return a_ref < b_ref;
              }
              return a.detail < b.detail;
            });
  frontier->erase(
      std::unique(frontier->begin(), frontier->end(),
                  [](const QueryFrontierEntry &a, const QueryFrontierEntry &b) {
                    return a.kind == b.kind && a.ref == b.ref &&
                           a.detail == b.detail;
                  }),
      frontier->end());
}

std::string_view QueryOutcomeName(QueryOutcome outcome) {
  switch (outcome) {
  case QueryOutcome::kComplete:
    return "complete";
  case QueryOutcome::kIncomplete:
    return "incomplete";
  }
  return "invalid";
}

bool IsCompleteAnswer(const QueryArtifact &artifact) {
  return artifact.coverage.outcome == QueryOutcome::kComplete &&
         artifact.coverage.frontier.empty() &&
         !artifact.coverage.proof_truncated;
}

StatusOr<QueryArtifactId>
ComputeQueryArtifactId(const QueryId &query_id, const QueryCoverage &coverage,
                       std::span<const facts::AnalysisFact> results,
                       std::span<const facts::AnalysisFact> premises,
                       std::span<const facts::WitnessEdge> proof) {
  std::string out;
  AppendString(&out, "veritas.query.artifact.v1");
  AppendString(&out, query_id.value());
  AppendU16(&out, static_cast<std::uint16_t>(coverage.outcome));
  AppendU16(&out, coverage.predicate_supported ? 1 : 0);
  AppendU16(&out, coverage.scope_closed ? 1 : 0);
  AppendU16(&out, coverage.proof_truncated ? 1 : 0);
  EncodeFrontier(&out, coverage.frontier);
  EncodeStrings(&out, coverage.assumptions);
  AppendU64(&out, coverage.examined_rows);
  AppendU64(&out, coverage.proof_edges);

  auto sorted_facts = [](std::span<const facts::AnalysisFact> facts) {
    std::vector<facts::AnalysisFact> sorted(facts.begin(), facts.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const facts::AnalysisFact &a, const facts::AnalysisFact &b) {
                return a.fact_id < b.fact_id;
              });
    return sorted;
  };
  const auto sorted_results = sorted_facts(results);
  const auto sorted_premises = sorted_facts(premises);
  AppendU32(&out, static_cast<std::uint32_t>(sorted_results.size()));
  for (const auto &fact : sorted_results) {
    AppendFact(&out, fact);
  }
  AppendU32(&out, static_cast<std::uint32_t>(sorted_premises.size()));
  for (const auto &fact : sorted_premises) {
    AppendFact(&out, fact);
  }

  std::vector<facts::WitnessEdge> sorted_proof(proof.begin(), proof.end());
  std::sort(sorted_proof.begin(), sorted_proof.end());
  AppendU32(&out, static_cast<std::uint32_t>(sorted_proof.size()));
  for (const auto &edge : sorted_proof) {
    AppendWitness(&out, edge);
  }
  return MakeContentId<QueryArtifactIdTag>("veritas.query.artifact.v1", out);
}

} // namespace veritas::query
