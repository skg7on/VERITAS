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

// QueryTypes.h — the demand-scoped query identity, boundary-summary and
// artifact vocabulary.
//
// A query is a bounded question about one registered semantic predicate:
// which endpoints, under which producer/rule/model/configuration versions, in
// which admitted scope, under which deterministic work limits. The descriptor
// is content-addressed; QueryId hashes its canonical bytes and never an
// analysis run id. Runtime wall/memory limits are operational abort policies
// and are deliberately excluded from that identity.
//
// `boundary-summary.v1` is a versioned artifact of its own. It records a
// function's conservative externally visible interface so an admitted query
// can cite it instead of expanding an unadmitted successor SCC. It never
// reinterprets `summary.v2` bytes and never carries model-inferred content as
// verified fact.

#ifndef VERITAS_QUERY_QUERY_TYPES_H_
#define VERITAS_QUERY_QUERY_TYPES_H_

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "veritas/core/Hash.h"
#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/facts/AnalysisFact.h"
#include "veritas/facts/AnalysisRun.h"
#include "veritas/facts/Witness.h"
#include "veritas/review/ReviewTypes.h"
#include "veritas/summary/SummaryArtifact.h"
#include "veritas/wpa/WpaComponent.h"

namespace veritas::query {

// --- Versions ---------------------------------------------------------------

inline constexpr std::string_view kQueryDescriptorVersion =
    "query.descriptor.v1";
inline constexpr std::string_view kQueryPredicateVersion = "query.predicate.v1";
inline constexpr std::string_view kBoundarySummaryVersion =
    "boundary-summary.v1";

// --- Tagged content identities ---------------------------------------------

struct QueryIdTag {
  static constexpr std::string_view kind = "query";
};

struct QueryArtifactIdTag {
  static constexpr std::string_view kind = "query-artifact";
};

struct BoundarySummaryIdTag {
  static constexpr std::string_view kind = "boundary-summary";
};

// A separate namespace from core::StableId: only the matching parser can
// construct one, so a QueryId can never be compared against a StableId.
template <class Tag> class ContentId {
public:
  const std::string &value() const { return value_; }

  bool operator==(const ContentId &) const = default;
  auto operator<=>(const ContentId &) const = default;

private:
  explicit ContentId(std::string value) : value_(std::move(value)) {}

  std::string value_;

  template <class T>
  friend StatusOr<ContentId<T>> ParseContentId(std::string_view);

  template <class T>
  friend StatusOr<ContentId<T>> MakeContentId(std::string_view,
                                              std::string_view);
};

using QueryId = ContentId<QueryIdTag>;
using QueryArtifactId = ContentId<QueryArtifactIdTag>;
using BoundarySummaryId = ContentId<BoundarySummaryIdTag>;

// Parses `<kind>:sha256:<64 lowercase hex>`. A wrong kind, algorithm, or
// malformed digest returns InvalidArgument.
template <class Tag>
StatusOr<ContentId<Tag>> ParseContentId(std::string_view text) {
  const std::string prefix = std::string(Tag::kind) + ":sha256:";
  if (!text.starts_with(prefix) || text.size() != prefix.size() + 64) {
    return Status::InvalidArgument("wrong content-id kind or malformed digest");
  }
  for (const char c : text.substr(prefix.size())) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return Status::InvalidArgument("content-id digest is not lowercase hex");
    }
  }
  return ContentId<Tag>(std::string(text));
}

// Hashes `domain` (with a NUL separator) followed by `bytes`.
template <class Tag>
StatusOr<ContentId<Tag>> MakeContentId(std::string_view domain,
                                       std::string_view bytes) {
  if (domain.empty()) {
    return Status::InvalidArgument("content-id domain must not be empty");
  }
  std::string preimage;
  preimage.reserve(domain.size() + 1 + bytes.size());
  preimage.append(domain);
  preimage.push_back('\0');
  preimage.append(bytes);
  const auto digest = core::ComputeSHA256(std::as_bytes(std::span(preimage)));
  return ParseContentId<Tag>(std::string(Tag::kind) +
                             ":sha256:" + core::DigestToHex(digest));
}

// --- Predicates -------------------------------------------------------------

// The registered query predicates. The spelling of each enumerator's text is
// part of the query-seed contract in logic/query/query_seed.v1.manifest.
enum class QueryPredicate : std::uint8_t {
  kReachableCall = 0,
  kGlobalFlow = 1,
  kMayRead = 2,
  kMayWrite = 3,
  kUnknownEffect = 4,
};

