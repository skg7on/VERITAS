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

// VeritasQueryEirTest.cpp — DEM-002 through DEM-006.
//
// `veritas-query evidence overflow` must publish a validated `eir.v1` case
// through the M10C semantic model, in three representations (canonical EIR-T,
// full-fidelity JSON, Protobuf) and at three evidence levels. These tests
// materialize real fixtures through the M1→M6→M9→M10A pipeline, run the PUBLIC
// CLI against the materialized store, and hold the four boundary properties
// DEM-002..DEM-006 name: level projection, cross-representation agreement,
// Protobuf output discipline, golden distinctness, and determinism.
//
// TWO COMPARISON STRENGTHS, DELIBERATELY
//
//   * BYTE-identical, for runs inside ONE build of the toolchain. Every
//     comparison that pits two artefacts this process produced against each
//     other is byte equality, because same toolchain → same `analysis_run_id`
//     → same content addresses → same bytes:
//       - DEM-002: the L0/L1/L2 texts against each other (program binding).
//       - DEM-003: `WriteEirText(ParseEirText(cli_text))` vs the CLI's own
//         text (the REP-001 write/parse/write fixpoint), `ToEvidenceJson` of
//         that parsed case vs the CLI's `--format eir-json` stdout, and the
//         canonical bytes decoded from `--format protobuf` vs the same case.
//         The `EvidenceID` equality here is the whole point of DEM-003.
//       - DEM-004: the bytes written to `--output` decode back to that case.
//       - DEM-006: all four outputs of two independently materialized stores.
//
//   * SEMANTIC, against the checked-in goldens. A golden CANNOT be compared
//     byte-for-byte. The run identity (`analysis_run_id`) derives from
//     `"souffle-" + SHA256(<vendored souffle executable bytes>)` and that
//     executable is not bit-reproducible, so a clean rebuild of the vendored
//     subtree changes the run id and everything downstream of it. Every other
//     reference is a content address over the analysis IR, and the
//     function-variant component of those addresses hashes LLVM's
//     `target-features` attribute, which the driver fills from the analysis
//     host's default CPU — so a golden regenerated on one host disagrees with
//     another host on every digest at an identical LLVM revision and an
//     identical target triple. DEM-006 demonstrates the complementary fact
//     directly: two stores materialized from the same fixture in two different
//     checkout roots are byte-identical, and two hosts are not.
//
//     The masking helper the M10B test (`VeritasQueryEvidenceTest.cpp`) uses,
//     `StabilizeDigests`, is NOT structurally reusable here, and this is worth
//     recording because the plan expected it might be. `StabilizeDigests`
//     masks runs of exactly 64 lowercase hex, which is every stable identity
//     and every digest-derived *full* case-local handle. It does not mask the
//     8-hex handles `EvidenceCaseBuilder` derives with `DigestPrefix`
//     ("E_" + kind slug + `digest_hex.substr(0, 8)`, e.g.
//     `E_value_1f4e694c`), and it cannot: an 8-hex run carries no marker that
//     distinguishes it from any other identifier, and masking all of them
//     would collapse two sibling entities onto one name. Those handles are
//     host-derived, and the L0 golden embeds them in omission *reasons* and
//     *identifiers* ("the member E_value_1f4e694c is withheld at l0"). So the
//     projection below is a different instrument: it quotients case-local
//     handle identity out entirely and keeps every value that is a function of
//     the case's meaning. See `StableSignature`.
//
// WHERE THE GOLDENS ARE, AND WHAT THEY ARE NOT
//
// `tests/golden/evidence/overflow_{unsafe.l0,unsafe.l1,unsafe.l1.eir.json,
// safe.l1,truncated.l1}.eir` are reviewed artefacts pinned against the CLI,
// and they are not the oracle for anything: the same-build comparisons above
// are strictly stronger, and the golden comparison exists to catch drift in
// the semantics a different toolchain must still reproduce.
//
// DEVIATION FROM THE DESIGN SPEC'S §14.3, RECORDED HERE BECAUSE IT IS A FINDING
//
// The spec's truncation demo names `--max-paths 1`, and the plan expected
// `overflow_truncated.l1.eir` to carry "the stable path-budget reason and no
// negative fact". Neither is reachable from this CLI, and the reasons are
// structural rather than incidental:
//
//   * The unsafe fixture's flow slice holds exactly ONE value-flow path, so
//     `--max-paths 1` cannot bind. Measured: byte-identical output to the
//     default in size and identical in fact set.
//   * The only truncation the builder records in a case is the dominating-check
//     query's, in `BuildDominatingCheck` (`OM_truncated_query_*` plus an
//     `Unknown`). It fires on `QueryCompleteness::kTruncated`, and
//     `EvidenceQueryService::RunDominatingChecks` reports `kTruncated` only
//     when more than `max_facts_per_query` facts carry
//     `coverage_kind == "dominating_check"`. No producer emits that kind today
//     — M10A emits the negative `"dominating_check_absence"` certificate — so
//     that query is complete-empty by construction and the branch is
//     unreachable from the CLI. Measured directly: `--max-facts 1`,
//     `--max-depth 1`, `--max-nodes 1`, and `--max-paths 1` all leave the case
//     with zero omissions and zero unknowns.
//   * `BuildPaths` reads only the flow slice's edges; it never consults
//     `flow_slice.metadata.completeness`. A truncated flow slice is therefore
//     not recorded in the case at all — no omission, no unknown, just a
//     missing path.
//
// So `overflow_truncated.l1.eir` is generated from the unsafe store with
// `--max-nodes 1`, the only CLI-reachable input that genuinely truncates a
// query, and the test asserts what that case actually contains: a genuine loss
// of the value-flow path, with no marker for it anywhere. The gap is asserted
// rather than papered over, and the goldens pin it so a later fix is visible.
//
// WHAT THE THREE GOLDENS ACTUALLY DIFFER BY
//
//   * `overflow_unsafe.l1.eir` and `overflow_safe.l1.eir` are both
//     `POSSIBLE_DEFECT` and both carry the derived `MUST_NOT
//     dominates_bounds_check(sink, sink)`. The safe fixture does NOT reach
//     `VERIFIED_SAFE` and does NOT carry positive dominating-check
//     counterevidence: `dominating_checks` is the same complete-empty query in
//     both (M9/M10A derives no positive check fact). What differs is the flow
//     shape — 13 entities / 19 facts / 4 edges on the unsafe fixture against
//     19 / 22 / 3 on the safe one — not the verdict.
//   * `overflow_truncated.l1.eir` is the same store as the unsafe default with
//     the flow slice truncated to no nodes: 0 edges, 0 paths, and the same
//     `MUST_NOT` fact as the default. It is the "partial evidence is not
//     marked" case above.
//
// The fixtures compile with `-fdebug-prefix-map=@PROJECT_ROOT@=.`, so the
// materialization root cannot leak into debug info and the two DEM-006 stores
// stay byte-identical even at different path lengths (asserted).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include "evidence/RealEvidencePipeline.h"
#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/evidence/EirText.h"
#include "veritas/evidence/EvidenceCanonicalizer.h"
#include "veritas/evidence/EvidenceCase.h"
#include "veritas/evidence/EvidenceJson.h"
#include "veritas/evidence/EvidenceProto.h"
#include "veritas/evidence/EvidenceValidator.h"

#ifndef VERITAS_QUERY_BINARY
#error "VERITAS_QUERY_BINARY must be defined by the build system"
#endif
#ifndef VERITAS_GOLDEN_DIR
#error "VERITAS_GOLDEN_DIR must be defined by the build system"
#endif

