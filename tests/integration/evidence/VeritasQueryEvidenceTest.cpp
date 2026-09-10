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

// VeritasQueryEvidenceTest.cpp — DEM-001.
//
// `veritas-query evidence overflow` must publish the M10B slice as deterministic
// diagnostic JSON. These tests materialize the unsafe fixture through the real
// M1→M6→M9→M10A pipeline, build the typed oracle in process, then run the
// PUBLIC CLI against the same materialized store. Typed content is validated
// first (claim seed, the real value-flow path, the pinned run, provenance, and
// the completeness of every fact set), and only then is output compared.
//
// Two comparison strengths are used, deliberately:
//
//   * BYTE-identical, for runs inside one build: CLI stdout vs
//     `ToDiagnosticJson(oracle)`, vs a second fresh store in another checkout
//     root, and vs the same store with its fact bindings re-inserted backwards.
//     Same toolchain → same `analysis_run_id` → byte-stable output.
//
//   * SEMANTIC, against the checked-in golden. The golden CANNOT be compared
//     byte-for-byte: `analysis_run_id` derives from
//     `engine_toolchain_identity = "souffle-" + SHA256(<vendored souffle
//     executable bytes>)`, and that executable is not bit-reproducible, so a
//     clean rebuild of the vendored subtree changes the digest and with it the
//     run id, the six query-completion fact ids, the run bindings, the
//     per-query provenance ids, and the canonical ordering that sorts by those
//     ids (Task 5 verification reproduced three distinct digests and three
//     distinct run ids from three clean rebuilds). Everything semantic — the
//     claim seed, the CPG flow nodes/edges, the GlobalFlow support, the fact
//     sets, and every completeness state — is identical across those builds.
//     The golden comparison therefore parses both documents and compares only
//     the toolchain-stable fields; see `SemanticSlice`. Making the vendored
//     Soufflé build bit-reproducible is a third_party build follow-up.
//
// The JSON is the DESC0PED slice. What the real pipeline produces and what this
// CLI therefore publishes is the value-flow closure (GlobalFlow projected onto
// CPG kFlowsTo edges, from the origin value to the value that leaves the
// analyzed code into the unmodeled sink) plus the provenance closure of the six
// per-query completion certificates.
//
// Two slots are complete-EMPTY by construction of BuildEvidenceInput, not by
// omission here, and are serialized faithfully rather than dropped:
//   * `dominating_checks` matches only POSITIVE "dominating_check" facts, and
//     M9/M10A derives only the negative "dominating_check_absence" certificate
//     (asserted directly on the fact store by OverflowEvidenceFixtureTest).
//   * `unknowns` is scoped through FindContainingFunction, which resolves a
//     node to its function through a kContains edge. The M6 projection emits no
//     kContains edges, so the scope falls back to the degenerate empty
//     FunctionVariant ref and no UnknownEffect fact matches.
// DEFERRED (scoping decision, not a defect): value-range [0,65535], capacity
// 2048, alias states, and the positive "dominating_check" fact are DEFERRED to
// a later milestone — M9/M10A does not emit them.
//
// The evidence_overflow_* fixtures compile with
// `-fdebug-prefix-map=@PROJECT_ROOT@=.` so the materialized checkout root cannot
// leak into debug info; without it the slice JSON differs between two
// materializations of the same source (asserted below).

#include <array>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>

#include "evidence/RealEvidencePipeline.h"
#include "veritas/analysis/semantic/SemanticTypes.h"
#include "veritas/core/Ids.h"
#include "veritas/cpg/CpgTypes.h"
#include "veritas/evidence/EvidenceQueryService.h"
#include "veritas/evidence/FactStoreEvidenceBackend.h"
#include "veritas/evidence/OverflowClaimSeed.h"
#include "veritas/evidence/SliceTypes.h"
#include "veritas/facts/AnalysisFact.h"
#include "veritas/facts/RelationSchema.h"
#include "veritas/summarydb/MetadataStore.h"

#ifndef VERITAS_QUERY_BINARY
#error "VERITAS_QUERY_BINARY must be defined by the build system"
#endif
#ifndef VERITAS_GOLDEN_DIR
#error "VERITAS_GOLDEN_DIR must be defined by the build system"
#endif

