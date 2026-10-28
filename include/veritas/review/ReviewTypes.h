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

#ifndef VERITAS_REVIEW_REVIEW_TYPES_H_
#define VERITAS_REVIEW_REVIEW_TYPES_H_
#include "veritas/core/Status.h"

#include <concepts>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
namespace veritas::review {

struct Json;
struct JsonObject : std::map<std::string, Json, std::less<>> {
  using map::map;
  const Json *get(std::string_view key) const;
  std::optional<std::string> getString(std::string_view key) const;
  std::optional<bool> getBoolean(std::string_view key) const;
  const JsonObject *getObject(std::string_view key) const;
  const std::vector<Json> *getArray(std::string_view key) const;
};
struct Json {
  using Object = JsonObject;
  using Array = std::vector<Json>;
  enum Kind { Null, Boolean, Number, String, List, Map };
  std::variant<std::nullptr_t, bool, int64_t, uint64_t, std::string, Array,
               Object>
      data;
  Json() : data(nullptr) {}
  Json(std::nullptr_t) : data(nullptr) {}
  Json(bool b) : data(b) {}
  template <std::signed_integral T>
    requires(!std::same_as<T, bool>)
  Json(T n) : data(int64_t(n)) {}
  template <std::unsigned_integral T>
    requires(!std::same_as<T, bool>)
  Json(T n) : data(uint64_t(n)) {}
  Json(double) = delete;
  Json(const char *s) : data(std::string(s)) {}
  Json(std::string s) : data(std::move(s)) {}
  Json(std::string_view s) : data(std::string(s)) {}
  Json(Array a) : data(std::move(a)) {}
  Json(Object o) : data(std::move(o)) {}
  Kind kind() const;
  std::optional<bool> getAsBoolean() const;
  std::optional<int64_t> getAsInteger() const;
  std::optional<uint64_t> getAsUINT64() const;
  std::optional<std::string> getAsString() const;
  const Object *getAsObject() const { return std::get_if<Object>(&data); }
  const Array *getAsArray() const { return std::get_if<Array>(&data); }
  bool operator==(const Json &) const = default;
};
inline constexpr std::string_view kProtocol = "veritas.review.backend.v2";
#define VERITAS_REVIEW_TAG(Name, Kind)                                         \
  struct Name##Tag {                                                           \
    static constexpr std::string_view kind = Kind;                             \
  };