namespace veritas::testing {
namespace {

namespace ev = evidence;

// ---------------------------------------------------------------------------
// The public CLI
// ---------------------------------------------------------------------------

std::string ShellQuote(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('\'');
  for (const char c : value) {
    if (c == '\'') {
      out.append("'\\''");
    } else {
      out.push_back(c);
    }
  }
  out.push_back('\'');
  return out;
}

struct CliResult {
  int exit_code = -1;
  std::string stdout_text;
};

// `veritas-query`'s stderr is merged into `stdout_text`: a failure message is
// the only evidence a rejected invocation leaves, and the tests that assert on
// rejection need it. Each call therefore returns one text stream and one exit
// code, read from the process rather than from a pipe's status.
CliResult RunVeritasQuery(const std::vector<std::string>& arguments) {
  std::string command = ShellQuote(VERITAS_QUERY_BINARY);
  for (const auto& argument : arguments) {
    command.push_back(' ');
    command.append(ShellQuote(argument));
  }
  command.append(" 2>&1");

  CliResult result;
  FILE* pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr) {
    return result;
  }
  std::array<char, 4096> buffer{};
  while (::fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
    result.stdout_text.append(buffer.data());
  }
  const int status = ::pclose(pipe);
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return result;
}

std::vector<std::string> EvidenceArguments(const std::filesystem::path& db_dir,
                                           std::string_view format,
                                           std::string_view level) {
  return {"evidence",         "overflow",     "--sink", "memcpy",
          "--db",             db_dir.string(), "--format", std::string(format),
          "--level",          std::string(level)};
}

CliResult RunEvidence(const std::filesystem::path& db_dir,
                      std::string_view format, std::string_view level) {
  return RunVeritasQuery(EvidenceArguments(db_dir, format, level));
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::ostringstream contents;
  contents << file.rdbuf();
  return contents.str();
}

std::filesystem::path GoldenPath(std::string_view name) {
  return std::filesystem::path(VERITAS_GOLDEN_DIR) / name;
}

// ---------------------------------------------------------------------------
// Materialized stores
// ---------------------------------------------------------------------------

// Runs the real pipeline for `fixture` once per process and hands back the
// store root. `FixtureProject` allocates a unique temporary directory that
// outlives the call, so the path stays valid for every later test.
StatusOr<std::filesystem::path> MaterializedStore(std::string_view fixture) {
  static std::map<std::string, std::filesystem::path> cache;
  const std::string key(fixture);
  const auto cached = cache.find(key);
  if (cached != cache.end()) {
    return cached->second;
  }
  auto snapshot = AnalyzeRealFixture(fixture);
  if (!snapshot.ok()) {
    return snapshot.status();
  }
  auto inserted = cache.emplace(key, snapshot->output_root);
  return inserted.first->second;
}

// ---------------------------------------------------------------------------
// Parsing the CLI's output
// ---------------------------------------------------------------------------

StatusOr<ev::EvidenceCase> ParseOrFail(std::string_view text,
                                       std::string_view what) {
  ev::EirParseError error;
  auto parsed = ev::ParseEirText(text, &error);
  if (!parsed.ok()) {
    return Status::InvalidArgument(std::string(what) + " does not parse: " +
                                   error.message + " (line " +
                                   std::to_string(error.line) + ", column " +
                                   std::to_string(error.column) + ")");
  }
  return parsed;
}

// The one member of the full-fidelity JSON document the DEM-003 comparison
// needs. The document is the writer's `llvm::json::OStream` output — one member
// per line, keys sorted, exactly one trailing newline — and `evidence_id` is
// the only key with that spelling, so the scan is unambiguous rather than a
// second JSON parser. Everything else is compared by re-serializing the parsed
// case, which is a byte comparison against the real writer.
StatusOr<std::string> JsonEvidenceId(std::string_view document) {
  constexpr std::string_view kKey = "\"evidence_id\": \"";
  const std::size_t start = document.find(kKey);
  if (start == std::string_view::npos) {
    return Status::InvalidArgument("the JSON document carries no evidence_id");
  }
  const std::size_t value_start = start + kKey.size();
  const std::size_t end = document.find('"', value_start);
  if (end == std::string_view::npos) {
    return Status::InvalidArgument("the JSON evidence_id is unterminated");
  }
  return std::string(document.substr(value_start, end - value_start));
}

// ---------------------------------------------------------------------------
// The toolchain-stable projection of one case
// ---------------------------------------------------------------------------
//
// A golden embeds identities that belong to the host that produced it (see the
// file header), so comparing two cases field by field is not a comparison of
// their semantics. What IS a function of the case's meaning is its shape as a
// labelled, ordered-reference graph — provided every content address is
// replaced by the KIND of identity it carries and every case-local handle is
// replaced by a position in that graph.
//
// `StableSignature` computes a canonical invariant of that graph:
//
//   1. Every member of the case becomes one `Item`: a category, a label
//      holding every non-reference field, and the ordered list of case-local
//      handles it points at. Stable identities enter the label as their kind
//      (`valref`, `memref`, `fact`, `model`, `layout`), never as a digest.
//   2. Every label is normalized: any occurrence of a case-local handle is
//      replaced by `@H` — which is what makes L0 omission reasons comparable,
//      since they name the members they withhold — and any residual run of
//      exactly 64 lowercase hex is replaced by `<digest>`, which catches a
//      stable identity reached through a `std::string` field the model does
//      not type (`Constraint::scope`, `Assumption::scope`).
//   3. Colours are refined over the reference graph for one round per item.
//      Round `k`'s colour of an item is a hash of its label together with the
//      round `k-1` colours of its references, in reference order and with
//      position included, so the invariant distinguishes a chain that visits
//      A before B from one that visits B before A.
//   4. The signature is the SORTED multiset of `category::label::colour`. Same
//      meaning → same signature. A difference in kind, count, wiring, order,
//      or referenced-member shape → a difference in signature.
//
// The invariant is not complete: it is a hash-based refinement, so two cases
// that are not isomorphic could in principle collide, and it quotients away
// *which* sibling of a colour class a reference names. Neither gap is load
// bearing here, because every same-build comparison in this file — including
// the full byte equality of DEM-003 and DEM-006 — is exact and strictly
// stronger. This projection is the cross-toolchain instrument, and it is
// deliberately the strong one for that job.

std::uint64_t Fnv1a(std::string_view text) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const char c : text) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::string ToHex(std::uint64_t value) {
  static constexpr std::string_view kDigits = "0123456789abcdef";
  std::string out(16, '0');
  for (std::size_t i = 0; i < 16; ++i) {
    out[15 - i] = kDigits[static_cast<std::size_t>((value >> (4 * i)) & 0xf)];
  }
  return out;
}

std::string Bool(bool value) { return value ? "1" : "0"; }

// The kind of a stable identity, e.g. "valref" for "valref:sha256:<digest>".
std::string IdentityKind(const core::StableId& id) {
  const std::string text = core::ToString(id);
  const std::size_t colon = text.find(':');
  return colon == std::string::npos ? text : text.substr(0, colon);
}

std::string IdentityKind(const std::optional<core::StableId>& id) {
  return id.has_value() ? IdentityKind(*id) : std::string("none");
}

// The canonical text of an optional identity, or the empty string when the
// member is absent. `EvidenceCase::evidence_id` is optional, and an unset one
// renders as empty rather than as the text of a default-constructed id.
std::string IdText(const std::optional<core::StableId>& id) {
  return id.has_value() ? core::ToString(*id) : std::string();
}

std::string ExpressionKindName(ev::Expression::Kind kind) {
  switch (kind) {
    case ev::Expression::Kind::kUnspecified:
      return "unspecified";
    case ev::Expression::Kind::kBool:
      return "bool";
    case ev::Expression::Kind::kInteger:
      return "integer";
    case ev::Expression::Kind::kString:
      return "string";
    case ev::Expression::Kind::kSymbol:
      return "symbol";
    case ev::Expression::Kind::kReference:
      return "reference";
    case ev::Expression::Kind::kCall:
      return "call";
    case ev::Expression::Kind::kNot:
      return "not";
    case ev::Expression::Kind::kCompare:
      return "compare";
    case ev::Expression::Kind::kAnd:
      return "and";
    case ev::Expression::Kind::kOr:
      return "or";
    case ev::Expression::Kind::kImplies:
      return "implies";
    case ev::Expression::Kind::kForAll:
      return "forall";
    case ev::Expression::Kind::kExists:
      return "exists";
  }
  return "unknown";
}

// Renders an expression structurally and appends every case-local handle it
// names, in operand order. A reference contributes its handle to `refs` and
// nothing to the text, so the handle never reaches a label.
std::string RenderExpression(const ev::Expression& expression,
                             std::vector<std::string>* refs) {
  const bool is_reference =
      expression.kind == ev::Expression::Kind::kReference;
  std::string out = ExpressionKindName(expression.kind);
  out.push_back('(');
  if (!is_reference) {
    out.append(expression.text);
  }
  out.push_back(',');
  out.append(std::to_string(expression.integer));
  out.push_back(',');
  out.append(Bool(expression.boolean));
  for (const ev::Expression& operand : expression.operands) {
    out.push_back(';');
    out.append(RenderExpression(operand, refs));
  }
  out.push_back(')');
  if (is_reference) {
    refs->push_back(expression.text);
  }
  return out;
}