namespace veritas::testing {
namespace {

namespace ev = evidence;

// Mirrors the public CLI defaults documented in the M10B design spec §6; the
// test passes them explicitly so a change to either side is caught.
ev::EvidenceQueryBudget Budget() {
  return ev::EvidenceQueryBudget{/*max_depth=*/8, /*max_nodes=*/256,
                                 /*max_paths=*/5, /*max_facts_per_query=*/64,
                                 /*max_provenance_depth=*/8};
}

std::vector<std::string> BudgetFlags() {
  return {"--max-depth",    "8", "--max-nodes", "256",
          "--max-paths",    "5", "--max-facts", "64",
          "--max-provenance-depth", "8"};
}

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

CliResult RunEvidenceOverflow(const std::filesystem::path& db_dir) {
  std::vector<std::string> arguments{"evidence", "overflow", "--sink", "memcpy",
                                     "--format", "json", "--db", db_dir.string()};
  const auto budget = BudgetFlags();
  arguments.insert(arguments.end(), budget.begin(), budget.end());
  return RunVeritasQuery(arguments);
}

// The in-process typed oracle: the exact `EvidenceBuildInput` the CLI must
// serialize, built from the same materialized store through the same public
// service entry point, plus the store root the CLI is pointed at.
struct Oracle {
  ev::EvidenceBuildInput input;
  cpg::ThinCpg cpg;
  std::filesystem::path output_root;
};

StatusOr<Oracle> BuildOracle(std::string_view fixture) {
  auto snapshot = AnalyzeRealFixture(fixture);
  if (!snapshot.ok()) {
    return snapshot.status();
  }
  auto facts = snapshot->fact_store.GetCurrentFacts(snapshot->run_id);
  if (!facts.ok()) {
    return facts.status();
  }
  // The CLI resolves the seed itself; the oracle resolves it through the same
  // shared production entry point so the two cannot drift. The CLI's own
  // resolution is exercised end to end by the byte comparison below.
  auto seed = ev::ResolveOverflowClaimSeed(snapshot->cpg, *facts, "memcpy");
  if (!seed.ok()) {
    return seed.status();
  }
  evidence::FactStoreEvidenceBackend backend(snapshot->fact_store,
                                             snapshot->descriptor);
  ev::EvidenceQueryService service(snapshot->cpg, backend, snapshot->run_id);
  auto input = service.BuildEvidenceInput(*seed, Budget());
  if (!input.ok()) {
    return input.status();
  }
  return Oracle{.input = std::move(*input),
                .cpg = std::move(snapshot->cpg),
                .output_root = snapshot->output_root};
}

// Typed assertions on the oracle before any byte comparison.
void ExpectTypedSliceContent(const ev::EvidenceBuildInput& input,
                             const cpg::ThinCpg& cpg) {
  // Claim seed: a buffer-overflow finding with a resolvable subject/source/sink.
  EXPECT_EQ(input.claim_seed.kind, ev::ClaimKind::kBufferOverflow);
  EXPECT_EQ(input.claim_seed.severity, ev::Severity::kHigh);
  EXPECT_EQ(input.claim_seed.finding_id.kind, core::IdKind::kFact);
  EXPECT_EQ(input.claim_seed.subject_ref.kind, core::IdKind::kMemoryRef);
  EXPECT_EQ(input.claim_seed.source_ref.kind, core::IdKind::kValueRef);
  EXPECT_EQ(input.claim_seed.sink_ref.kind, core::IdKind::kValueRef);
  EXPECT_NE(input.claim_seed.subject_ref.digest_hex, std::string());
  EXPECT_NE(input.claim_seed.source_ref.digest_hex, std::string());
  EXPECT_NE(input.claim_seed.sink_ref.digest_hex, std::string());

  // The value-flow path the CLI publishes is non-empty, and every node and edge
  // it contains is a real member of the pinned projection (no fabricated graph
  // content).
  EXPECT_FALSE(input.flow_slice.nodes.empty());
  EXPECT_FALSE(input.flow_slice.edges.empty());
  std::set<core::StableId> cpg_nodes;
  for (const auto& node : cpg.nodes()) {
    cpg_nodes.insert(node.node_id);
  }
  std::set<core::StableId> cpg_edges;
  for (const auto& edge : cpg.edges()) {
    cpg_edges.insert(edge.edge_id);
  }
  for (const auto& node : input.flow_slice.nodes) {
    EXPECT_EQ(cpg_nodes.count(node.node_id), 1u) << node.label;
  }
  for (const auto& edge : input.flow_slice.edges) {
    EXPECT_EQ(cpg_edges.count(edge.edge_id), 1u);
  }
  // The seed's endpoints are the ends of the published path.
  EXPECT_EQ(cpg_nodes.count(input.claim_seed.source_ref), 1u);
  EXPECT_EQ(cpg_nodes.count(input.claim_seed.sink_ref), 1u);

  // Each query result is completeness-qualified and carries the pinned run.
  EXPECT_EQ(input.flow_slice.metadata.completeness,
            ev::QueryCompleteness::kComplete);
  EXPECT_TRUE(input.flow_slice.metadata.truncation_reasons.empty());
  EXPECT_EQ(input.flow_slice.metadata.analysis_run_id.kind,
            core::IdKind::kAnalysisRun);

  // Deferred relations are complete-empty, not omitted and not fabricated.
  for (const ev::EvidenceFactSet* set :
       {&input.ranges, &input.capacities, &input.aliases,
        &input.dominating_checks, &input.unknowns}) {
    EXPECT_EQ(set->metadata.completeness, ev::QueryCompleteness::kComplete);
    EXPECT_TRUE(set->metadata.truncation_reasons.empty());
    EXPECT_TRUE(set->facts.empty());
  }

  // The one analysis run is pinned and every query's completion certificate is
  // resolvable.
  EXPECT_FALSE(input.query_completion_facts.empty());
  EXPECT_FALSE(input.query_completion_bindings.empty());
  EXPECT_GT(input.provenance.nodes_size(), 0);
}

// "Reverse backend insertion": re-insert the store's run fact bindings in the
// opposite physical order. The durable read surface is content-ordered — every
// read the evidence path issues is served by a covering index keyed on the
// content-derived id (`run_fact_bindings_current` for the fact query,
// `(projection_id, node_id)` for the projection), so `GetCurrentFacts` already
// yields fact-id order and insertion order is not observable today. Reversing
// it is therefore a regression guard: if a store change ever makes physical row
// order visible, this comparison fails instead of silently reordering the
// published slice.
void ReverseFactBindingInsertionOrder(const std::filesystem::path& db_dir) {
  auto store = veritas::summarydb::MetadataStore::Open(db_dir / "metadata.db");
  ASSERT_TRUE(store.ok()) << store.status().message();

  const auto first_fact = [&](std::string_view order_by) {
    auto rows = store->Query(
        "SELECT fact_id FROM run_fact_bindings WHERE is_current = 1 " +
            std::string(order_by) + " LIMIT 1",
        {});
    EXPECT_TRUE(rows.ok()) << rows.status().message();
    return rows.ok() && !rows->empty() ? (*rows)[0][0] : std::string();
  };

  const std::string before_rowid = first_fact("ORDER BY rowid");
  const std::string before_index = first_fact("");
  EXPECT_FALSE(before_rowid.empty());

  // binding_id is the table's rowid and is excluded, so the re-inserted rows
  // are assigned fresh, ascending rowids in the reversed sequence.
  static constexpr std::string_view kBindingColumns =
      "run_id, fact_id, confidence, producer_kind, analyzer_run_id, scope_kind, "
      "scope_id, selected_witness_id, is_current";
  const std::string create =
      "CREATE TABLE veritas_reversed_bindings AS SELECT " +
      std::string(kBindingColumns) + " FROM run_fact_bindings ORDER BY rowid DESC";
  const std::string insert = "INSERT INTO run_fact_bindings (" +
                             std::string(kBindingColumns) +
                             ") SELECT * FROM veritas_reversed_bindings";
  for (const std::string& statement :
       {create, std::string("DELETE FROM run_fact_bindings"), insert,
        std::string("DROP TABLE veritas_reversed_bindings")}) {
    const Status status = store->Execute(statement, {});
    ASSERT_TRUE(status.ok()) << status.message();
  }

  // The physical insertion order really was reversed ...
  EXPECT_NE(first_fact("ORDER BY rowid"), before_rowid)
      << "the reversal did not rewrite the physical row order";
  // ... while the read the evidence path performs is index-served and therefore
  // unchanged, which is why the CLI cannot observe insertion order.
  EXPECT_EQ(first_fact(""), before_index);
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::ostringstream contents;
  contents << file.rdbuf();
  return contents.str();
}

// ---------------------------------------------------------------------------
// Semantic (toolchain-stable) comparison of two slice documents
// ---------------------------------------------------------------------------
//
// ToDiagnosticJson is a debug representation with no production reader, so the
// reader below is test-local by design: DEM-001 exists precisely to pin the
// `--format json` document's shape and content.

// A minimal JSON tree: objects, arrays, strings, numbers, booleans, null. The
// slice document contains nothing else.
struct JsonNode {
  enum class Kind { kNull, kBool, kNumber, kString, kArray, kObject };