inline constexpr std::size_t kQueryPredicateCount = 5;

std::string_view PredicateName(QueryPredicate predicate);
StatusOr<QueryPredicate> ParsePredicate(std::string_view name);

// The IDB relation this predicate asks about, and the WPA component that
// derives it. Both come from the registered relations.v2 registry.
facts::RelationId PredicateRelation(QueryPredicate predicate);
wpa::WpaComponentKind PredicateComponent(QueryPredicate predicate);
core::IdKind PredicateSourceKind(QueryPredicate predicate);
// nullopt for a predicate whose sink column carries no stable identity
// (UnknownEffect's reason text).
std::optional<core::IdKind> PredicateSinkKind(QueryPredicate predicate);
// Path-local predicates derive facts only from paths between their endpoints;
// an excluded callee that cannot reach the sink therefore cannot contribute.
bool PredicateIsPathLocal(QueryPredicate predicate);
// Memory/effect predicates can be affected by shared or escaped memory, so
// their scope stays open unless a boundary contract covers it.
bool PredicateUsesMemory(QueryPredicate predicate);
// The base relations the component consumes locally, recorded in the plan.
std::vector<facts::RelationId>
PredicateLocalRelations(QueryPredicate predicate);

// The compiled query-seed registry. It mirrors
// logic/query/query_seed.v1.manifest, which is the human-readable contract for
// which baseline bundle a seeded kernel reuses. It names the baseline bundle
// and never replaces it: the seed restricts the admitted SCC set and the typed
// endpoint bindings, while the bundle's rules and epistemic weakening stay
// untouched.
struct QuerySeedSpec {
  QueryPredicate predicate = QueryPredicate::kReachableCall;
  std::string seed_version = "query-seed.v1";
  wpa::WpaComponentKind component = wpa::WpaComponentKind::kReachability;
  facts::RelationId relation = facts::RelationId::kReachableCall;
  core::IdKind source_kind = core::IdKind::kFunctionVariant;
  std::optional<core::IdKind> sink_kind;
  std::string baseline_bundle;
};

const std::vector<QuerySeedSpec> &RegisteredQuerySeeds();
const QuerySeedSpec *FindQuerySeed(QueryPredicate predicate);

// --- Descriptor -------------------------------------------------------------

// A typed endpoint reference. `enclosing_function` is the function whose facts
// the query may need to examine: it equals `id` for a function reference and
// names the containing function for a value or memory reference, so a planner
// can compute a relevance set without opening a store.
struct TypedRef {
  core::IdKind kind = core::IdKind::kFunctionVariant;
  core::StableId id{};
  core::StableId enclosing_function{};

  bool operator==(const TypedRef &) const = default;
};

// Deterministic work/output limits. Every value is part of QueryId.
//
// Defaults are the lifecycle contract: 100,000 examined rows, 20,000 proof
// edges, 64 functions per SCC, and 8 admitted SCCs.
struct QueryLimits {
  std::uint64_t max_examined_rows = 100000;
  std::uint64_t max_proof_edges = 20000;
  std::uint32_t max_functions = 64;
  std::uint32_t max_sccs = 8;

  bool operator==(const QueryLimits &) const = default;
};

// Operational abort policy. Wall/memory limits are not deterministic semantic
// inputs, so they are excluded from QueryId; they appear only in the plan and
// in the artifact's cost metadata.
struct QueryRuntimeLimits {
  std::uint64_t timeout_ms = 0;
  std::uint64_t memory_mb = 0;
  std::uint32_t threads = 1;

  bool operator==(const QueryRuntimeLimits &) const = default;
};

struct QueryDescriptor {
  review::ReviewBinding binding;
  QueryPredicate predicate = QueryPredicate::kReachableCall;
  std::string predicate_version = std::string(kQueryPredicateVersion);
  // Endpoint bindings. `sources` must be non-empty; `sinks` is optional for
  // predicates that can answer an open target/memory question.
  std::vector<TypedRef> sources;
  std::vector<TypedRef> sinks;
  // Scope anchors. Typically the functions containing the endpoints; the
  // planner uses them to bound relevance and they never widen an admitted set.
  std::vector<TypedRef> subjects;
  // Selected producer/rule/model/configuration identities. `DefaultProducers`
  // builds the canonical set from a dependency snapshot's semantics.
  std::vector<review::ProviderVersion> producers;
  // Ordered boundary-summary identities admitted to this query.
  std::vector<BoundarySummaryId> boundary_summaries;
  // Digest of the selected program scope. Required and non-empty.
  std::string scope_digest;
  QueryLimits limits;
  QueryRuntimeLimits runtime;