void AddReference(std::vector<std::string>* refs, const std::string& handle) {
  if (!handle.empty()) {
    refs->push_back(handle);
  }
}

std::string RenderBinding(const ev::ProgramBinding& program) {
  // `target_triple` is deliberately absent: it names the host that ran the
  // analysis, not the case. The four identity strings are kept, because after
  // masking they still say which binding was pinned; `analysis_run_id` is
  // reduced to its kind for the same reason.
  return "repository=" + program.repository_id + ";revision=" +
         program.revision_id + ";build_variant=" + program.build_variant_id +
         ";configuration=" + program.analysis_configuration_id +
         ";type_layout=" + program.type_layout_id + ";analysis_run=" +
         IdentityKind(program.analysis_run_id) + ";analyzers=" +
         std::to_string(program.analyzer_versions.size());
}

struct Item {
  std::string category;
  std::string label;
  std::vector<std::string> refs;
};

struct CaseGraph {
  std::vector<Item> items;
  std::map<std::string, std::size_t> by_handle;
};

// Every case-local handle the case declares, longest first so that substituting
// a handle cannot partially overwrite a longer one.
std::vector<std::string> LocalHandles(const ev::EvidenceCase& value) {
  std::vector<std::string> handles;
  handles.push_back(value.primary_claim.id);
  for (const ev::Entity& entity : value.entities) handles.push_back(entity.id);
  for (const ev::Edge& edge : value.edges) handles.push_back(edge.id);
  for (const ev::Path& path : value.paths) handles.push_back(path.id);
  for (const ev::Fact& fact : value.facts) handles.push_back(fact.id);
  for (const ev::Assumption& assumption : value.assumptions) {
    handles.push_back(assumption.id);
  }
  for (const ev::Hypothesis& hypothesis : value.hypotheses) {
    handles.push_back(hypothesis.id);
  }
  for (const ev::Unknown& unknown : value.unknowns) {
    handles.push_back(unknown.id);
  }
  for (const ev::Constraint& constraint : value.constraints) {
    handles.push_back(constraint.id);
  }
  for (const ev::Provenance& record : value.provenance) {
    handles.push_back(record.id);
  }
  for (const ev::ProofObligation& obligation : value.proof_obligations) {
    handles.push_back(obligation.id);
  }
  for (const ev::SummaryReference& summary : value.summaries) {
    handles.push_back(summary.id);
  }
  for (const ev::Dependency& dependency : value.dependencies) {
    handles.push_back(dependency.id);
  }
  for (const ev::Omission& omission : value.omissions) {
    handles.push_back(omission.id);
  }
  handles.erase(std::remove(handles.begin(), handles.end(), std::string()),
                handles.end());
  std::sort(handles.begin(), handles.end(),
            [](const std::string& left, const std::string& right) {
              return left.size() != right.size() ? left.size() > right.size()
                                                 : left < right;
            });
  handles.erase(std::unique(handles.begin(), handles.end()), handles.end());
  return handles;
}

bool IsLowerHex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

// Replaces every run of exactly 64 lowercase hex with `<digest>`. A shorter or
// longer run is left alone, which is the same rule the M10B test applies.
std::string MaskDigests(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size();) {
    if (IsLowerHex(text[i])) {
      std::size_t end = i;
      while (end < text.size() && IsLowerHex(text[end])) {
        ++end;
      }
      if (end - i == 64) {
        out.append("<digest>");
      } else {
        out.append(text.substr(i, end - i));
      }
      i = end;
      continue;
    }
    out.push_back(text[i]);
    ++i;
  }
  return out;
}

std::string NormalizeLabel(std::string label,
                           const std::vector<std::string>& handles) {
  for (const std::string& handle : handles) {
    std::string::size_type position = 0;
    while ((position = label.find(handle, position)) != std::string::npos) {
      label.replace(position, handle.size(), "@H");
      position += 2;
    }
  }
  return MaskDigests(label);
}

CaseGraph BuildGraph(const ev::EvidenceCase& value) {
  const std::vector<std::string> handles = LocalHandles(value);
  CaseGraph graph;

  const auto add = [&graph, &handles](std::string category,
                                      const std::string& handle,
                                      std::string label,
                                      std::vector<std::string> refs) {
    if (!handle.empty()) {
      graph.by_handle.emplace(handle, graph.items.size());
    }
    graph.items.push_back(Item{std::move(category),
                               NormalizeLabel(std::move(label), handles),
                               std::move(refs)});
  };

  add("case", std::string(),
      "level=" + std::string(ev::ToString(value.level)) +
          ";state=" + std::string(ev::ToString(value.verification_state)) +
          ";schema=" + value.schema_version + ";" +
          RenderBinding(value.program),
      {});

  for (const ev::Entity& entity : value.entities) {
    std::string label =
        "kind=" + std::string(ev::ToString(entity.kind)) +
        ";stable=" + IdentityKind(entity.stable_id);
    std::vector<std::string> refs;
    for (const auto& entry : entity.properties) {
      label += ";property[" + entry.first +
               "]=" + RenderExpression(entry.second, &refs);
    }
    add("entity", entity.id, std::move(label), std::move(refs));
  }

  {
    const ev::Claim& claim = value.primary_claim;
    std::vector<std::string> refs;
    const std::string predicate = RenderExpression(claim.predicate, &refs);
    AddReference(&refs, claim.subject);
    add("claim", claim.id,
        "kind=" + std::string(ev::ToString(claim.kind)) +
            ";severity=" + std::string(ev::ToString(claim.severity)) +
            ";description=" + claim.description + ";predicate=" + predicate,
        std::move(refs));
  }

  for (const ev::Fact& fact : value.facts) {
    std::vector<std::string> refs;
    const std::string predicate = RenderExpression(fact.predicate, &refs);
    AddReference(&refs, fact.provenance_id);
    add("fact", fact.id,
        "stable=" + IdentityKind(fact.stable_id) +
            ";epistemic=" + std::string(ev::ToString(fact.epistemic)) +
            ";confidence=" + std::string(ev::ToString(fact.confidence)) +
            ";producer=" + fact.producer + ";derived=" + Bool(fact.derived) +
            ";predicate=" + predicate,
        std::move(refs));
  }

  for (const ev::Assumption& assumption : value.assumptions) {
    std::vector<std::string> refs;
    const std::string predicate = RenderExpression(assumption.predicate, &refs);
    add("assumption", assumption.id,
        "source=" + assumption.source + ";scope=" + assumption.scope +
            ";predicate=" + predicate,
        std::move(refs));
  }

  for (const ev::Hypothesis& hypothesis : value.hypotheses) {
    std::vector<std::string> refs;
    const std::string predicate = RenderExpression(hypothesis.predicate, &refs);
    add("hypothesis", hypothesis.id,
        "producer=" + hypothesis.producer + ";reason=" + hypothesis.reason +
            ";confidence=" +
            std::string(ev::ToString(hypothesis.confidence)) +
            ";predicate=" + predicate,
        std::move(refs));
  }

  for (const ev::Unknown& unknown : value.unknowns) {
    std::vector<std::string> refs;
    const std::string property = RenderExpression(unknown.property, &refs);
    for (const std::string& blocking : unknown.blocking_ids) {
      AddReference(&refs, blocking);
    }
    add("unknown", unknown.id,
        "reason_code=" + std::string(ev::ToString(unknown.reason_code)) +
            ";reason=" + unknown.reason +
            ";resolution=" + unknown.suggested_resolution +
            ";property=" + property,
        std::move(refs));
  }

  for (const ev::Edge& edge : value.edges) {
    std::vector<std::string> refs{edge.from, edge.to};
    AddReference(&refs, edge.provenance_id);
    AddReference(&refs, edge.summarized_by);
    add("edge", edge.id,
        "kind=" + std::string(ev::ToString(edge.kind)) +
            ";epistemic=" + std::string(ev::ToString(edge.epistemic)) +
            ";expandable=" + Bool(edge.expandable),
        std::move(refs));
  }

  for (const ev::Path& path : value.paths) {
    std::vector<std::string> refs(path.entity_ids.begin(),
                                  path.entity_ids.end());
    std::string label = "kind=" + std::string(ev::ToString(path.kind)) +
                        ";feasibility=" +
                        std::string(ev::ToString(path.feasibility));
    for (const ev::Expression& condition : path.conditions) {
      label += ";condition=" + RenderExpression(condition, &refs);
    }
    AddReference(&refs, path.provenance_id);
    add("path", path.id, std::move(label), std::move(refs));
  }

  for (const ev::Constraint& constraint : value.constraints) {
    std::vector<std::string> refs;
    const std::string expression =
        RenderExpression(constraint.expression, &refs);
    AddReference(&refs, constraint.provenance_id);
    add("constraint", constraint.id,
        "scope=" + constraint.scope +
            ";epistemic=" + std::string(ev::ToString(constraint.epistemic)) +
            ";expression=" + expression,
        std::move(refs));
  }

  for (const ev::Provenance& record : value.provenance) {
    std::vector<std::string> refs(record.input_fact_ids.begin(),
                                  record.input_fact_ids.end());
    AddReference(&refs, record.source_anchor_id);
    add("provenance", record.id,
        "producer=" + record.producer + ";rule=" + record.rule +
            ";version=" + record.version +
            ";configuration=" + record.configuration +
            ";analysis_run=" + IdentityKind(record.analysis_run_id),
        std::move(refs));
  }

  for (const ev::ProofObligation& obligation : value.proof_obligations) {
    std::vector<std::string> refs;
    const std::string predicate = RenderExpression(obligation.predicate, &refs);
    const std::string budget = RenderExpression(obligation.budget, &refs);
    AddReference(&refs, obligation.result_id);
    std::string label =
        "goal=" + std::string(ev::ToString(obligation.goal_kind)) +
        ";status=" + std::string(ev::ToString(obligation.status)) +
        ";producer=" + obligation.verification_producer + ";verifiers=";
    for (const std::string& verifier : obligation.verifier_kinds) {
      label += verifier + ",";
    }
    label += ";predicate=" + predicate + ";budget=" + budget;
    add("obligation", obligation.id, std::move(label), std::move(refs));
  }

  for (const ev::SummaryReference& summary : value.summaries) {
    std::vector<std::string> refs;
    AddReference(&refs, summary.function_id);
    for (const std::string& component : summary.components) {
      AddReference(&refs, component);
    }
    add("summary", summary.id,
        "summary_id=" + IdentityKind(summary.summary_id),
        std::move(refs));
  }

  for (const ev::Dependency& dependency : value.dependencies) {
    add("dependency", dependency.id,
        "kind=" + std::string(ev::ToString(dependency.kind)) +
            ";stable=" + IdentityKind(dependency.stable_id),
        {});
  }

  for (const ev::Omission& omission : value.omissions) {
    std::vector<std::string> refs;
    AddReference(&refs, omission.subject);
    add("omission", omission.id,
        "kind=" + omission.kind + ";reason=" + omission.reason +
            ";expandable=" + Bool(omission.expandable),
        std::move(refs));
  }

  return graph;
}