  Kind kind = Kind::kNull;
  bool boolean = false;
  std::string text;  // string body, number literal, or "true"/"false"/"null"
  std::vector<JsonNode> items;                             // array elements
  std::vector<std::pair<std::string, JsonNode>> members;   // object members

  const JsonNode* Member(std::string_view name) const {
    for (const auto& entry : members) {
      if (entry.first == name) {
        return &entry.second;
      }
    }
    return nullptr;
  }
};

class JsonReader {
 public:
  explicit JsonReader(std::string_view text) : text_(text) {}

  bool Parse(JsonNode* out) {
    SkipSpace();
    if (!ParseValue(out)) {
      return false;
    }
    SkipSpace();
    return pos_ == text_.size();
  }

 private:
  bool ParseValue(JsonNode* out) {
    if (pos_ >= text_.size()) {
      return false;
    }
    switch (text_[pos_]) {
      case '{':
        return ParseObject(out);
      case '[':
        return ParseArray(out);
      case '"':
        out->kind = JsonNode::Kind::kString;
        return ParseString(&out->text);
      case 't':
        return ParseLiteral("true", JsonNode::Kind::kBool, true, out);
      case 'f':
        return ParseLiteral("false", JsonNode::Kind::kBool, false, out);
      case 'n':
        return ParseLiteral("null", JsonNode::Kind::kNull, false, out);
      default:
        out->kind = JsonNode::Kind::kNumber;
        return ParseNumber(&out->text);
    }
  }

  bool ParseLiteral(std::string_view literal, JsonNode::Kind kind, bool boolean,
                    JsonNode* out) {
    if (text_.compare(pos_, literal.size(), literal) != 0) {
      return false;
    }
    pos_ += literal.size();
    out->kind = kind;
    out->boolean = boolean;
    out->text = std::string(literal);
    return true;
  }

  bool ParseObject(JsonNode* out) {
    out->kind = JsonNode::Kind::kObject;
    ++pos_;  // '{'
    SkipSpace();
    if (pos_ < text_.size() && text_[pos_] == '}') {
      ++pos_;
      return true;
    }
    while (true) {
      SkipSpace();
      std::string key;
      if (!ParseString(&key)) {
        return false;
      }
      SkipSpace();
      if (pos_ >= text_.size() || text_[pos_] != ':') {
        return false;
      }
      ++pos_;
      SkipSpace();
      JsonNode value;
      if (!ParseValue(&value)) {
        return false;
      }
      out->members.emplace_back(std::move(key), std::move(value));
      SkipSpace();
      if (pos_ >= text_.size()) {
        return false;
      }
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (text_[pos_] == '}') {
        ++pos_;
        return true;
      }
      return false;
    }
  }

  bool ParseArray(JsonNode* out) {
    out->kind = JsonNode::Kind::kArray;
    ++pos_;  // '['
    SkipSpace();
    if (pos_ < text_.size() && text_[pos_] == ']') {
      ++pos_;
      return true;
    }
    while (true) {
      SkipSpace();
      JsonNode value;
      if (!ParseValue(&value)) {
        return false;
      }
      out->items.push_back(std::move(value));
      SkipSpace();
      if (pos_ >= text_.size()) {
        return false;
      }
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (text_[pos_] == ']') {
        ++pos_;
        return true;
      }
      return false;
    }
  }

  bool ParseString(std::string* out) {
    if (pos_ >= text_.size() || text_[pos_] != '"') {
      return false;
    }
    ++pos_;
    out->clear();
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '"') {
        return true;
      }
      if (c != '\\') {
        out->push_back(c);
        continue;
      }
      if (pos_ >= text_.size()) {
        return false;
      }
      const char escape = text_[pos_++];
      switch (escape) {
        case '"':
        case '\\':
        case '/':
          out->push_back(escape);
          break;
        case 'b':
          out->push_back('\b');
          break;
        case 'f':
          out->push_back('\f');
          break;
        case 'n':
          out->push_back('\n');
          break;
        case 'r':
          out->push_back('\r');
          break;
        case 't':
          out->push_back('\t');
          break;
        case 'u':
          // The slice document carries only ASCII (ids, symbol names, enums),
          // so the four hex digits are consumed and dropped.
          if (pos_ + 4 > text_.size()) {
            return false;
          }
          pos_ += 4;
          out->push_back('?');
          break;
        default:
          return false;
      }
    }
    return false;
  }