  bool operator==(const QueryDescriptor &) const = default;
};

Status ValidateTypedRef(const TypedRef &ref);
Status ValidateQueryLimits(const QueryLimits &limits);
Status ValidateQueryDescriptor(const QueryDescriptor &descriptor);

// The canonical producer set for a semantic analysis configuration. The order
// and names are contract: every query identity includes all six.
std::vector<review::ProviderVersion>
DefaultProducers(const facts::AnalysisRunSemanticDescriptor &semantics);

// Requires the descriptor's producers to contain exactly the six canonical
// producer names with versions/digests that agree with `semantics`. A missing,
// extra, or disagreeing producer is rejected before execution.
Status ValidateProducers(const std::vector<review::ProviderVersion> &producers,
                         const facts::AnalysisRunSemanticDescriptor &semantics);

// Canonical, length-prefixed encoding of every identity-bearing field.
// `runtime` is excluded by construction. The encoding is injective: no choice
// of field contents can forge a boundary between fields.
std::string EncodeQueryDescriptor(const QueryDescriptor &descriptor);

// Validates the descriptor and hashes its canonical encoding.
StatusOr<QueryId> ComputeQueryId(const QueryDescriptor &descriptor);

// --- Boundary summaries -----------------------------------------------------

// The conservative externally visible part of a function: which values enter
// and leave, and which memory escapes. Boundary facts are stated in the
// derived-relation vocabulary of their component, so a consumer can turn them
// into successor support without reinterpreting summary.v2 bytes.
struct BoundaryFunctionInterface {
  std::vector<core::StableId> parameter_values;
  std::vector<core::StableId> returned_values;
  std::vector<core::StableId> escaped_memory;

  bool operator==(const BoundaryFunctionInterface &) const = default;
};

struct BoundarySummary {
  std::string schema_version = std::string(kBoundarySummaryVersion);
  core::StableId function_variant_id{};
  std::string configuration_digest;
  std::string layout_digest;
  BoundaryFunctionInterface interface;
  // Sorted, unique, and non-empty. A summary supports only the predicates it
  // explicitly names.
  std::vector<QueryPredicate> supported_predicates;
  // Facts in the predicate's derived relation vocabulary (for example
  // GlobalFlow facts), never in a support relation.
  std::vector<facts::AnalysisFact> facts;
  std::vector<std::string> assumptions;
  // Empty means the boundary is closed for the predicates it supports.
  std::vector<std::string> unknown_frontier;
  std::vector<std::string> dependency_digests;
  // Call sites whose unresolved/indirect dispatch this boundary explicitly
  // covers. A call site absent from this list keeps the frontier open.
  std::vector<core::StableId> covered_call_sites;
  std::string producer_version;

  bool operator==(const BoundarySummary &) const = default;
};

// Reader-side validation: schema version, identity kinds, lowercase 64-hex
// digests, sorted/unique predicates, and every fact's derived identity/row.
Status ValidateBoundarySummary(const BoundarySummary &summary);

// True when the summary is readable, names the predicate, and is closed enough
// to substitute for a successor SCC for that predicate.
bool BoundarySupports(const BoundarySummary &summary, QueryPredicate predicate);

// Canonical serialization. This is `boundary-summary.v1`'s own durable
// encoding; it is not summary.v2 and a reader refuses any other version.
StatusOr<std::string> SerializeBoundarySummary(const BoundarySummary &summary);
StatusOr<BoundarySummary> ParseBoundarySummary(std::string_view bytes);

// Identity over the canonical serialization, domain-separated by version.
StatusOr<BoundarySummaryId>
ComputeBoundarySummaryId(const BoundarySummary &summary);

// --- Dependency snapshot ----------------------------------------------------

// The immutable inputs a plan may use. Summaries provide the call/value-flow/
// memory configuration; boundary summaries substitute for unadmitted
// successors. The snapshot digest binds the two and the semantic run
// configuration so a plan cannot be executed against a different snapshot.
struct DependencySnapshot {
  core::StableId revision_id{};
  facts::AnalysisRunSemanticDescriptor semantics;
  std::vector<summary::SummaryArtifact> summaries;
  std::vector<BoundarySummary> boundary_summaries;
};