// True when every reference in the graph resolved to a declared member. An
// unresolved one would leak a host-derived handle into the comparison, so the
// caller asserts this rather than trusting it.
bool ReferencesResolve(const CaseGraph& graph) {
  for (const Item& item : graph.items) {
    for (const std::string& reference : item.refs) {
      if (graph.by_handle.find(reference) == graph.by_handle.end()) {
        return false;
      }
    }
  }
  return true;
}

std::vector<std::string> StableSignature(const ev::EvidenceCase& value) {
  const CaseGraph graph = BuildGraph(value);
  const std::size_t count = graph.items.size();

  std::vector<std::uint64_t> colour(count);
  for (std::size_t i = 0; i < count; ++i) {
    colour[i] = Fnv1a(graph.items[i].label);
  }
  // One round per item is one more than any graph of this size needs to make
  // the invariant distinguishable, so the loop is not looking for a fixpoint
  // (the hash never repeats) — it is bounding the unfolding depth.
  std::vector<std::uint64_t> next(count);
  for (std::size_t round = 0; round <= count; ++round) {
    for (std::size_t i = 0; i < count; ++i) {
      const Item& item = graph.items[i];
      std::string material = item.category;
      material.push_back('|');
      material.append(ToHex(colour[i]));
      for (std::size_t position = 0; position < item.refs.size(); ++position) {
        material.push_back('|');
        material.append(std::to_string(position));
        material.push_back(':');
        const auto target = graph.by_handle.find(item.refs[position]);
        material.append(target == graph.by_handle.end()
                            ? std::string("?")
                            : ToHex(colour[target->second]));
      }
      next[i] = Fnv1a(material);
    }
    colour.swap(next);
  }

  std::vector<std::string> signature;
  signature.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    signature.push_back(graph.items[i].category + "::" + graph.items[i].label +
                        "::" + ToHex(colour[i]));
  }
  std::sort(signature.begin(), signature.end());
  return signature;
}

std::string SignatureText(const std::vector<std::string>& signature) {
  std::string out;
  for (const std::string& line : signature) {
    out.append(line);
    out.push_back('\n');
  }
  return out;
}

// ---------------------------------------------------------------------------
// Shared fixture helpers
// ---------------------------------------------------------------------------

// Every case-local handle the case declares, as a set, for the DEM-002
// projection check. Same-build comparison, so the handles may be compared
// directly.
std::set<std::string> HandleSet(const ev::EvidenceCase& value) {
  std::set<std::string> handles;
  handles.insert(value.primary_claim.id);
  for (const ev::Entity& entity : value.entities) handles.insert(entity.id);
  for (const ev::Edge& edge : value.edges) handles.insert(edge.id);
  for (const ev::Path& path : value.paths) handles.insert(path.id);
  for (const ev::Fact& fact : value.facts) handles.insert(fact.id);
  for (const ev::Assumption& a : value.assumptions) handles.insert(a.id);
  for (const ev::Hypothesis& h : value.hypotheses) handles.insert(h.id);
  for (const ev::Unknown& u : value.unknowns) handles.insert(u.id);
  for (const ev::Constraint& c : value.constraints) handles.insert(c.id);
  for (const ev::Provenance& p : value.provenance) handles.insert(p.id);
  for (const ev::ProofObligation& o : value.proof_obligations) {
    handles.insert(o.id);
  }
  for (const ev::SummaryReference& s : value.summaries) handles.insert(s.id);
  for (const ev::Dependency& d : value.dependencies) handles.insert(d.id);
  for (const ev::Omission& o : value.omissions) handles.insert(o.id);
  handles.erase(std::string());
  return handles;
}

std::set<std::string> Difference(const std::set<std::string>& left,
                                 const std::set<std::string>& right) {
  std::set<std::string> out;
  for (const std::string& value : left) {
    if (right.count(value) == 0) {
      out.insert(value);
    }
  }
  return out;
}

std::string JoinSet(const std::set<std::string>& values) {
  std::string out;
  for (const std::string& value : values) {
    if (!out.empty()) {
      out.append(", ");
    }
    out.append(value);
  }
  return out.empty() ? std::string("(none)") : out;
}

constexpr std::string_view kWithheldPrefix = "the member ";
constexpr std::string_view kWithheldSuffix =
    " is withheld at l0; a higher level can recover it";

// The member an L0 level-projection omission names.
StatusOr<std::string> WithheldMember(const ev::Omission& omission) {
  const std::string& reason = omission.reason;
  if (reason.compare(0, kWithheldPrefix.size(), kWithheldPrefix) != 0 ||
      reason.size() < kWithheldPrefix.size() + kWithheldSuffix.size() ||
      reason.compare(reason.size() - kWithheldSuffix.size(),
                     kWithheldSuffix.size(), kWithheldSuffix) != 0) {
    return Status::InvalidArgument("unexpected level-projection reason: " +
                                   reason);
  }
  return reason.substr(kWithheldPrefix.size(),
                       reason.size() - kWithheldPrefix.size() -
                           kWithheldSuffix.size());
}

}  // namespace