  bool ParseNumber(std::string* out) {
    const std::size_t start = pos_;
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' ||
          c == 'e' || c == 'E') {
        ++pos_;
        continue;
      }
      break;
    }
    if (pos_ == start) {
      return false;
    }
    *out = std::string(text_.substr(start, pos_ - start));
    return true;
  }

  void SkipSpace() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
        ++pos_;
        continue;
      }
      break;
    }
  }

  std::string_view text_;
  std::size_t pos_ = 0;
};

Status ParseJson(std::string_view text, JsonNode* out) {
  if (!JsonReader(text).Parse(out)) {
    return Status::InvalidArgument("malformed slice JSON");
  }
  return Status::Ok();
}

const JsonNode& RequireMember(const JsonNode& node, std::string_view name) {
  const JsonNode* member = node.Member(name);
  // Every call site checks presence first and reports a Status, so reaching
  // here without the member is a bug in this file, not bad input.
  assert(member != nullptr);
  return *member;
}

// One fact row reduced to everything an independent clean build of the
// toolchain cannot change: the canonical fact id, the relation name, and every
// cell. Cells are rendered with an explicit kind tag so a string cell never
// compares equal to a numeric cell carrying the same digits.
struct FactRowView {
  core::StableId fact_id;
  std::string relation;
  std::vector<std::string> cells;

  bool operator==(const FactRowView& other) const {
    return fact_id == other.fact_id && relation == other.relation &&
           cells == other.cells;
  }
};

std::string RenderFactRow(const FactRowView& row) {
  std::string out = core::ToString(row.fact_id) + " [" + row.relation + "]";
  for (const std::string& cell : row.cells) {
    out += " | " + cell;
  }
  return out;
}

// A fact set reduced to the fields that survive an independent clean build of
// the toolchain: `QueryResultMetadata` minus its run-derived `analysis_run_id`
// and `query_provenance_id`.
struct FactSetView {
  std::vector<FactRowView> facts;
  ev::QueryCompleteness completeness = ev::QueryCompleteness::kUnspecified;
  std::vector<ev::TruncationReason> truncation_reasons;
  std::size_t examined_items = 0;
};

// The toolchain-stable projection of one slice document.
struct SemanticSlice {
  ev::ClaimSeed claim_seed;
  std::vector<core::StableId> flow_nodes;
  std::vector<core::StableId> flow_edges;
  std::vector<FactRowView> flow_supporting_facts;
  std::vector<FactRowView> flow_contradicting_facts;
  std::vector<FactRowView> flow_unknowns;
  std::vector<core::StableId> flow_provenance_refs;
  FactSetView flow;
  FactSetView ranges;
  FactSetView capacities;
  FactSetView aliases;
  FactSetView dominating_checks;
  FactSetView unknowns;
};

StatusOr<std::size_t> ReadCount(const JsonNode& node) {
  if (node.kind != JsonNode::Kind::kNumber) {
    return Status::InvalidArgument("expected a JSON number");
  }
  std::size_t value = 0;
  const auto [ptr, ec] =
      std::from_chars(node.text.data(), node.text.data() + node.text.size(),
                      value);
  if (ec != std::errc() || ptr != node.text.data() + node.text.size()) {
    return Status::InvalidArgument("malformed JSON number '" + node.text + "'");
  }
  return value;
}

StatusOr<core::StableId> ReadStableId(const JsonNode& node) {
  if (node.kind != JsonNode::Kind::kString) {
    return Status::InvalidArgument("expected a JSON string id");
  }
  return core::ParseStableId(node.text);
}

// Reads the `QueryResultMetadata` members that a clean rebuild cannot change.
// `analysis_run_id` and `query_provenance_id` are deliberately not read.
StatusOr<FactSetView> ReadMetadataView(const JsonNode& metadata) {
  if (metadata.kind != JsonNode::Kind::kObject) {
    return Status::InvalidArgument("metadata is not an object");
  }
  for (const std::string_view name :
       {"completeness", "truncation_reasons", "examined_items"}) {
    if (metadata.Member(name) == nullptr) {
      return Status::InvalidArgument("metadata is missing '" +
                                     std::string(name) + "'");
    }
  }
  FactSetView view;
  auto completeness =
      ev::ParseQueryCompleteness(RequireMember(metadata, "completeness").text);
  if (!completeness.ok()) {
    return completeness.status();
  }
  view.completeness = *completeness;
  const JsonNode& reasons = RequireMember(metadata, "truncation_reasons");
  if (reasons.kind != JsonNode::Kind::kArray) {
    return Status::InvalidArgument("truncation_reasons is not an array");
  }
  for (const JsonNode& reason : reasons.items) {
    auto parsed = ev::ParseTruncationReason(reason.text);
    if (!parsed.ok()) {
      return parsed.status();
    }
    view.truncation_reasons.push_back(*parsed);
  }
  auto examined = ReadCount(RequireMember(metadata, "examined_items"));
  if (!examined.ok()) {
    return examined.status();
  }
  view.examined_items = *examined;
  return view;
}

// One serialized fact row: `fact_id`, `relation`, and the ordered `cells`.
StatusOr<FactRowView> ReadFactRow(const JsonNode& fact) {
  if (fact.kind != JsonNode::Kind::kObject) {
    return Status::InvalidArgument("expected a JSON fact object");
  }
  const JsonNode* id = fact.Member("fact_id");
  if (id == nullptr) {
    return Status::InvalidArgument("fact is missing fact_id");
  }
  const JsonNode* relation = fact.Member("relation");
  if (relation == nullptr || relation->kind != JsonNode::Kind::kString) {
    return Status::InvalidArgument("fact is missing a string relation");
  }
  const JsonNode* cells = fact.Member("cells");
  if (cells == nullptr || cells->kind != JsonNode::Kind::kArray) {
    return Status::InvalidArgument("fact is missing a cells array");
  }
  auto parsed_id = ReadStableId(*id);
  if (!parsed_id.ok()) {
    return parsed_id.status();
  }
  FactRowView row;
  row.fact_id = *parsed_id;
  row.relation = relation->text;
  row.cells.reserve(cells->items.size());
  for (const JsonNode& cell : cells->items) {
    switch (cell.kind) {
      case JsonNode::Kind::kString:
        row.cells.push_back("s:" + cell.text);
        break;
      case JsonNode::Kind::kNumber:
        row.cells.push_back("n:" + cell.text);
        break;
      case JsonNode::Kind::kBool:
        row.cells.push_back("b:" + cell.text);
        break;
      default:
        return Status::InvalidArgument("unsupported fact cell encoding");
    }
  }
  return row;
}