Status ValidateDependencySnapshot(const DependencySnapshot &snapshot);

// Semantic digest over the revision, run configuration, summary identities and
// their calls/value-flow/memory/unknown component digests, and the ordered
// boundary-summary identities. Evidence-only summary differences and creation
// timestamps do not move it.
std::string ComputeDependencySnapshotDigest(const DependencySnapshot &snapshot);

// --- Coverage and frontier --------------------------------------------------

enum class QueryFrontierKind : std::uint8_t {
  kOversizedScc,
  kSccBudget,
  kUnresolvedCall,
  kExternalBoundary,
  kMissingBoundary,
  kUnknownEffect,
  kSharedMemory,
  kRowBudget,
  kTimeout,
  kProofBudget,
  kMissingPremise,
};

std::string_view FrontierKindName(QueryFrontierKind kind);

struct QueryFrontierEntry {
  QueryFrontierKind kind = QueryFrontierKind::kMissingBoundary;
  std::optional<core::StableId> ref;
  std::string detail;

  bool operator==(const QueryFrontierEntry &) const = default;
};

// Deterministic order (kind, reference spelling, detail) and duplicate
// removal. Frontier order is never identity-bearing until it is canonical.
void CanonicalizeQueryFrontier(std::vector<QueryFrontierEntry> *frontier);

enum class QueryOutcome : std::uint8_t {
  kComplete,
  kIncomplete,
};

std::string_view QueryOutcomeName(QueryOutcome outcome);

struct QueryCoverage {
  QueryOutcome outcome = QueryOutcome::kIncomplete;
  // True when the selected predicate/version is registered and its producers
  // agree with the dependency snapshot.
  bool predicate_supported = false;
  // True only when traversal exhausted a supported closed scope.
  bool scope_closed = false;
  std::vector<QueryFrontierEntry> frontier;
  std::vector<std::string> assumptions;
  std::uint64_t examined_rows = 0;
  std::uint64_t proof_edges = 0;
  // True when a canonical prefix of results was returned because the proof
  // budget could not cover every selected result. No selected result ever has
  // a truncated proof.
  bool proof_truncated = false;

  bool operator==(const QueryCoverage &) const = default;
};

// Cost and setup accounting. Wall time is recorded but never identity-bearing.
struct QueryCost {
  std::uint64_t kernels_executed = 0;
  std::uint64_t worker_setup_count = 0;
  std::uint64_t materialized_rows = 0;
  std::uint64_t successor_support_rows = 0;
  std::uint64_t boundary_rows = 0;
  std::uint64_t wall_ms = 0;
  // The query path consumes already-produced summaries and never bootstraps
  // global SVF; the flag is recorded explicitly so a future provider cannot
  // hide bootstrap cost inside a query.
  bool global_svf_bootstrap = false;
  std::string global_svf_bootstrap_identity = "not-run:local-summary-input";

  bool operator==(const QueryCost &) const = default;
};

// The executed artifact. `results` are the admitted tuples matching the
// descriptor's predicate and endpoints; `premises` are the intermediate and
// root facts needed to check them; `proof` is the selected finite rooted proof
// DAG over both. An incomplete artifact is inspectable by its identity but can
// never answer a complete request.
struct QueryArtifact {
  QueryId query_id;
  QueryArtifactId artifact_id;
  std::vector<facts::AnalysisFact> results;
  std::vector<facts::AnalysisFact> premises;
  std::vector<facts::WitnessEdge> proof;
  QueryCoverage coverage;
  QueryCost cost;

  bool operator==(const QueryArtifact &) const = default;
};

bool IsCompleteAnswer(const QueryArtifact &artifact);

// Hashes result membership, premises, coverage (including completion outcome,
// frontier, assumptions and budgets), and the selected proof. Cost and timing
// are excluded.
StatusOr<QueryArtifactId>
ComputeQueryArtifactId(const QueryId &query_id, const QueryCoverage &coverage,
                       std::span<const facts::AnalysisFact> results,
                       std::span<const facts::AnalysisFact> premises,
                       std::span<const facts::WitnessEdge> proof);

} // namespace veritas::query

#endif // VERITAS_QUERY_QUERY_TYPES_H_
