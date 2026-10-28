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

#include "veritas/review/LocalEvidence.h"
#include "StructuralIndexInternal.h"
#include <algorithm>
#include <chrono>
#include <sys/resource.h>
namespace veritas::review {
namespace {
Json ProducerJson(const ProviderVersion &p) {
  return Json::Object{{"name", p.name},
                      {"version", p.version},
                      {"config_digest", p.config_digest}};
}
Json Semantic(const LocalEvidence &e) {
  Json::Array observations, assumptions;
  for (const auto &a : e.assumptions)
    assumptions.push_back(a);
  for (const auto &o : e.observations) {
    Json value;
    if (const auto *operands = std::get_if<CopyOperands>(&o.value))
      value = Json::Object{{"destination", RefJson(operands->destination)},
                           {"source", RefJson(operands->source)},
                           {"length", RefJson(operands->length)}};
    else if (const auto *extent = std::get_if<ByteExtent>(&o.value))
      value = Json::Object{{"bytes", extent->bytes}};
    else {
      const auto &guard = std::get<LocalGuard>(o.value);
      Json::Array refs;
      for (const auto &ref : guard.operand_refs)
        refs.push_back(RefJson(ref));
      value = Json::Object{{"condition_ref", RefJson(guard.condition_ref)},
                           {"dominates", guard.dominates},
                           {"relation", "SIZE_LE_EXTENT"},
                           {"operand_refs", std::move(refs)}};
    }
    Json::Array sources;
    for (const auto &ref : o.source_refs)
      sources.push_back(ref.value());
    observations.push_back(Json::Object{{"slot", o.slot},
                                        {"subject_ref", RefJson(o.subject_ref)},
                                        {"value", std::move(value)},
                                        {"source_refs", std::move(sources)}});
  }
  return Json::Object{{"binding", ToJson(e.binding)},
                      {"predicate_kind", e.predicate_kind},
                      {"callsite_ref", RefJson(e.callsite_ref)},
                      {"observations", std::move(observations)},
                      {"assumptions", std::move(assumptions)},
                      {"coverage", CoverageJson(e.coverage)},
                      {"producer", ProducerJson(e.producer)}};
}
} // namespace
Json ToJson(const LocalEvidence &e) {
  auto json = Semantic(e);
  auto &object = std::get<Json::Object>(json.data);
  object["local_evidence_id"] = e.local_evidence_id.value();
  object["usage"] = UsageJson(e.usage);
  return json;
}
StatusOr<LocalEvidence> InvestigateLocal(const LocalRequest &request) {
  const auto start = std::chrono::steady_clock::now();
  rusage cpu_start{};
  getrusage(RUSAGE_SELF, &cpu_start);
  const auto &data = *request.index.data_;
  if (request.binding != data.binding ||
      request.callsite_ref.index_id != data.id)
    return Status::InvalidArgument(
        "local request does not match pinned binding/index");
  if (request.predicate_kind != "copy_bound.v1" || !request.max_examined ||
      request.requested_slots.empty() ||
      !std::is_sorted(request.requested_slots.begin(),
                      request.requested_slots.end()) ||
      std::adjacent_find(request.requested_slots.begin(),
                         request.requested_slots.end()) !=
          request.requested_slots.end())
    return Status::InvalidArgument("unregistered predicate, nonpositive budget "
                                   "or unsorted/duplicate slots");
  for (const auto &slot : request.requested_slots)
    if (slot != "OPERANDS" && slot != "EXTENT" && slot != "GUARD")
      return Status::InvalidArgument("unknown local slot");
  auto found = data.copies.find(request.callsite_ref.ordinal);
  if (found == data.copies.end() ||
      request.callsite_ref.ordinal >= data.entities.size())
    return Status::InvalidArgument("unknown/non-callsite entity ref");
  const auto &copy = found->second;
  const auto &call = data.entities[request.callsite_ref.ordinal];
  auto placeholder = MakeArtifactId<LocalEvidenceTag>(nullptr);
  if (!placeholder.ok())
    return placeholder.status();
  LocalEvidence result{*placeholder,
                       data.binding,
                       request.predicate_kind,
                       request.callsite_ref,
                       {},
                       {"compiler target is x86_64-unknown-linux-gnu; include "
                        "inputs are captured only",
                        "structural observations do not decide copy "
                        "feasibility or execution-domain safety"},
                       data.coverage,
                       data.producer,
                       {}};
  result.assumptions.insert(result.assumptions.end(),
                            data.snapshot.program_scope().assumptions.begin(),
                            data.snapshot.program_scope().assumptions.end());
  result.coverage.examined = 0;
  auto unsupported = [&](const std::string &slot, const std::string &reason) {
    result.coverage.supported = false;
    result.coverage.frontier_refs.push_back(
        data.id.value() + "/" + std::to_string(call.ordinal) + "/" + slot);
    result.coverage.reasons.push_back(reason);
  };
  auto ref = [&](uint32_t ordinal) { return EntityRef{data.id, ordinal}; };
  for (const auto &slot : request.requested_slots) {
    if (result.coverage.examined >= request.max_examined) {
      result.coverage.traversal = "INCOMPLETE";
      unsupported(slot, "local examination budget exhausted");
      continue;
    }
    ++result.coverage.examined;
    if (!copy.admitted || call.operands.size() != 3) {
      unsupported(slot, "unregistered, macro, cast or incomplete copy form");
      continue;
    }
    const auto &destination = data.entities[call.operands[0]];
    if (slot == "OPERANDS") {
      result.observations.push_back(
          {slot,
           request.callsite_ref,
           CopyOperands{ref(call.operands[0]), ref(call.operands[1]),
                        ref(call.operands[2])},
           {call.source_ref}});
    } else if (slot == "EXTENT") {
      if (copy.extent)
        result.observations.push_back({slot,
                                       ref(destination.ordinal),
                                       ByteExtent{*copy.extent},
                                       {destination.source_ref}});
      else
        unsupported(slot, "destination is not a direct local constant integer "
                          "array; pointer/VLA/heap extent unsupported");
    } else {
      if (copy.guards.empty())
        unsupported(slot, "no admitted unchanged unsigned-size local guard");
      for (size_t i = 0; i < copy.guards.size(); ++i) {
        if (i > 0) {
          if (result.coverage.examined >= request.max_examined) {
            result.coverage.traversal = "INCOMPLETE";
            unsupported(slot, "local guard examination budget exhausted");
            break;
          }
          ++result.coverage.examined;
        }
        const auto &guard = copy.guards[i];
        result.observations.push_back(
            {slot,
             request.callsite_ref,
             LocalGuard{ref(guard.condition),
                        guard.dominates,
                        {ref(call.operands[2]), ref(call.operands[0])}},
             {data.entities[guard.condition].source_ref,
              destination.source_ref}});
      }
    }
  }
  auto id = MakeArtifactId<LocalEvidenceTag>(Semantic(result));
  if (!id.ok())
    return id.status();
  result.local_evidence_id = *id;
  result.usage.examined = result.coverage.examined;
  result.usage.wall_ms = static_cast<uint64_t>(
      (std::chrono::duration_cast<std::chrono::microseconds>(
           std::chrono::steady_clock::now() - start)
           .count() +
       999) /
      1000);
  rusage cpu_end{};
  getrusage(RUSAGE_SELF, &cpu_end);
  auto micros = [](const rusage &u) {
    return static_cast<uint64_t>(u.ru_utime.tv_sec + u.ru_stime.tv_sec) *
               1000000 +
           u.ru_utime.tv_usec + u.ru_stime.tv_usec;
  };
  result.usage.cpu_ms = (micros(cpu_end) - micros(cpu_start) + 999) / 1000;
  // Count the cost fields themselves. The decimal byte count reaches a fixed
  // point in at most a few iterations, including across digit boundaries.
  for (;;) {
    auto json = CanonicalJson(ToJson(result));
    if (!json.ok())
      return json.status();
    if (result.usage.output_bytes == json->size())
      break;
    result.usage.output_bytes = json->size();
  }
  auto valid = ValidateWireType(ToJson(result), "LocalEvidence");
  if (!valid.ok())
    return valid;
  return result;
}
} // namespace veritas::review