VERITAS_REVIEW_TAG(Snapshot, "review-snapshot")
VERITAS_REVIEW_TAG(ProgramScope, "program-scope")
VERITAS_REVIEW_TAG(Context, "context")
VERITAS_REVIEW_TAG(Candidate, "candidate")
VERITAS_REVIEW_TAG(LocalEvidence, "local-evidence")
VERITAS_REVIEW_TAG(Obligation, "obligation")
VERITAS_REVIEW_TAG(Verification, "verification")
VERITAS_REVIEW_TAG(ReviewReport, "review-report")
VERITAS_REVIEW_TAG(StructuralIndex, "structural-index")
#undef VERITAS_REVIEW_TAG
// Separate from core::StableId; only the matching tagged parser can create one.
template <class Tag> class ArtifactId {
public:
  const std::string &value() const { return value_; }
  bool operator==(const ArtifactId &) const = default;

private:
  explicit ArtifactId(std::string value) : value_(std::move(value)) {}
  std::string value_;
  template <class T>
  friend StatusOr<ArtifactId<T>> ParseArtifactId(std::string_view);
};
bool IsDigest(std::string_view);
std::string HashBytes(std::string_view);
template <class Tag>
StatusOr<ArtifactId<Tag>> ParseArtifactId(std::string_view value) {
  const std::string prefix = std::string(Tag::kind) + ":sha256:";
  if (!value.starts_with(prefix) || !IsDigest(value.substr(prefix.size())))
    return Status::InvalidArgument("wrong artifact kind or malformed ID");
  return ArtifactId<Tag>(std::string(value));
}
using SnapshotId = ArtifactId<SnapshotTag>;
using ProgramScopeId = ArtifactId<ProgramScopeTag>;
using ContextId = ArtifactId<ContextTag>;
using CandidateId = ArtifactId<CandidateTag>;
using LocalEvidenceId = ArtifactId<LocalEvidenceTag>;
using ObligationId = ArtifactId<ObligationTag>;
using VerificationId = ArtifactId<VerificationTag>;
using ReviewReportId = ArtifactId<ReviewReportTag>;
using StructuralIndexId = ArtifactId<StructuralIndexTag>;
// Invalid UTF-8, duplicate keys, floats, integer overflow and excessive nesting
// fail with InvalidArgument. All JSON parse/encode operations are bounded.
StatusOr<Json> ParseStrictJson(std::string_view);
StatusOr<std::string> CanonicalJson(const Json &);
Status ValidateWireType(const Json &, std::string_view schema_name);
template <class Tag>
StatusOr<ArtifactId<Tag>> MakeArtifactId(const Json &semantic) {
  auto bytes = CanonicalJson(semantic);
  if (!bytes.ok())
    return bytes.status();
  std::string domain = "veritas.review." + std::string(Tag::kind) + ".v1";
  domain.push_back('\0');
  return ParseArtifactId<Tag>(std::string(Tag::kind) +
                              ":sha256:" + HashBytes(domain + *bytes));
}
struct NativeBinding {
  std::string analysis_run_id, projection_id, fact_snapshot_id;
  bool operator==(const NativeBinding &) const = default;
};
struct ProviderVersion {
  std::string name, version, config_digest;
  auto operator<=>(const ProviderVersion &) const = default;
};
struct ReviewBinding {
  std::optional<SnapshotId> snapshot_id;
  std::string repository_id, base_revision_id, working_tree_digest;
  std::optional<ProgramScopeId> program_scope_id;
  std::string build_context_digest;
  std::optional<NativeBinding> native_binding;
  std::vector<ProviderVersion> provider_versions;
  bool operator==(const ReviewBinding &) const = default;
};
struct ProgramScope {
  std::vector<std::string> member_tu_digests, dependency_artifact_digests;
  std::string target_digest;
  std::string membership_source = "OUTPUT_HINT", coverage = "OPEN";
  std::vector<std::string> assumptions, frontier;
};
struct EntityRef {
  StructuralIndexId index_id;
  uint32_t ordinal;
  bool operator==(const EntityRef &) const = default;
};
struct Region {
  uint64_t start, end;
};
struct EntityBinding {
  EntityRef entity_ref;
  uint64_t start, end;
  std::string kind;
};
struct ContextArtifact {
  ContextId context_id;
  ReviewBinding binding;
  std::string kind, path, content_digest;
  uint64_t start, end;
  std::string excerpt;
  std::vector<Region> omitted_regions;
  std::string selection_reason;
  std::vector<EntityBinding> entity_bindings;
};
struct AnchorRef {
  ContextId context_id;
  uint64_t start, end;
  std::optional<EntityRef> entity_ref;
};
struct CandidateProposal {
  ReviewBinding binding;
  std::string origin, claim_kind;
  std::vector<AnchorRef> anchor_refs;
  std::vector<ContextId> contract_refs;
  std::string question;
  std::vector<std::string> assumptions;
};
struct Coverage {
  bool supported = false, scope_closed = false;
  std::string traversal = "INCOMPLETE";
  std::vector<std::string> frontier_refs, reasons;
  uint64_t examined = 0;
};
struct Usage {
  uint64_t wall_ms = 0, cpu_ms = 0, peak_rss_bytes = 0, output_bytes = 0,
           examined = 0;
};
struct BackendEnvelope {
  std::string request_id, operation;
  std::optional<bool> ok;
  std::optional<ReviewBinding> binding;
  Json document;
};
// Unknown/version-mismatched requests and inconsistent success/failure branches
// are InvalidArgument. expected_* pin the transport correlation when supplied.
StatusOr<BackendEnvelope> DecodeV2(std::string_view,
                                   std::string_view expected_request_id = {},
                                   std::string_view expected_operation = {});
Json ToJson(const ReviewBinding &);
Json ToJson(const ProgramScope &);
StatusOr<ReviewBinding> DecodeReviewBinding(const Json &);
} // namespace veritas::review
#endif