// ---------------------------------------------------------------------------
// DEM-002 — EmitsValidatedL0L1L2
// ---------------------------------------------------------------------------
//
// Failure mode guarded: "Higher-detail members leak into L0 without omission."
// The check below is the exact inverse of that: every member L1 declares and L0
// does not is named by an L0 omission. It is a SAME-BUILD comparison and
// compares case-local handles directly, which is only meaningful because both
// documents come from one store and one toolchain.
TEST(VeritasQueryEirTest, DEM002EmitsValidatedL0L1L2) {
  auto store = MaterializedStore("evidence_overflow_unsafe");
  ASSERT_TRUE(store.ok()) << store.status().message();

  std::map<ev::EvidenceLevel, ev::EvidenceCase> cases;
  for (const auto& [level, text] :
       std::vector<std::pair<ev::EvidenceLevel, std::string>>{
           {ev::EvidenceLevel::kL0, "l0"},
           {ev::EvidenceLevel::kL1, "l1"},
           {ev::EvidenceLevel::kL2, "l2"}}) {
    const CliResult cli = RunEvidence(*store, "eir-t", text);
    ASSERT_EQ(cli.exit_code, 0) << text << ": " << cli.stdout_text;
    ASSERT_FALSE(cli.stdout_text.empty()) << text;

    // ParseEirText is the reader, and it validates and finalizes: a document
    // that parses is a document that passed `RequireValidEvidenceCase` and
    // carries its computed content address.
    auto parsed = ParseOrFail(cli.stdout_text, text);
    ASSERT_TRUE(parsed.ok()) << parsed.status().message();
    EXPECT_EQ(parsed->level, level) << text;
    EXPECT_TRUE(parsed->evidence_id.has_value()) << text;
    EXPECT_TRUE(ev::RequireValidEvidenceCase(*parsed).ok()) << text;
    // The write/parse/write fixpoint (`REP-001`) holds at every level, so the
    // CLI's own text is the canonical layout.
    auto rewritten =
        ev::WriteEirText(*parsed, ev::EirTextStyle::kCanonical);
    ASSERT_TRUE(rewritten.ok()) << rewritten.status().message();
    EXPECT_EQ(*rewritten, cli.stdout_text) << text;

    cases.emplace(level, std::move(*parsed));
  }

  const ev::EvidenceCase& l0 = cases.at(ev::EvidenceLevel::kL0);
  const ev::EvidenceCase& l1 = cases.at(ev::EvidenceLevel::kL1);
  const ev::EvidenceCase& l2 = cases.at(ev::EvidenceLevel::kL2);

  // The program binding is one binding: the same repository, revision, build
  // variant, target, configuration, type layout, and run at every level. Only
  // `target_triple` is excluded from the golden projection, never from this
  // one — it is the same string here by construction.
  for (const ev::EvidenceCase* other : {&l1, &l2}) {
    EXPECT_EQ(l0.program.repository_id, other->program.repository_id);
    EXPECT_EQ(l0.program.revision_id, other->program.revision_id);
    EXPECT_EQ(l0.program.build_variant_id, other->program.build_variant_id);
    EXPECT_EQ(l0.program.target_triple, other->program.target_triple);
    EXPECT_EQ(l0.program.analysis_configuration_id,
              other->program.analysis_configuration_id);
    EXPECT_EQ(l0.program.type_layout_id, other->program.type_layout_id);
    EXPECT_EQ(l0.program.analysis_run_id, other->program.analysis_run_id);
  }

  // L0 withholds members and says so. Every omission it carries is a
  // level-projection marker, and it names the member it withheld.
  EXPECT_GT(l0.omissions.size(), 0u);
  std::set<std::string> named;
  for (const ev::Omission& omission : l0.omissions) {
    EXPECT_EQ(omission.kind, "level_projection");
    EXPECT_TRUE(omission.expandable);
    auto member = WithheldMember(omission);
    ASSERT_TRUE(member.ok()) << member.status().message();
    named.insert(*member);
  }

  // No silent omission. DEM-002's stated failure mode is exactly the negation
  // of this: the L0 omissions and the members L0 actually dropped are the same
  // set. Stated as equality rather than as containment, because containment in
  // one direction would also hold for an omission that named a member L0 kept.
  const std::set<std::string> l0_handles = HandleSet(l0);
  const std::set<std::string> l1_handles = HandleSet(l1);
  std::set<std::string> dropped;
  for (const std::string& handle : l1_handles) {
    if (l0_handles.count(handle) == 0) {
      dropped.insert(handle);
    }
  }
  EXPECT_EQ(named, dropped)
      << "the l0 omissions and the members l0 dropped disagree\n"
      << "  named but not dropped: " << JoinSet(Difference(named, dropped))
      << "\n  dropped but not named: " << JoinSet(Difference(dropped, named));

  // L0 is strictly smaller on the collections it withholds from, and it keeps
  // the one member the claim's own question needs. Entities are deliberately
  // NOT asserted to shrink: the L0 projection keeps every entity a retained
  // fact names, and the retained facts name all of them, so this fixture's
  // projection removes members without removing any node they hang off.
  EXPECT_LE(l0.entities.size(), l1.entities.size());
  EXPECT_LT(l0.facts.size(), l1.facts.size());
  EXPECT_LT(l0.provenance.size(), l1.provenance.size());
  EXPECT_EQ(l0.constraints.size(), 0u);
  EXPECT_EQ(l1.constraints.size(), 1u);
  EXPECT_EQ(l0.paths.size(), 1u);
  EXPECT_EQ(l1.paths.size(), 1u);
  EXPECT_EQ(l0.primary_claim.id, l1.primary_claim.id);
  EXPECT_EQ(l0.primary_claim.kind, l1.primary_claim.kind);
  EXPECT_EQ(l0.primary_claim.severity, l1.primary_claim.severity);

  // L1 withholds nothing: no level-projection marker survives at the causal
  // level, so the projection is one-way and cannot be mistaken for a case that
  // simply lost members.
  EXPECT_EQ(l1.omissions.size(), 0u);

  // L2 does not drop anything L1 kept, and adds no marker L1 lacks. On this
  // fixture L2 is L1 with a different `level` token and nothing else: the only
  // L2-only expansion is `BuildSummaryExpansions`, which returns early unless
  // the case has Function entities, and this fixture has none.
  EXPECT_GE(l2.entities.size(), l1.entities.size());
  EXPECT_GE(l2.facts.size(), l1.facts.size());
  EXPECT_GE(l2.provenance.size(), l1.provenance.size());
  EXPECT_EQ(l2.omissions.size(), l1.omissions.size());
  EXPECT_EQ(l2.primary_claim.id, l1.primary_claim.id);
  EXPECT_EQ(l2.verification_state, l1.verification_state);

  // The three texts are pairwise distinct documents and three distinct
  // identities: the level is inside the case's meaning, so it is inside the
  // address.
  EXPECT_NE(*ev::WriteEirText(l0, ev::EirTextStyle::kCanonical),
            *ev::WriteEirText(l1, ev::EirTextStyle::kCanonical));
  EXPECT_NE(*ev::WriteEirText(l1, ev::EirTextStyle::kCanonical),
            *ev::WriteEirText(l2, ev::EirTextStyle::kCanonical));
  EXPECT_NE(l0.evidence_id, l1.evidence_id);
  EXPECT_NE(l1.evidence_id, l2.evidence_id);
}

