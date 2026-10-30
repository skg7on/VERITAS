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

#include "veritas/review/Verification.h"
namespace veritas::review {
namespace {
using O = Json::Object;
std::string Text(const O &o, std::string_view key) { return *o.getString(key); }
Status Bad() {
  return Status::InvalidArgument(
      "verification authority/binding/witness mismatch");
}
StatusOr<std::string> Identity(const Json &j, std::string_view field,
                               bool obligation) {
  if (!j.getAsObject())
    return Bad();
  auto o = *j.getAsObject();
  o.erase(std::string(field));
  if (obligation) {
    auto id = MakeArtifactId<ObligationTag>(o);
    if (!id.ok())
      return id.status();
    return id->value();
  }
  auto id = MakeArtifactId<VerificationTag>(o);
  if (!id.ok())
    return id.status();
  return id->value();
}
} // namespace
StatusOr<std::string> VerificationObligationId(const Json &q) {
  return Identity(q, "obligation_id", true);
}
StatusOr<std::string> VerificationResultId(const Json &r) {
  return Identity(r, "verification_id", false);
}
StatusOr<std::string> VerificationRequestDigest(const Json &q) {
  auto bytes = CanonicalJson(q);
  if (!bytes.ok())
    return bytes.status();
  return HashBytes(*bytes);
}
Status ValidateVerificationRequest(const Json &q) {
  auto st = ValidateWireType(q, "VerificationRequest");
  if (!st.ok())
    return st;
  const auto &o = *q.getAsObject();
  const auto &d = *o.getObject("execution_domain");
  const auto &v = *o.getObject("typed_operands");
  if (Text(o, "obligation_id") != *VerificationObligationId(q) ||
      *d.get("snapshot_id") != *o.getObject("binding")->get("snapshot_id") ||
      Text(d, "input_digest") != Text(v, "payload_digest") ||
      Text(d, "harness_id") != Text(v, "harness_id") ||
      *v.get("requested_bytes")->getAsUINT64() >
          *v.get("readable_bytes")->getAsUINT64())
    return Bad();
  return Status::Ok();
}
Status ValidateVerificationResult(const Json &q, const Json &r,
                                  const VerificationPolicy &p) {
  auto st = ValidateVerificationRequest(q);
  if (!st.ok())
    return st;
  st = ValidateWireType(r, "VerificationResult");
  if (!st.ok())
    return st;
  const auto &a = *q.getAsObject();
  const auto &b = *r.getAsObject();
  if (Text(b, "verification_id") != *VerificationResultId(r) ||
      Text(b, "request_digest") != *VerificationRequestDigest(q))
    return Bad();
  for (auto key : {"binding", "predicate_kind", "typed_operands",
                   "execution_domain", "assumptions"})
    if (*a.get(key) != *b.get(key))
      return Bad();
  const auto &producer = *b.getObject("producer");
  if (!p.producers.contains({Text(producer, "name"), Text(producer, "version"),
                             Text(producer, "config_digest")}) ||
      Text(producer, "name") != Text(a, "backend_id") ||
      Text(producer, "version") != Text(a, "backend_version"))
    return Bad();
  const auto obligation = Text(a, "obligation_id"),
             result = Text(b, "verification_id");
  for (const auto &item : *b.getArray("assumptions")) {
    auto s = *item.getAsString();
    if (!p.admitted_assumptions.contains(s) ||
        s.find(obligation) != std::string::npos ||
        s.find(result) != std::string::npos)
      return Bad();
  }
  std::set<std::string> deps, inputs;
  for (const auto &item : *b.getArray("input_dependencies")) {
    const auto &dep = *item.getAsObject();
    auto key = Text(dep, "artifact_id");
    auto found = p.artifacts.find(key);
    if (!deps.insert(key).second || key == obligation || key == result ||
        found == p.artifacts.end() ||
        HashBytes(found->second) != Text(dep, "content_digest"))
      return Bad();
  }
  for (const auto &item : *a.getArray("input_artifact_ids"))
    inputs.insert(*item.getAsString());
  for (const auto &key : inputs)
    if (!deps.contains(key))
      return Bad();
  for (auto field : {"certificate_artifact_ids", "counterexample_artifact_ids"})
    for (const auto &item : *b.getArray(field))
      if (!deps.contains(*item.getAsString()))
        return Bad();
  const auto status = Text(b, "status");
  if (status == "PROVED" || status == "REFUTED") {
    const auto &coverage = *b.getObject("coverage");
    const auto &witness =
        *b.getArray(status == "PROVED" ? "certificate_artifact_ids"
                                       : "counterexample_artifact_ids");
    if (Text(b, "operational_outcome") != "COMPLETED" || witness.empty() ||
        !*coverage.getBoolean("supported") ||
        Text(coverage, "traversal") != "COMPLETE" ||
        !coverage.getArray("frontier_refs")->empty() ||
        !p.authorized_results.contains(result))
      return Bad();
    for (const auto &item : witness)
      if (inputs.contains(*item.getAsString()))
        return Bad();
    const auto &v = *a.getObject("typed_operands");
    bool holds = *v.get("requested_bytes")->getAsUINT64() <=
                 *v.get("capacity_bytes")->getAsUINT64();
    if ((status == "PROVED") != holds)
      return Bad();
  }
  return Status::Ok();
}
StatusOr<ReviewDisposition> ApplyVerification(ReviewDisposition state,
                                              const Json &q, const Json &r,
                                              const VerificationPolicy &p) {
  auto st = ValidateVerificationResult(q, r, p);
  if (!st.ok())
    return st;
  auto status = Text(*r.getAsObject(), "status");
  if (status == "REFUTED")
    return ReviewDisposition::VerifiedFinding;
  if (status == "PROVED")
    return ReviewDisposition::NoDefectObserved;
  return state;
}
} // namespace veritas::review