// The fact rows of the array member `name` of `node`.
StatusOr<std::vector<FactRowView>> ReadFactRows(const JsonNode& node,
                                                std::string_view name) {
  const JsonNode* member = node.Member(name);
  if (member == nullptr) {
    return Status::InvalidArgument("missing JSON member '" + std::string(name) +
                                   "'");
  }
  if (member->kind != JsonNode::Kind::kArray) {
    return Status::InvalidArgument("'" + std::string(name) +
                                   "' is not a JSON array");
  }
  std::vector<FactRowView> rows;
  rows.reserve(member->items.size());
  for (const JsonNode& item : member->items) {
    auto row = ReadFactRow(item);
    if (!row.ok()) {
      return row.status();
    }
    rows.push_back(std::move(*row));
  }
  return rows;
}

// The ids of an array of bare ID strings (`provenance_refs`), in document order.
StatusOr<std::vector<core::StableId>> ReadPlainIdArray(const JsonNode& node,
                                                       std::string_view name) {
  const JsonNode* member = node.Member(name);
  if (member == nullptr) {
    return Status::InvalidArgument("missing JSON member '" + std::string(name) +
                                   "'");
  }
  if (member->kind != JsonNode::Kind::kArray) {
    return Status::InvalidArgument("'" + std::string(name) +
                                   "' is not a JSON array");
  }
  std::vector<core::StableId> ids;
  ids.reserve(member->items.size());
  for (const JsonNode& item : member->items) {
    auto parsed = ReadStableId(item);
    if (!parsed.ok()) {
      return parsed.status();
    }
    ids.push_back(*parsed);
  }
  return ids;
}

StatusOr<FactSetView> ReadFactSet(const JsonNode& node) {
  if (node.kind != JsonNode::Kind::kObject) {
    return Status::InvalidArgument("expected a JSON fact set object");
  }
  if (node.Member("facts") == nullptr || node.Member("metadata") == nullptr) {
    return Status::InvalidArgument("fact set is missing facts or metadata");
  }
  auto view = ReadMetadataView(RequireMember(node, "metadata"));
  if (!view.ok()) {
    return view.status();
  }
  auto rows = ReadFactRows(node, "facts");
  if (!rows.ok()) {
    return rows.status();
  }
  view->facts = std::move(*rows);
  return view;
}

// The ids of the array member `name` of `node`, in document order.
StatusOr<std::vector<core::StableId>> ReadIdArray(const JsonNode& node,
                                                  std::string_view name,
                                                  std::string_view id_key) {
  const JsonNode* member = node.Member(name);
  if (member == nullptr) {
    return Status::InvalidArgument("missing JSON member '" + std::string(name) +
                                   "'");
  }
  if (member->kind != JsonNode::Kind::kArray) {
    return Status::InvalidArgument("'" + std::string(name) +
                                   "' is not a JSON array");
  }
  std::vector<core::StableId> ids;
  ids.reserve(member->items.size());
  for (const JsonNode& item : member->items) {
    const JsonNode* id = item.Member(id_key);
    if (id == nullptr) {
      return Status::InvalidArgument("'" + std::string(name) +
                                     "' entry is missing '" +
                                     std::string(id_key) + "'");
    }
    auto parsed = ReadStableId(*id);
    if (!parsed.ok()) {
      return parsed.status();
    }
    ids.push_back(*parsed);
  }
  return ids;
}