// ---------------------------------------------------------------------------
// DEM-003 — TextJsonAndProtoAgree
// ---------------------------------------------------------------------------
//
// Failure mode guarded: "Golden text alone considered sufficient." All three
// M10C representations are read and compared, and every comparison here is
// SAME-BUILD byte equality or identity equality — which is exactly where
// byte-equality belongs, and why the `EvidenceID` assertion is legitimate.
// The JSON document has no reader in M10C, so it is not decoded back into a
// case; it is checked by the one member that can be read unambiguously
// (`evidence_id`) and by re-serializing the case the TEXT parsed into, byte for
// byte.
TEST(VeritasQueryEirTest, DEM003TextJsonAndProtoAgree) {
  auto store = MaterializedStore("evidence_overflow_unsafe");
  ASSERT_TRUE(store.ok()) << store.status().message();

  const CliResult text = RunEvidence(*store, "eir-t", "l1");
  ASSERT_EQ(text.exit_code, 0) << text.stdout_text;
  const CliResult json = RunEvidence(*store, "eir-json", "l1");
  ASSERT_EQ(json.exit_code, 0) << json.stdout_text;

  const std::filesystem::path destination =
      std::filesystem::path(::testing::TempDir()) / "dem003.eir.pb";
  std::error_code ignored;
  std::filesystem::remove(destination, ignored);
  std::vector<std::string> proto_arguments =
      EvidenceArguments(*store, "protobuf", "l1");
  proto_arguments.push_back("--output");
  proto_arguments.push_back(destination.string());
  const CliResult proto = RunVeritasQuery(proto_arguments);
  ASSERT_EQ(proto.exit_code, 0) << proto.stdout_text;
  // The binary representation never reaches stdout.
  EXPECT_TRUE(proto.stdout_text.empty())
      << "protobuf wrote to stdout: " << proto.stdout_text;
  ASSERT_TRUE(std::filesystem::exists(destination));

  // The text is the reference: it is the only representation with a reader.
  auto parsed = ParseOrFail(text.stdout_text, "the eir-t output");
  ASSERT_TRUE(parsed.ok()) << parsed.status().message();
  const ev::EvidenceCase& from_text = *parsed;
  ASSERT_TRUE(from_text.evidence_id.has_value());

  // 1. The JSON document is exactly the JSON of the case the text parses into.
  //    Byte equality, same build.
  auto expected_json = ev::ToEvidenceJson(from_text);
  ASSERT_TRUE(expected_json.ok()) << expected_json.status().message();
  EXPECT_EQ(json.stdout_text, *expected_json)
      << "eir-json is not the JSON of the case eir-t parses into";

  // 2. The JSON carries the same identity.
  auto json_id = JsonEvidenceId(json.stdout_text);
  ASSERT_TRUE(json_id.ok()) << json_id.status().message();
  EXPECT_EQ(*json_id, IdText(from_text.evidence_id));

  // 3. The Protobuf bytes decode to a case with the same canonical bytes and
  //    the same identity. Three representations, one case.
  const std::string bytes = ReadFile(destination);
  EXPECT_FALSE(bytes.empty());
  auto decoded = ev::DecodeEvidenceProto(bytes);
  ASSERT_TRUE(decoded.ok()) << decoded.status().message();
  auto from_text_bytes = ev::CanonicalEvidenceBytes(from_text);
  ASSERT_TRUE(from_text_bytes.ok()) << from_text_bytes.status().message();
  auto from_proto_bytes = ev::CanonicalEvidenceBytes(*decoded);
  ASSERT_TRUE(from_proto_bytes.ok()) << from_proto_bytes.status().message();
  EXPECT_EQ(*from_text_bytes, *from_proto_bytes)
      << "protobuf and eir-t disagree on the case's canonical bytes";
  ASSERT_TRUE(decoded->evidence_id.has_value());
  EXPECT_EQ(core::ToString(*decoded->evidence_id),
            IdText(from_text.evidence_id))
      << "protobuf and eir-t disagree on the EvidenceID";
  EXPECT_EQ(*json_id, core::ToString(*decoded->evidence_id));

  // 4. The identity really is the content address of the case, not a stored
  //    field that happened to agree: recomputing from the text's case must
  //    reproduce it.
  auto recomputed = ev::ComputeEvidenceId(from_text);
  ASSERT_TRUE(recomputed.ok()) << recomputed.status().message();
  EXPECT_EQ(core::ToString(*recomputed),
            IdText(from_text.evidence_id));

  // 5. Non-vacuity: the comparison inspects a real document. A case mutated in
  //    one member must not serialize to the same canonical bytes, or step 3
  //    would pass on any two documents at all.
  ev::EvidenceCase mutated = from_text;
  ASSERT_FALSE(mutated.entities.empty());
  mutated.entities.front().kind = ev::EntityKind::kUnspecified;
  auto mutated_bytes = ev::CanonicalEvidenceBytes(mutated);
  if (mutated_bytes.ok()) {
    EXPECT_NE(*mutated_bytes, *from_text_bytes)
        << "changing an entity kind did not change the canonical bytes";
  } else {
    // Refusing the mutated case is also a non-vacuity witness: the canonical
    // boundary is looking at it.
    EXPECT_FALSE(mutated_bytes.status().ok());
  }

  // 6. And a mutated case's identity differs, which is what DEM-003's ID
  //    assertion is worth.
  auto mutated_id = ev::ComputeEvidenceId(mutated);
  if (mutated_id.ok()) {
    EXPECT_NE(core::ToString(*mutated_id),
              IdText(from_text.evidence_id));
  }
}

// ---------------------------------------------------------------------------
// DEM-004 — ProtobufOutputIsRequiredAndFailureAtomic
// ---------------------------------------------------------------------------
//
// Failure modes guarded: "Binary stdout or truncated destination."
//
// `--format protobuf` is the one format whose payload is not text, so it is the
// one format that must be told where to put it, and the destination is never
// opened for writing: the bytes go to a uniquely named sibling temporary and
// reach the destination only through one `rename`.
TEST(VeritasQueryEirTest, DEM004ProtobufOutputIsRequiredAndFailureAtomic) {
  auto store = MaterializedStore("evidence_overflow_unsafe");
  ASSERT_TRUE(store.ok()) << store.status().message();

  const std::filesystem::path area =
      std::filesystem::path(::testing::TempDir()) / "dem004";
  std::error_code ignored;
  std::filesystem::remove_all(area, ignored);
  ASSERT_TRUE(std::filesystem::create_directories(area));

  // (a) No destination at all. The payload is binary, so writing it to stdout
  //     would corrupt a terminal or a pipe; the CLI must refuse instead.
  const CliResult missing = RunEvidence(*store, "protobuf", "l1");
  EXPECT_NE(missing.exit_code, 0);
  EXPECT_NE(missing.stdout_text.find("--output"), std::string::npos)
      << missing.stdout_text;

  // (b) A destination whose parent is not a directory. The temporary cannot be
  //     created beside it, so the run fails and nothing appears.
  const std::filesystem::path blocker = area / "blocker";
  {
    std::ofstream file(blocker);
    file << "not a directory\n";
  }
  std::vector<std::string> unwritable = EvidenceArguments(*store, "protobuf", "l1");
  unwritable.push_back("--output");
  unwritable.push_back((blocker / "out.pb").string());
  const CliResult blocked = RunVeritasQuery(unwritable);
  EXPECT_NE(blocked.exit_code, 0);
  EXPECT_FALSE(blocked.stdout_text.empty()) << "a failure must say why";
  EXPECT_FALSE(std::filesystem::exists(blocker / "out.pb"));
  EXPECT_EQ(ReadFile(blocker), "not a directory\n")
      << "the failure damaged an unrelated file";

  // (c) An existing destination that cannot be replaced. The bytes are written
  //     to the temporary first, so the failure happens at the rename and the
  //     temporary is the only path removed. A directory is used as the
  //     destination because `rename` onto one fails deterministically, without
  //     depending on the privileges the test happens to run with.
  const std::filesystem::path occupied = area / "occupied.pb";
  ASSERT_TRUE(std::filesystem::create_directories(occupied));
  {
    std::ofstream marker(occupied / "marker.txt");
    marker << "still here\n";
  }
  std::vector<std::string> replacement =
      EvidenceArguments(*store, "protobuf", "l1");
  replacement.push_back("--output");
  replacement.push_back(occupied.string());
  const CliResult failed = RunVeritasQuery(replacement);
  EXPECT_NE(failed.exit_code, 0);
  EXPECT_TRUE(std::filesystem::is_directory(occupied))
      << "the destination was replaced by a file despite the failure";
  EXPECT_EQ(ReadFile(occupied / "marker.txt"), "still here\n");
  for (const auto& entry : std::filesystem::directory_iterator(area)) {
    EXPECT_EQ(entry.path().filename().string().find(".tmp."), std::string::npos)
        << "a failed write left its temporary behind: " << entry.path();
  }

  // (d) Success: an existing destination is replaced atomically, the payload
  //     decodes, and it is the same case the text representation carries.
  const std::filesystem::path destination = area / "out.pb";
  {
    std::ofstream prior(destination, std::ios::binary | std::ios::trunc);
    prior << "PRIOR CONTENTS\n";
  }
  std::vector<std::string> succeed = EvidenceArguments(*store, "protobuf", "l1");
  succeed.push_back("--output");
  succeed.push_back(destination.string());
  const CliResult written = RunVeritasQuery(succeed);
  ASSERT_EQ(written.exit_code, 0) << written.stdout_text;
  EXPECT_TRUE(written.stdout_text.empty());
  EXPECT_NE(ReadFile(destination), "PRIOR CONTENTS\n")
      << "the destination still holds its prior contents";

  const CliResult text = RunEvidence(*store, "eir-t", "l1");
  ASSERT_EQ(text.exit_code, 0) << text.stdout_text;
  auto parsed = ParseOrFail(text.stdout_text, "the eir-t output");
  ASSERT_TRUE(parsed.ok()) << parsed.status().message();
  auto decoded = ev::DecodeEvidenceProto(ReadFile(destination));
  ASSERT_TRUE(decoded.ok()) << decoded.status().message();
  auto text_bytes = ev::CanonicalEvidenceBytes(*parsed);
  auto proto_bytes = ev::CanonicalEvidenceBytes(*decoded);
  ASSERT_TRUE(text_bytes.ok()) << text_bytes.status().message();
  ASSERT_TRUE(proto_bytes.ok()) << proto_bytes.status().message();
  EXPECT_EQ(*text_bytes, *proto_bytes);

  // No temporary survived the successful write either.
  for (const auto& entry : std::filesystem::directory_iterator(area)) {
    EXPECT_EQ(entry.path().filename().string().find(".tmp."), std::string::npos)
        << "the temporary was not renamed away: " << entry.path();
  }

  std::filesystem::remove_all(area, ignored);
}