// Parses one slice document into its toolchain-stable projection. Every member
// the diagnostic format defines is required to be present (DEM-001's point is
// that `--format json` stays slice JSON); only the run-bearing members are
// dropped from the result.
StatusOr<SemanticSlice> ParseSemanticSlice(std::string_view json) {
  JsonNode root;
  if (Status status = ParseJson(json, &root); !status.ok()) {
    return status;
  }
  if (root.kind != JsonNode::Kind::kObject) {
    return Status::InvalidArgument("slice JSON is not an object");
  }
  for (const std::string_view name :
       {"claim_seed", "flow_slice", "ranges", "capacities", "aliases",
        "dominating_checks", "unknowns", "query_completion_facts",
        "query_completion_bindings", "provenance"}) {
    if (root.Member(name) == nullptr) {
      return Status::InvalidArgument("slice JSON is missing '" +
                                     std::string(name) + "'");
    }
  }

  SemanticSlice slice;
  const JsonNode& seed = RequireMember(root, "claim_seed");
  for (const std::string_view name :
       {"finding_id", "kind", "severity", "subject_ref", "source_ref",
        "sink_ref"}) {
    if (seed.Member(name) == nullptr) {
      return Status::InvalidArgument("claim seed is missing '" +
                                     std::string(name) + "'");
    }
  }
  auto finding_id = ReadStableId(RequireMember(seed, "finding_id"));
  auto kind = ev::ParseClaimKind(RequireMember(seed, "kind").text);
  auto severity = ev::ParseSeverity(RequireMember(seed, "severity").text);
  auto subject = ReadStableId(RequireMember(seed, "subject_ref"));
  auto source = ReadStableId(RequireMember(seed, "source_ref"));
  auto sink = ReadStableId(RequireMember(seed, "sink_ref"));
  if (!finding_id.ok() || !kind.ok() || !severity.ok() || !subject.ok() ||
      !source.ok() || !sink.ok()) {
    return Status::InvalidArgument("claim seed is malformed");
  }
  slice.claim_seed = ev::ClaimSeed{.finding_id = *finding_id,
                                   .kind = *kind,
                                   .severity = *severity,
                                   .subject_ref = *subject,
                                   .source_ref = *source,
                                   .sink_ref = *sink};

  const JsonNode& flow = RequireMember(root, "flow_slice");
  for (const std::string_view name :
       {"nodes", "edges", "supporting_facts", "contradicting_facts", "unknowns",
        "provenance_refs", "metadata"}) {
    if (flow.Member(name) == nullptr) {
      return Status::InvalidArgument("flow slice is missing '" +
                                     std::string(name) + "'");
    }
  }
  auto nodes = ReadIdArray(flow, "nodes", "node_id");
  auto edges = ReadIdArray(flow, "edges", "edge_id");
  auto supporting = ReadFactRows(flow, "supporting_facts");
  auto contradicting = ReadFactRows(flow, "contradicting_facts");
  auto flow_unknowns = ReadFactRows(flow, "unknowns");
  auto provenance_refs = ReadPlainIdArray(flow, "provenance_refs");
  auto flow_metadata = ReadMetadataView(RequireMember(flow, "metadata"));
  if (!nodes.ok() || !edges.ok() || !supporting.ok() ||
      !contradicting.ok() || !flow_unknowns.ok() || !provenance_refs.ok() ||
      !flow_metadata.ok()) {
    return Status::InvalidArgument("flow slice is malformed");
  }
  slice.flow_nodes = std::move(*nodes);
  slice.flow_edges = std::move(*edges);
  slice.flow_supporting_facts = std::move(*supporting);
  slice.flow_contradicting_facts = std::move(*contradicting);
  slice.flow_unknowns = std::move(*flow_unknowns);
  slice.flow_provenance_refs = std::move(*provenance_refs);
  slice.flow = std::move(*flow_metadata);

  const auto read_set = [&](std::string_view name,
                            FactSetView* out) -> Status {
    auto parsed = ReadFactSet(RequireMember(root, name));
    if (!parsed.ok()) {
      return parsed.status();
    }
    *out = std::move(*parsed);
    return Status::Ok();
  };
  if (Status status = read_set("ranges", &slice.ranges); !status.ok()) {
    return status;
  }
  if (Status status = read_set("capacities", &slice.capacities); !status.ok()) {
    return status;
  }
  if (Status status = read_set("aliases", &slice.aliases); !status.ok()) {
    return status;
  }
  if (Status status = read_set("dominating_checks", &slice.dominating_checks);
      !status.ok()) {
    return status;
  }
  if (Status status = read_set("unknowns", &slice.unknowns); !status.ok()) {
    return status;
  }
  return slice;
}

// ---------------------------------------------------------------------------
// Comparing the toolchain-stable projections
// ---------------------------------------------------------------------------

std::string RenderId(const core::StableId& id) { return core::ToString(id); }

template <typename T, typename Render>
bool SameList(std::string_view name, const std::vector<T>& left,
              const std::vector<T>& right, Render render, std::string* mismatch) {
  if (left.size() != right.size()) {
    *mismatch = std::string(name) + ": " + std::to_string(left.size()) +
                " entries vs " + std::to_string(right.size());
    return false;
  }
  for (std::size_t i = 0; i < left.size(); ++i) {
    if (!(left[i] == right[i])) {
      *mismatch = std::string(name) + "[" + std::to_string(i) + "]: " +
                  render(left[i]) + " vs " + render(right[i]);
      return false;
    }
  }
  return true;
}

template <typename T, typename Render>
bool SameValue(std::string_view name, const T& left, const T& right,
               Render render, std::string* mismatch) {
  if (left == right) {
    return true;
  }
  *mismatch = std::string(name) + ": " + render(left) + " vs " + render(right);
  return false;
}

bool SameFactSet(std::string_view name, const FactSetView& left,
                 const FactSetView& right, std::string* mismatch) {
  if (!SameList(std::string(name) + ".facts", left.facts, right.facts,
                RenderFactRow, mismatch)) {
    return false;
  }
  if (!SameValue(std::string(name) + ".completeness", left.completeness,
                 right.completeness,
                 [](ev::QueryCompleteness value) {
                   return std::string(ev::ToString(value));
                 },
                 mismatch)) {
    return false;
  }
  if (!SameList(std::string(name) + ".truncation_reasons",
                left.truncation_reasons, right.truncation_reasons,
                [](ev::TruncationReason value) {
                  return std::string(ev::ToString(value));
                },
                mismatch)) {
    return false;
  }
  return SameValue(std::string(name) + ".examined_items", left.examined_items,
                   right.examined_items,
                   [](std::size_t value) { return std::to_string(value); },
                   mismatch);
}