// ---------------------------------------------------------------------------
// DEM-005 — UnsafeSafeAndTruncatedGoldensRemainDistinct
// ---------------------------------------------------------------------------
//
// Failure mode guarded: "Safe/truncated output byte-identical to unsafe case."
//
// Each golden is regenerated from its own store and compared SEMANTICALLY, and
// the three are then required to be both byte-distinct and semantically
// distinct. See the file header for what they actually differ by, and for why
// the truncated golden comes from `--max-nodes 1` rather than §14.3's
// `--max-paths 1`.
TEST(VeritasQueryEirTest, DEM005UnsafeSafeAndTruncatedGoldensRemainDistinct) {
  auto unsafe_store = MaterializedStore("evidence_overflow_unsafe");
  ASSERT_TRUE(unsafe_store.ok()) << unsafe_store.status().message();
  auto safe_store = MaterializedStore("evidence_overflow_safe");
  ASSERT_TRUE(safe_store.ok()) << safe_store.status().message();

  const CliResult unsafe = RunEvidence(*unsafe_store, "eir-t", "l1");
  ASSERT_EQ(unsafe.exit_code, 0) << unsafe.stdout_text;
  const CliResult safe = RunEvidence(*safe_store, "eir-t", "l1");
  ASSERT_EQ(safe.exit_code, 0) << safe.stdout_text;
  const std::vector<std::string> truncated_arguments = {
      "evidence", "overflow", "--sink", "memcpy", "--db",
      unsafe_store->string(), "--format", "eir-t", "--level", "l1",
      "--max-nodes", "1"};
  const CliResult truncated = RunVeritasQuery(truncated_arguments);
  ASSERT_EQ(truncated.exit_code, 0) << truncated.stdout_text;

  // Each golden parses and validates, so a golden is a real eir.v1 case and
  // not a transcript. ParseEirText validates and finalizes, and refusing a
  // document whose stored identity is not its recomputed content address is
  // part of that contract — so a parse is also a self-consistency proof.
  struct Golden {
    std::string name;
    const std::string* cli_text;
  };
  const std::vector<Golden> goldens = {
      {"overflow_unsafe.l1.eir", &unsafe.stdout_text},
      {"overflow_safe.l1.eir", &safe.stdout_text},
      {"overflow_truncated.l1.eir", &truncated.stdout_text},
  };
  std::map<std::string, ev::EvidenceCase> cases;
  for (const Golden& golden : goldens) {
    const std::filesystem::path path = GoldenPath(golden.name);
    ASSERT_TRUE(std::filesystem::exists(path))
        << "missing golden; regenerate with the CLI: " << path;
    const std::string golden_text = ReadFile(path);

    auto from_golden = ParseOrFail(golden_text, golden.name);
    ASSERT_TRUE(from_golden.ok())
        << golden.name << ": " << from_golden.status().message();
    EXPECT_TRUE(from_golden->evidence_id.has_value()) << golden.name;
    cases.emplace(golden.name, std::move(*from_golden));

    auto from_cli = ParseOrFail(*golden.cli_text, golden.name + " (CLI)");
    ASSERT_TRUE(from_cli.ok())
        << golden.name << ": " << from_cli.status().message();

    const CaseGraph golden_graph = BuildGraph(cases.at(golden.name));
    EXPECT_TRUE(ReferencesResolve(golden_graph))
        << golden.name
        << ": a reference did not resolve, so the projection would compare a "
           "host-derived handle";
    const CaseGraph cli_graph = BuildGraph(*from_cli);
    EXPECT_TRUE(ReferencesResolve(cli_graph)) << golden.name;

    // The CLI's output and the checked-in golden carry the same semantics.
    EXPECT_EQ(SignatureText(StableSignature(*from_cli)),
              SignatureText(StableSignature(cases.at(golden.name))))
        << golden.name
        << " no longer matches the CLI: the toolchain-stable projection "
           "differs\n--- tool projection ---\n"
        << SignatureText(StableSignature(*from_cli))
        << "--- golden projection ---\n"
        << SignatureText(StableSignature(cases.at(golden.name)));
  }

  const ev::EvidenceCase& unsafe_case = cases.at("overflow_unsafe.l1.eir");
  const ev::EvidenceCase& safe_case = cases.at("overflow_safe.l1.eir");
  const ev::EvidenceCase& truncated_case =
      cases.at("overflow_truncated.l1.eir");

  // Byte-distinct: the failure mode this case names.
  EXPECT_NE(unsafe.stdout_text, safe.stdout_text);
  EXPECT_NE(unsafe.stdout_text, truncated.stdout_text);
  EXPECT_NE(safe.stdout_text, truncated.stdout_text);

  // SEMANTICALLY distinct, by the same instrument used against the goldens.
  const std::string unsafe_signature = SignatureText(StableSignature(unsafe_case));
  const std::string safe_signature = SignatureText(StableSignature(safe_case));
  const std::string truncated_signature =
      SignatureText(StableSignature(truncated_case));
  EXPECT_NE(unsafe_signature, safe_signature);
  EXPECT_NE(unsafe_signature, truncated_signature);
  EXPECT_NE(safe_signature, truncated_signature);

  // Non-vacuity, in three directions. The projection has to inspect the case it
  // is given, and it has to ignore exactly the identities that are not part of
  // the case's meaning — otherwise the golden comparison would pass on any two
  // documents at all, and the masking would be indistinguishable from a
  // comparison that never looked.
  {
    ev::EvidenceCase retyped = unsafe_case;
    ASSERT_FALSE(retyped.entities.empty());
    retyped.entities.front().kind = ev::EntityKind::kUnspecified;
    EXPECT_NE(SignatureText(StableSignature(retyped)), unsafe_signature)
        << "the projection does not inspect an entity's kind";
  }
  {
    // Re-pointing the last segment at an entity the path does not otherwise
    // mention has to move the signature. Without this the projection could be
    // comparing labels alone and never reading a handle at all, and every
    // comparison above would still be green on documents of the same shape.
    //
    // Re-ordering two of the path's own segments would NOT move it, and that is
    // correct rather than a gap: the path is a chain of same-kind `value`
    // entities whose labels are identical after masking, and the edge direction
    // runs from the referrer to the referent, so those entities carry no
    // references of their own and two of them are genuinely indistinguishable.
    // A colour-refinement invariant cannot separate a graph's automorphic
    // vertices, and neither can any reader of the meaning.
    ev::EvidenceCase repointed = unsafe_case;
    ASSERT_EQ(repointed.paths.size(), 1u);
    std::vector<std::string>& chain = repointed.paths.front().entity_ids;
    ASSERT_GE(chain.size(), 2u);
    std::string outsider;
    for (const ev::Entity& entity : repointed.entities) {
      if (entity.kind == ev::EntityKind::kMemoryObject) {
        outsider = entity.id;
        break;
      }
    }
    ASSERT_FALSE(outsider.empty());
    ASSERT_EQ(std::count(chain.begin(), chain.end(), outsider), 0);
    chain.back() = outsider;
    EXPECT_NE(SignatureText(StableSignature(repointed)), unsafe_signature)
        << "the projection ignores a path's segments, so it compares labels "
           "alone and never reads the handle graph";
  }
  {
    ev::EvidenceCase reidentified = unsafe_case;
    ASSERT_FALSE(reidentified.entities.empty());
    ASSERT_TRUE(reidentified.entities.front().stable_id.has_value());
    core::StableId other = *reidentified.entities.front().stable_id;
    ASSERT_FALSE(other.digest_hex.empty());
    other.digest_hex[0] = other.digest_hex[0] == '0' ? '1' : '0';
    reidentified.entities.front().stable_id = other;
    EXPECT_EQ(SignatureText(StableSignature(reidentified)), unsafe_signature)
        << "the projection leaked a content address into the comparison, so it "
           "is not usable across toolchains";
  }

  // The differences themselves, asserted rather than assumed.
  //
  // Safe and unsafe carry the SAME verdict and the same derived negative
  // absence: the safe fixture's difference is the flow shape it was built
  // around, not a promotion to `VERIFIED_SAFE` and not positive
  // dominating-check counterevidence (`dominating_checks` is the same
  // complete-empty query on both — M9/M10A derives no positive check fact).
  EXPECT_EQ(unsafe_case.verification_state, ev::VerificationState::kPossibleDefect);
  EXPECT_EQ(safe_case.verification_state, ev::VerificationState::kPossibleDefect);
  EXPECT_EQ(unsafe_case.verification_state, safe_case.verification_state);
  EXPECT_EQ(unsafe_case.primary_claim.kind, safe_case.primary_claim.kind);
  EXPECT_EQ(unsafe_case.primary_claim.severity, safe_case.primary_claim.severity);
  EXPECT_EQ(unsafe_case.paths.size(), 1u);
  EXPECT_EQ(safe_case.paths.size(), 1u);
  EXPECT_NE(unsafe_case.paths.front().entity_ids.size(),
            safe_case.paths.front().entity_ids.size())
      << "the two fixtures were expected to differ in flow length";
  EXPECT_NE(unsafe_case.facts.size(), safe_case.facts.size());

  // The truncated case is partial evidence with no marker for its partiality:
  // the flow slice's nodes and edges are gone, so there is no path to emit, and
  // `BuildPaths` never consults the slice's completeness, so nothing records
  // that the path was cut rather than absent. The derived negative absence
  // survives, because the dominating-check query is a different query and is
  // still complete.
  EXPECT_GT(unsafe_case.paths.size(), 0u);
  EXPECT_EQ(truncated_case.paths.size(), 0u);
  EXPECT_EQ(truncated_case.edges.size(), 0u);
  EXPECT_EQ(truncated_case.omissions.size(), 0u);
  EXPECT_EQ(truncated_case.unknowns.size(), 0u);
  EXPECT_EQ(truncated_case.primary_claim.kind,
            unsafe_case.primary_claim.kind);
  EXPECT_EQ(truncated_case.verification_state, unsafe_case.verification_state);

  const auto has_negative_absence = [](const ev::EvidenceCase& value) {
    for (const ev::Fact& fact : value.facts) {
      if (fact.predicate.kind == ev::Expression::Kind::kCall &&
          fact.predicate.text == "dominates_bounds_check" &&
          fact.epistemic == ev::EpistemicState::kMustNot) {
        return true;
      }
    }
    return false;
  };
  EXPECT_TRUE(has_negative_absence(unsafe_case));
  EXPECT_TRUE(has_negative_absence(safe_case));
  EXPECT_TRUE(has_negative_absence(truncated_case));

  // Three distinct content addresses. Same build, so the addresses are
  // comparable — and this is what makes "the goldens are distinct cases" a
  // statement about the model rather than about the text's whitespace.
  EXPECT_NE(core::ToString(*unsafe_case.evidence_id),
            core::ToString(*safe_case.evidence_id));
  EXPECT_NE(core::ToString(*unsafe_case.evidence_id),
            core::ToString(*truncated_case.evidence_id));
  EXPECT_NE(core::ToString(*safe_case.evidence_id),
            core::ToString(*truncated_case.evidence_id));
}

// ---------------------------------------------------------------------------
// DEM-006 — RepeatedRunsAndCheckoutRootsAreDeterministic
// ---------------------------------------------------------------------------
//
// Failure mode guarded: "Absolute path, store order, or run timing changes
// output." Two independently materialized stores in two differently named
// checkout roots, with fresh stores and separate analyses, must publish
// byte-identical output in all four formats.
//
// The fixtures compile with `-fdebug-prefix-map=@PROJECT_ROOT@=.`, so the
// checkout root cannot reach the debug info; without it the two runs would
// differ, and this test is what would say so.
//
// Every comparison here is SAME-BUILD byte equality, and each format is
// asserted separately because each has its own writer and its own ordering
// rules.
TEST(VeritasQueryEirTest, DEM006RepeatedRunsAndCheckoutRootsAreDeterministic) {
  auto first = MaterializedStore("evidence_overflow_unsafe");
  ASSERT_TRUE(first.ok()) << first.status().message();

  // `MaterializedStore` caches per process, and this test needs two INDEPENDENT
  // materializations, so the second root is materialized directly rather than
  // through the cache.
  auto second_snapshot = AnalyzeRealFixture("evidence_overflow_unsafe");
  ASSERT_TRUE(second_snapshot.ok()) << second_snapshot.status().message();
  const std::filesystem::path second_root = second_snapshot->output_root;
  ASSERT_NE(first->string(), second_root.string());

  ASSERT_TRUE(std::filesystem::exists(*first / "metadata.db"));
  ASSERT_TRUE(std::filesystem::exists(second_root / "metadata.db"));

  const std::filesystem::path area =
      std::filesystem::path(::testing::TempDir()) / "dem006";
  std::error_code ignored;
  std::filesystem::remove_all(area, ignored);
  ASSERT_TRUE(std::filesystem::create_directories(area));

  for (const std::string_view format : {"eir-t", "eir-json"}) {
    const CliResult left = RunEvidence(*first, format, "l1");
    ASSERT_EQ(left.exit_code, 0) << format << ": " << left.stdout_text;
    const CliResult right = RunEvidence(second_root, format, "l1");
    ASSERT_EQ(right.exit_code, 0) << format << ": " << right.stdout_text;
    EXPECT_FALSE(left.stdout_text.empty()) << format;
    EXPECT_EQ(left.stdout_text, right.stdout_text)
        << format << " is not byte-stable across stores and checkout roots";
  }

  // The two stores must be two *distinct* materializations for the comparison
  // above to mean anything: same semantics, different directories.
  auto left_case = ParseOrFail(RunEvidence(*first, "eir-t", "l1").stdout_text,
                               "first eir-t");
  ASSERT_TRUE(left_case.ok()) << left_case.status().message();
  auto right_case = ParseOrFail(
      RunEvidence(second_root, "eir-t", "l1").stdout_text, "second eir-t");
  ASSERT_TRUE(right_case.ok()) << right_case.status().message();
  EXPECT_EQ(SignatureText(StableSignature(*left_case)),
            SignatureText(StableSignature(*right_case)));

  // The binary format separately: it has its own writer and its own atomic
  // replacement path.
  std::vector<std::string> left_proto = EvidenceArguments(*first, "protobuf", "l1");
  left_proto.push_back("--output");
  left_proto.push_back((area / "left.pb").string());
  ASSERT_EQ(RunVeritasQuery(left_proto).exit_code, 0);
  std::vector<std::string> right_proto =
      EvidenceArguments(second_root, "protobuf", "l1");
  right_proto.push_back("--output");
  right_proto.push_back((area / "right.pb").string());
  ASSERT_EQ(RunVeritasQuery(right_proto).exit_code, 0);
  const std::string left_bytes = ReadFile(area / "left.pb");
  EXPECT_FALSE(left_bytes.empty());
  EXPECT_EQ(left_bytes, ReadFile(area / "right.pb"))
      << "protobuf is not byte-stable across stores and checkout roots";

  // And the identity those bytes carry is the identity the text carries.
  auto left_decoded = ev::DecodeEvidenceProto(left_bytes);
  ASSERT_TRUE(left_decoded.ok()) << left_decoded.status().message();
  EXPECT_EQ(core::ToString(*left_case->evidence_id),
            core::ToString(*left_decoded->evidence_id));

  // Repeating the SAME store must also reproduce the same bytes: run timing is
  // not allowed to reach the output either.
  const CliResult repeated = RunEvidence(*first, "eir-t", "l1");
  ASSERT_EQ(repeated.exit_code, 0) << repeated.stdout_text;
  const CliResult once_more = RunEvidence(*first, "eir-t", "l1");
  ASSERT_EQ(once_more.exit_code, 0) << once_more.stdout_text;
  EXPECT_EQ(repeated.stdout_text, once_more.stdout_text);

  std::filesystem::remove_all(area, ignored);
}

}  // namespace veritas::testing