// True when two slices agree on every field an independent clean build of the
// toolchain cannot change. The first disagreement is written to `mismatch`.
bool SameStableSlice(const SemanticSlice& left, const SemanticSlice& right,
                     std::string* mismatch) {
  if (!SameValue("claim_seed.finding_id", left.claim_seed.finding_id,
                 right.claim_seed.finding_id, RenderId, mismatch)) {
    return false;
  }
  if (!SameValue("claim_seed.kind", left.claim_seed.kind, right.claim_seed.kind,
                 [](ev::ClaimKind value) {
                   return std::string(ev::ToString(value));
                 },
                 mismatch)) {
    return false;
  }
  if (!SameValue("claim_seed.severity", left.claim_seed.severity,
                 right.claim_seed.severity,
                 [](ev::Severity value) {
                   return std::string(ev::ToString(value));
                 },
                 mismatch)) {
    return false;
  }
  if (!SameValue("claim_seed.subject_ref", left.claim_seed.subject_ref,
                 right.claim_seed.subject_ref, RenderId, mismatch)) {
    return false;
  }
  if (!SameValue("claim_seed.source_ref", left.claim_seed.source_ref,
                 right.claim_seed.source_ref, RenderId, mismatch)) {
    return false;
  }
  if (!SameValue("claim_seed.sink_ref", left.claim_seed.sink_ref,
                 right.claim_seed.sink_ref, RenderId, mismatch)) {
    return false;
  }
  if (!SameList("flow_slice.nodes", left.flow_nodes, right.flow_nodes, RenderId,
                mismatch)) {
    return false;
  }
  if (!SameList("flow_slice.edges", left.flow_edges, right.flow_edges, RenderId,
                mismatch)) {
    return false;
  }
  if (!SameList("flow_slice.supporting_facts", left.flow_supporting_facts,
                right.flow_supporting_facts, RenderFactRow, mismatch)) {
    return false;
  }
  if (!SameList("flow_slice.contradicting_facts",
                left.flow_contradicting_facts, right.flow_contradicting_facts,
                RenderFactRow, mismatch)) {
    return false;
  }
  if (!SameList("flow_slice.unknowns", left.flow_unknowns, right.flow_unknowns,
                RenderFactRow, mismatch)) {
    return false;
  }
  if (!SameList("flow_slice.provenance_refs", left.flow_provenance_refs,
                right.flow_provenance_refs, RenderId, mismatch)) {
    return false;
  }
  if (!SameFactSet("flow_slice.metadata", left.flow, right.flow, mismatch)) {
    return false;
  }
  return SameFactSet("ranges", left.ranges, right.ranges, mismatch) &&
         SameFactSet("capacities", left.capacities, right.capacities,
                     mismatch) &&
         SameFactSet("aliases", left.aliases, right.aliases, mismatch) &&
         SameFactSet("dominating_checks", left.dominating_checks,
                     right.dominating_checks, mismatch) &&
         SameFactSet("unknowns", left.unknowns, right.unknowns, mismatch);
}

TEST(VeritasQueryEvidenceTest, CliEmitsGoldenSliceJsonDeterministically) {
  auto oracle = BuildOracle("evidence_overflow_unsafe");
  ASSERT_TRUE(oracle.ok()) << oracle.status().message();

  // Typed oracle content first.
  ExpectTypedSliceContent(oracle->input, oracle->cpg);
  const std::string expected = ev::ToDiagnosticJson(oracle->input);
  EXPECT_EQ(expected.back(), '\n') << "slice JSON must end with one newline";
  EXPECT_EQ(expected.find("}}\n\n"), std::string::npos)
      << "slice JSON must not add a second trailing newline";

  // The public CLI over the same materialized store.
  const CliResult cli = RunEvidenceOverflow(oracle->output_root);
  ASSERT_EQ(cli.exit_code, 0) << cli.stdout_text;
  EXPECT_EQ(cli.stdout_text, expected)
      << "CLI output drifted from the typed oracle";

  // Determinism against the store's physical order: the same materialized store
  // with its run fact bindings re-inserted backwards must publish the same
  // bytes.
  ReverseFactBindingInsertionOrder(oracle->output_root);
  const CliResult reversed = RunEvidenceOverflow(oracle->output_root);
  ASSERT_EQ(reversed.exit_code, 0) << reversed.stdout_text;
  EXPECT_EQ(reversed.stdout_text, cli.stdout_text)
      << "slice JSON depends on fact-binding insertion order";

  // Determinism across an independent materialization: a second clean store in
  // a different checkout root must produce byte-identical JSON.
  auto second = BuildOracle("evidence_overflow_unsafe");
  ASSERT_TRUE(second.ok()) << second.status().message();
  EXPECT_NE(second->output_root, oracle->output_root);
  const CliResult second_cli = RunEvidenceOverflow(second->output_root);
  ASSERT_EQ(second_cli.exit_code, 0) << second_cli.stdout_text;
  EXPECT_EQ(second_cli.stdout_text, cli.stdout_text)
      << "slice JSON is not byte-stable across stores/checkout roots";

  // The checked-in golden carries the same slice semantics. It is compared
  // SEMANTICALLY, never byte-for-byte: the golden embeds the analysis run id,
  // which derives from the digest of the vendored Soufflé executable, and that
  // executable is not bit-reproducible across clean builds (see the file
  // header). The run-derived members — analysis_run_id, the six query
  // provenance ids, the completion-fact ids, the run bindings, and the
  // provenance graph — are therefore excluded from the comparison, while
  // everything a clean rebuild cannot change (claim seed, CPG flow nodes and
  // edges, GlobalFlow support, fact sets, and every completeness state) is
  // required to match exactly.
  const std::filesystem::path golden =
      std::filesystem::path(VERITAS_GOLDEN_DIR) / "overflow_unsafe.slice.json";
  ASSERT_TRUE(std::filesystem::exists(golden))
      << "missing golden; regenerate with the CLI: " << golden;

  auto cli_slice = ParseSemanticSlice(cli.stdout_text);
  ASSERT_TRUE(cli_slice.ok()) << cli_slice.status().message();
  auto golden_slice = ParseSemanticSlice(ReadFile(golden));
  ASSERT_TRUE(golden_slice.ok()) << golden_slice.status().message();

  std::string mismatch;
  EXPECT_TRUE(SameStableSlice(*cli_slice, *golden_slice, &mismatch))
      << "CLI output no longer matches " << golden << ": " << mismatch;

  // Non-vacuity guard: the comparison must actually inspect the fields it
  // claims to compare, so a single mutated semantic field has to be caught.
  // The baseline is the CLI's own slice, so this guard is independent of the
  // golden's state.
  SemanticSlice mutated = *cli_slice;
  mutated.claim_seed.sink_ref = mutated.claim_seed.finding_id;
  std::string mutation_mismatch;
  EXPECT_FALSE(SameStableSlice(mutated, *cli_slice, &mutation_mismatch))
      << "the semantic comparison does not inspect the claim seed";
  EXPECT_NE(mutation_mismatch.find("claim_seed.sink_ref"), std::string::npos)
      << mutation_mismatch;

  SemanticSlice truncated = *cli_slice;
  truncated.flow_supporting_facts.clear();
  std::string truncation_mismatch;
  EXPECT_FALSE(SameStableSlice(truncated, *cli_slice, &truncation_mismatch))
      << "the semantic comparison does not inspect the flow support";
  EXPECT_NE(truncation_mismatch.find("flow_slice.supporting_facts"),
            std::string::npos)
      << truncation_mismatch;

  // Cells, not just fact ids, are inside the comparison: an equal-length
  // mutation of one cell of one flow fact must be caught. M10C adds cells to
  // these rows, so a cell dropped from the comparison has to fail here.
  ASSERT_FALSE(cli_slice->flow_supporting_facts.empty());
  ASSERT_FALSE(cli_slice->flow_supporting_facts[0].cells.empty());
  SemanticSlice cell_mutated = *cli_slice;
  cell_mutated.flow_supporting_facts[0].cells[0] = "s:mutated-cell";
  std::string cell_mismatch;
  EXPECT_FALSE(SameStableSlice(cell_mutated, *cli_slice, &cell_mismatch))
      << "the semantic comparison does not inspect fact cells";
  EXPECT_NE(cell_mismatch.find("flow_slice.supporting_facts"),
            std::string::npos)
      << cell_mismatch;

  // The three flow-slice fields M10C materializes must already be inside the
  // comparison even while they are empty in the demo slice, so a future field
  // removal (or a dropped element) fails the golden rather than passing
  // vacuously. Each mutation injects one semantic element.
  FactRowView injected;
  injected.fact_id = cli_slice->claim_seed.finding_id;
  injected.relation = "GlobalFlow";
  injected.cells = {"s:injected"};

  SemanticSlice with_contradiction = *cli_slice;
  with_contradiction.flow_contradicting_facts.push_back(injected);
  std::string contradiction_mismatch;
  EXPECT_FALSE(
      SameStableSlice(with_contradiction, *cli_slice, &contradiction_mismatch))
      << "the semantic comparison does not inspect flow contradicting facts";
  EXPECT_NE(contradiction_mismatch.find("flow_slice.contradicting_facts"),
            std::string::npos)
      << contradiction_mismatch;

  SemanticSlice with_unknown = *cli_slice;
  with_unknown.flow_unknowns.push_back(injected);
  std::string unknown_mismatch;
  EXPECT_FALSE(SameStableSlice(with_unknown, *cli_slice, &unknown_mismatch))
      << "the semantic comparison does not inspect flow unknowns";
  EXPECT_NE(unknown_mismatch.find("flow_slice.unknowns"), std::string::npos)
      << unknown_mismatch;

  ASSERT_FALSE(cli_slice->flow_provenance_refs.empty());
  SemanticSlice with_ref = *cli_slice;
  with_ref.flow_provenance_refs.push_back(
      cli_slice->flow_provenance_refs.front());
  std::string ref_mismatch;
  EXPECT_FALSE(SameStableSlice(with_ref, *cli_slice, &ref_mismatch))
      << "the semantic comparison does not inspect flow provenance refs";
  EXPECT_NE(ref_mismatch.find("flow_slice.provenance_refs"), std::string::npos)
      << ref_mismatch;
}

TEST(VeritasQueryEvidenceTest, RejectsInvalidOptionSurface) {
  const std::string db = "/nonexistent/veritas-query-evidence";
  const std::vector<std::pair<std::string, std::vector<std::string>>> cases = {
      {"unknown format",
       {"evidence", "overflow", "--sink", "memcpy", "--format", "sarif", "--db",
        db}},
      {"missing format", {"evidence", "overflow", "--sink", "memcpy", "--db", db}},
      {"missing sink", {"evidence", "overflow", "--format", "json", "--db", db}},
      {"unsupported sink",
       {"evidence", "overflow", "--sink", "strcpy", "--format", "json", "--db",
        db}},
      {"missing db",
       {"evidence", "overflow", "--sink", "memcpy", "--format", "json"}},
      {"duplicate option",
       {"evidence", "overflow", "--sink", "memcpy", "--sink", "memcpy",
        "--format", "json", "--db", db}},
      {"unknown option",
       {"evidence", "overflow", "--sink", "memcpy", "--format", "json", "--db",
        db, "--bogus", "1"}},
      {"missing option value",
       {"evidence", "overflow", "--sink", "memcpy", "--format", "json", "--db"}},
      {"zero budget",
       {"evidence", "overflow", "--sink", "memcpy", "--format", "json", "--db",
        db, "--max-nodes", "0"}},
      {"overflowing integer",
       {"evidence", "overflow", "--sink", "memcpy", "--format", "json", "--db",
        db, "--max-nodes", "99999999999999999999999999"}},
      {"non-numeric budget",
       {"evidence", "overflow", "--sink", "memcpy", "--format", "json", "--db",
        db, "--max-depth", "eight"}},
  };

  for (const auto& [name, arguments] : cases) {
    const CliResult result = RunVeritasQuery(arguments);
    EXPECT_NE(result.exit_code, 0) << name << ": " << result.stdout_text;
    EXPECT_EQ(result.stdout_text.find('{'), std::string::npos)
        << name << " produced JSON instead of a stable error";
    EXPECT_NE(result.stdout_text.find("veritas-query: "), std::string::npos)
        << name << ": " << result.stdout_text;
  }
}

TEST(VeritasQueryEvidenceTest, MissingStoreFailsBeforeEmittingJson) {
  const CliResult result = RunEvidenceOverflow(
      std::filesystem::path("/nonexistent/veritas-query-evidence"));
  EXPECT_NE(result.exit_code, 0);
  EXPECT_EQ(result.stdout_text.find('{'), std::string::npos);
  EXPECT_NE(result.stdout_text.find("veritas-query: "), std::string::npos)
      << result.stdout_text;
}

}  // namespace
}  // namespace veritas::testing
