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

#include "veritas/facts/AnalysisFactBus.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>

#include "veritas/facts/AnalysisFact.h"
#include "veritas/facts/AnalysisRun.h"
#include "veritas/facts/RowArena.h"
#include "veritas/facts/Witness.h"
#include "veritas/wpa/WpaOrchestrator.h"
#include "veritas/wpa/WpaRunRepository.h"

namespace veritas::facts {
namespace {

namespace sem = analysis::semantic;

// A component's payload is an arena, so a fixture that used to assign a vector
// appends each element instead. A fixture whose append cannot encode its own
// row is a broken fixture, not a case under test.
void AddFact(wpa::WpaComponentResult *result, const AnalysisFact &fact) {
  auto appended = result->facts.AppendFact(fact);
  if (!appended.ok()) {
    std::abort();
  }
}

void AddWitness(wpa::WpaComponentResult *result, const WitnessEdge &edge) {
  auto appended = result->witnesses.AppendWitness(edge);
  if (!appended.ok()) {
    std::abort();
  }
}

// The first witness a component's arena holds, decoded, so a fixture that has
// one edge asks the range for it rather than indexing a vector.
WitnessEdge FirstWitness(const wpa::WpaComponentResult &result) {
  return *WitnessRange(&result.witnesses).begin();
}

// The batch's first witness, decoded, for the cases that assemble a batch with
// exactly one edge.
WitnessEdge FirstBatchWitness(const AnalysisFactBatch &batch) {
  return *batch.witnesses().begin();
}

static_assert(std::is_same_v<
              decltype(&AnalysisFactBus::Publish),
              Status (AnalysisFactBus::*)(const AnalysisFactBatch &) const>);

constexpr std::string_view kDirect = "wpa.reachability.direct.v2";
constexpr std::string_view kFlowParameter = "wpa.flow.global.parameter.v2";

core::StableId FunctionId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kFunctionVariant,
                            std::as_bytes(std::span(name.data(), name.size())));
}

core::StableId CallSiteId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kCallSite,
                            std::as_bytes(std::span(name.data(), name.size())));
}

core::StableId ValueId(std::string_view name) {
  return core::MakeStableId(core::IdKind::kValueRef,
                            std::as_bytes(std::span(name.data(), name.size())));
}

SemanticRow Reachable(std::string_view from, std::string_view to) {
  return SemanticRow{
      RelationId::kReachableCall,
      {FunctionId(from), FunctionId(to), sem::EpistemicState::kMay}};
}

SemanticRow DirectCall(std::string_view from, std::string_view to) {
  return SemanticRow{RelationId::kDirectCall,
                     {CallSiteId(std::string(from) + "->" + std::string(to)),
                      FunctionId(from), FunctionId(to),
                      sem::DispatchKind::kDirect, sem::EpistemicState::kMay}};
}

WitnessEdge Edge(const SemanticRow &result, std::string_view rule,
                 const SemanticRow &input, std::uint32_t ordinal) {
  return WitnessEdge{.result = SemanticKey{result},
                     .rule_id = std::string(rule),
                     .input = SemanticKey{input},
                     .input_ordinal = ordinal};
}

// A parameter flow binds `actual` to `formal` at one call site.
SemanticRow ParameterFlow(std::string_view site, std::string_view actual,
                          std::string_view formal) {
  return SemanticRow{RelationId::kParameterFlow,
                     {CallSiteId(site), ValueId(actual), ValueId(formal),
                      sem::EpistemicState::kMust}};
}

// The global flow a parameter flow is promoted to. The call site is projected
// away, so two call sites sharing an actual and a formal promote to one row.
SemanticRow GlobalFlow(std::string_view from, std::string_view to) {
  return SemanticRow{RelationId::kGlobalFlow,
                     {ValueId(from), ValueId(to), sem::EpistemicState::kMust}};
}

AnalysisRunManifest TestRun() {
  AnalysisRunDescriptor descriptor;
  descriptor.revision_id = core::MakeStableId(
      core::IdKind::kRevision, std::as_bytes(std::span("rev", 3)));
  descriptor.build_variant_id = core::MakeStableId(
      core::IdKind::kBuildVariant, std::as_bytes(std::span("bv", 2)));
  descriptor.summary_schema_version = "summary.v2";
  descriptor.relation_schema_version = "relations.v2";
  descriptor.rule_bundle_version = "rules.v2";
  descriptor.model_bundle_version = "models.v1";
  descriptor.svf_configuration_hash = std::string(64, 'a');
  descriptor.wpa_configuration_hash = std::string(64, 'b');
  descriptor.engine = EngineIdentity::kSouffle;
  descriptor.engine_toolchain_identity = "test-toolchain";
  return std::move(MakeAnalysisRun(descriptor)).value();
}

// --- Order preservation -----------------------------------------------------
//
// The assembled batch's order is load-bearing: `DeriveBatchId` hashes it, and
// the row order of every published table derives from it. Since the batch id is
// a hash, an order that is only approximately right is a *silent* failure --
// every fact identity in every store moves and nothing reports an error.
//
// Everything below reproduces the ordering `MakeAnalysisFactBatch` used before
// the ordering keys were packed, independently, from the run's own rows: the
// encoded semantic keys are rebuilt here and compared with the string
// comparator that used to order them. Comparing against a golden captured from
// the implementation under test would agree with a defect in it, so no such
// golden is used.

// Renders the four fields the witness comparator orders by: the result key, the
// rule id, the input key, and the input ordinal, each length-prefixed so the
// rendering is injective -- two witnesses the comparator cannot separate render
// identically, and two it can always render differently. That makes comparing
// rendered sequences exactly as strong as comparing the edges themselves, while
// never depending on which of two comparator-equal elements a sort happened to
// place first. Injectivity is all that is needed; the rendering is not
// order-faithful, and nothing here compares rendered bytes for order.
std::string RenderWitnessSortKey(const WitnessEdge &edge) {
  std::string result_key;
  AppendSemanticKey(&result_key, edge.result.row);
  std::string input_key;
  AppendSemanticKey(&input_key, edge.input.row);
  std::string out;
  out.reserve(result_key.size() + input_key.size() + edge.rule_id.size() + 32);
  const auto append_field = [&out](std::string_view field) {
    out.append(std::to_string(field.size()));
    out.push_back(':');
    out.append(field);
  };
  append_field(result_key);
  append_field(edge.rule_id);
  append_field(input_key);
  append_field(std::to_string(edge.input_ordinal));
  return out;
}

std::vector<std::string>
RenderWitnessSortKeys(const std::vector<WitnessEdge> &edges) {
  std::vector<std::string> rendered;
  rendered.reserve(edges.size());
  for (const auto &edge : edges) {
    rendered.push_back(RenderWitnessSortKey(edge));
  }
  return rendered;
}

std::vector<std::string> RenderFactKeys(const std::vector<AnalysisFact> &facts) {
  std::vector<std::string> rendered;
  rendered.reserve(facts.size());
  for (const auto &fact : facts) {
    rendered.push_back(EncodeSemanticKey(fact.row));
  }
  return rendered;
}

// The decoded rows of a range. The batch holds its payload in arenas, so a
// caller that wants to index a row rather than stream it asks for the rows it
// wants; nothing here retains a range past the call.
std::vector<AnalysisFact> Materialize(const AnalysisFactRange &facts) {
  std::vector<AnalysisFact> rows;
  rows.reserve(facts.size());
  for (const AnalysisFact &fact : facts) {
    rows.push_back(fact);
  }
  return rows;
}

std::vector<WitnessEdge> Materialize(const WitnessRange &witnesses) {
  std::vector<WitnessEdge> edges;
  edges.reserve(witnesses.size());
  for (const WitnessEdge &edge : witnesses) {
    edges.push_back(edge);
  }
  return edges;
}

// The pre-change witness order, reimplemented here from the run's rows: encode
// each edge's endpoint rows, compare `std::tie(result_key, rule_id, input_key,
// input_ordinal)` as strings, and collapse exact repeats.
//
// Two things this deliberately does not reproduce, both because the fixture
// below makes them no-ops: the ownership pass that drops a witness whose result
// fact a second component also derived (the fixture has no doubly derived
// fact), and the fact sort (covered by `CanonicalFactOrderByStringKeys`). What
// it does reproduce is the part under test -- the comparator and the
// `std::unique` that runs on its output.
std::vector<WitnessEdge>
CanonicalWitnessOrderByStringKeys(const wpa::WpaRunResult &run) {
  struct KeyedEdge {
    std::string result_key;
    std::string rule_id;
    std::string input_key;
    std::uint32_t input_ordinal = 0;
    WitnessEdge edge;
  };
  std::vector<KeyedEdge> keyed;
  for (const auto &completion : run.completed_components) {
    for (const auto &edge : WitnessRange(&completion.result.witnesses)) {
      keyed.push_back(KeyedEdge{
          .result_key = EncodeSemanticKey(edge.result.row),
          .rule_id = edge.rule_id,
          .input_key = EncodeSemanticKey(edge.input.row),
          .input_ordinal = edge.input_ordinal,
          .edge = edge,
      });
    }
  }
  // The ordering decision, spelled out: the same four fields in the same order,
  // compared through `std::char_traits<char>`, which is what the packed ranks
  // have to reproduce.
  std::ranges::sort(keyed, [](const KeyedEdge &left, const KeyedEdge &right) {
    return std::tie(left.result_key, left.rule_id, left.input_key,
                    left.input_ordinal) <
           std::tie(right.result_key, right.rule_id, right.input_key,
                    right.input_ordinal);
  });
  std::vector<WitnessEdge> ordered;
  ordered.reserve(keyed.size());
  for (auto &entry : keyed) {
    ordered.push_back(std::move(entry.edge));
  }
  // `unique` removes consecutive `operator==` repeats, so the outcome is
  // independent of the internal order of a block of comparator-equal elements
  // only when every such block is a block of wholly identical edges. The
  // fixtures below satisfy that; a fixture that did not would make this helper
  // as order-sensitive as the sort it is checking.
  ordered.erase(std::ranges::unique(ordered).begin(), ordered.end());
  return ordered;
}

// The pre-change fact order: sort by the encoded semantic key of the row.
std::vector<AnalysisFact>
CanonicalFactOrderByStringKeys(const wpa::WpaRunResult &run) {
  std::vector<AnalysisFact> facts;
  for (const auto &completion : run.completed_components) {
    for (const auto &fact : AnalysisFactRange(&completion.result.facts)) {
      facts.push_back(fact);
    }
  }
  std::ranges::sort(facts,
                    [](const AnalysisFact &left, const AnalysisFact &right) {
                      return EncodeSemanticKey(left.row) <
                             EncodeSemanticKey(right.row);
                    });
  return facts;
}

// A run whose witness edges are arranged so that every tie the comparator can
// break is present, and so that byte order is not insertion order:
//
//   * two edges agreeing on the result and differing at `rule_id`
//     ("wpa.rule.zulu" is inserted before "wpa.rule.alpha");
//   * two agreeing on the result and the rule and differing at `input_key`
//     (the call sites `tie->a` and `tie->c`);
//   * two agreeing on all three and differing at `input_ordinal`.
//
// Every result is a published fact and every input is a declared root, so the
// ownership pass drops nothing and the whole surviving order is compared. True
// to the ownership contract, no fact is derived by two components.
wpa::WpaRunResult TiebreakerRun() {
  const SemanticRow result_a = Reachable("tie-alpha", "tie-beta");
  const SemanticRow result_b = Reachable("tie-gamma", "tie-delta");
  const SemanticRow result_c = Reachable("tie-epsilon", "tie-zeta");
  const SemanticRow root_a = DirectCall("tie", "a");
  const SemanticRow root_b = DirectCall("tie", "b");
  const SemanticRow root_c = DirectCall("tie", "c");
  const auto fact = [](const SemanticRow &row) { return MakeFact(row).value(); };

  const auto component = [&](std::string_view scc, std::vector<SemanticRow> rows,
                             std::vector<WitnessEdge> edges) {
    wpa::WpaComponentCompletion completion;
    completion.key =
        wpa::WpaComponentKey{FunctionId(scc), wpa::WpaComponentKind::kFlow};
    completion.result.scc_id = completion.key.scc_id;
    completion.result.component = completion.key.component;
    completion.result.logical_input_hash = "logical";
    completion.result.fixpoint_hash = "fixpoint";
    completion.result.external_hash = "external";
    for (auto &row : rows) {
      AddFact(&completion.result, fact(row));
    }
    for (const WitnessEdge &edge : edges) {
      AddWitness(&completion.result, edge);
    }
    return completion;
  };

  const auto completion_a =
      component("scc:tie-a", {result_a},
                {Edge(result_a, "wpa.rule.zulu", root_a, 0),
                 Edge(result_a, "wpa.rule.alpha", root_a, 0),
                 Edge(result_a, "wpa.rule.alpha", root_c, 0),
                 Edge(result_a, "wpa.rule.alpha", root_c, 1)});
  const auto completion_b =
      component("scc:tie-b", {result_b, result_c},
                {Edge(result_b, "wpa.rule.alpha", root_b, 0),
                 Edge(result_c, "wpa.rule.gamma", root_c, 2)});

  wpa::WpaRunResult run;
  run.run = TestRun();
  run.expected_components = {completion_a.key, completion_b.key};
  run.completed_components = {completion_a, completion_b};
  run.rooted_input_fact_ids = {fact(root_a).fact_id, fact(root_b).fact_id,
                               fact(root_c).fact_id};
  run.rooted_input_facts = {RootedInputFact{.fact = fact(root_a)},
                            RootedInputFact{.fact = fact(root_b)},
                            RootedInputFact{.fact = fact(root_c)}};
  return run;
}

std::filesystem::path TempDbPath() {
  std::string tmpl =
      (std::filesystem::temp_directory_path() / "veritas-factbus-XXXXXX")
          .string();
  char *made = ::mkdtemp(tmpl.data());
  return std::filesystem::path(made);
}

// A valid batch: one component, one rooted input (the direct call), one derived
// fact (the reachable call) proved by one witness edge.
AnalysisFactBatch SuccessfulBatch() {
  AnalysisFactBatch batch;
  batch.run = TestRun();

  wpa::WpaComponentKey key{FunctionId("scc"),
                           wpa::WpaComponentKind::kReachability};
  batch.expected_components = {key};

  wpa::WpaComponentCompletion completion;
  completion.key = key;
  completion.result_object_key = "result-object";
  completion.result.scc_id = key.scc_id;
  completion.result.component = key.component;
  completion.result.logical_input_hash = "logical";
  completion.result.fixpoint_hash = "fixpoint";
  completion.result.external_hash = "external";
  batch.completed_components = {completion};

  const auto root = MakeFact(DirectCall("f", "g")).value();
  const auto derived = MakeFact(Reachable("f", "g")).value();
  batch.rooted_input_fact_ids = {root.fact_id};
  batch.SetFacts({derived});
  batch.SetWitnesses(
      {Edge(Reachable("f", "g"), kDirect, DirectCall("f", "g"), 0)});
  batch.batch_id = DeriveBatchId(batch);
  return batch;
}

wpa::WpaRunResult DuplicateProofRun() {
  const SemanticRow shared_flow = GlobalFlow("r", "p");
  const SemanticRow root_a = ParameterFlow("cs:a", "r", "p");
  const SemanticRow root_b = ParameterFlow("cs:b", "r", "p");
  const auto root_fact_a = MakeFact(root_a).value();
  const auto root_fact_b = MakeFact(root_b).value();

  auto component = [](std::string_view scc, const SemanticRow &root,
                      const SemanticRow &flow) {
    wpa::WpaComponentCompletion completion;
    completion.key =
        wpa::WpaComponentKey{FunctionId(scc), wpa::WpaComponentKind::kFlow};
    completion.result.scc_id = completion.key.scc_id;
    completion.result.component = completion.key.component;
    completion.result.logical_input_hash = "logical";
    completion.result.fixpoint_hash = "fixpoint";
    completion.result.external_hash = "external";
    AddFact(&completion.result, MakeFact(flow).value());
    AddWitness(&completion.result, Edge(flow, kFlowParameter, root, 0));
    return completion;
  };
  const auto completion_a = component("scc:a", root_a, shared_flow);
  const auto completion_b = component("scc:b", root_b, shared_flow);

  wpa::WpaRunResult run;
  run.run = TestRun();
  run.expected_components = {completion_a.key, completion_b.key};
  run.completed_components = {completion_a, completion_b};
  run.rooted_input_fact_ids = {root_fact_a.fact_id, root_fact_b.fact_id};
  run.rooted_input_facts = {RootedInputFact{.fact = root_fact_a},
                            RootedInputFact{.fact = root_fact_b}};
  return run;
}

class RecordingSink : public AnalysisFactSink {
public:
  Status Publish(const AnalysisFactBatch &batch) override {
    batches_.push_back(batch);
    ++counts_[core::ToString(batch.batch_id)];
    return Status::Ok();
  }

  const std::vector<AnalysisFactBatch> &batches() const { return batches_; }
  std::size_t logical_publication_count(const core::StableId &batch_id) const {
    const auto it = counts_.find(core::ToString(batch_id));
    return it == counts_.end() ? 0 : it->second;
  }

private:
  std::vector<AnalysisFactBatch> batches_;
  std::map<std::string, std::size_t> counts_;
};

class FailOnceSink : public AnalysisFactSink {
public:
  Status Publish(const AnalysisFactBatch &batch) override {
    if (!failed_once_) {
      failed_once_ = true;
      return Status::Internal("injected fan-out failure");
    }
    ++counts_[core::ToString(batch.batch_id)];
    return Status::Ok();
  }

  std::size_t logical_publication_count(const core::StableId &batch_id) const {
    const auto it = counts_.find(core::ToString(batch_id));
    return it == counts_.end() ? 0 : it->second;
  }

private:
  bool failed_once_ = false;
  std::map<std::string, std::size_t> counts_;
};

TEST(AnalysisFactBusTest, DeliversOneValidatedImmutableBatch) {
  const auto db = TempDbPath();
  auto repo = wpa::WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok()) << repo.status().message();

  RecordingSink sink;
  AnalysisFactBus bus(*repo);
  bus.AddSink("recording", sink);

  const auto batch = SuccessfulBatch();
  ASSERT_TRUE(bus.Publish(batch).ok());
  ASSERT_EQ(sink.batches().size(), 1u);
  EXPECT_EQ(sink.batches()[0].run.run_id, batch.run.run_id);

  std::filesystem::remove_all(db);
}

// Two sibling callers each bind the same callee-owned value to the same formal,
// at their own call sites. `GlobalFlow` drops the call site, so both components
// derive the identical fact from their own `ParameterFlow` row and the
// flattened run carries it twice. A run's provenance records one proof per
// fact, so the assembled batch keeps one fact and one whole derivation -- not a
// mixture of the two proofs, which would bind a single-input rule at two
// ordinals.
TEST(AnalysisFactBusTest, CoalescesAFactProvenByTwoComponents) {
  const auto db = TempDbPath();
  auto repo = wpa::WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok()) << repo.status().message();
  AnalysisFactBus bus(*repo);

  wpa::WpaRunResult run = DuplicateProofRun();
  const auto &completion_a = run.completed_components[0];
  const auto &completion_b = run.completed_components[1];

  // Ownership goes to the first component in canonical key order, so the
  // surviving proof is whichever component's key sorts first -- and it must be
  // that component's whole derivation, root and all.
  const bool first_is_a = completion_a.key < completion_b.key;
  const SemanticRow surviving_root =
      first_is_a ? FirstWitness(completion_a.result).input.row
                 : FirstWitness(completion_b.result).input.row;

  const AnalysisFactBatch batch = MakeAnalysisFactBatch(run);
  ASSERT_EQ(batch.fact_count(), 1u);
  ASSERT_EQ(batch.witness_count(), 1u);
  EXPECT_EQ(FirstBatchWitness(batch).input.row, surviving_root);
  EXPECT_TRUE(bus.Publish(batch).ok());

  // Ownership follows the sorted component keys, not the order the orchestrator
  // happened to complete them in, so reversing the completion order changes
  // nothing at all -- not even the batch identity.
  wpa::WpaRunResult reversed = run;
  reversed.completed_components = {completion_b, completion_a};
  const AnalysisFactBatch reversed_batch = MakeAnalysisFactBatch(reversed);
  ASSERT_EQ(reversed_batch.witness_count(), 1u);
  EXPECT_EQ(FirstBatchWitness(reversed_batch).input.row, surviving_root);
  EXPECT_EQ(reversed_batch.batch_id, batch.batch_id);
  EXPECT_TRUE(bus.Publish(reversed_batch).ok());

  std::filesystem::remove_all(db);
}

// The ordering keys used to be the encoded semantic key of each endpoint, held
// as a `std::string` and compared lexicographically. They are now dense ranks
// over the distinct keys, and the claim that makes the change safe is that the
// substitution is an order-isomorphism: equal keys take equal ranks, and
// ascending rank is ascending byte order. This compares the assembled order
// against an independent implementation of the string comparator, so the claim
// is checked rather than assumed.
TEST(AnalysisFactBusTest, PackedRanksPreserveStringOrderIncludingTies) {
  auto run = TiebreakerRun();
  const std::vector<AnalysisFact> canonical_facts =
      CanonicalFactOrderByStringKeys(run);
  const std::vector<WitnessEdge> canonical_witnesses =
      CanonicalWitnessOrderByStringKeys(run);

  // Guard against a vacuous test. The fixture inserts the rule-zulu edge before
  // the rule-alpha one and the comparator orders alpha first, so the fixture's
  // insertion order is not the canonical order. Were it already sorted, an
  // assembly that never sorted at all would satisfy the comparison below. The
  // rendering helpers are not used here: they are length-prefixed for
  // injectivity, which is what the comparison needs, and that prefixing is not
  // order-faithful.
  const SemanticRow tie_result = Reachable("tie-alpha", "tie-beta");
  const std::string tie_result_key = EncodeSemanticKey(tie_result);
  const std::string tie_input_key = EncodeSemanticKey(DirectCall("tie", "a"));
  EXPECT_LT(std::make_tuple(tie_result_key, std::string("wpa.rule.alpha"),
                            tie_input_key, std::uint32_t{0}),
            std::make_tuple(tie_result_key, std::string("wpa.rule.zulu"),
                            tie_input_key, std::uint32_t{0}));

  const AnalysisFactBatch packed = MakeAnalysisFactBatch(std::move(run));

  EXPECT_EQ(RenderFactKeys(Materialize(packed.facts())),
            RenderFactKeys(canonical_facts));
  EXPECT_EQ(RenderWitnessSortKeys(Materialize(packed.witnesses())),
            RenderWitnessSortKeys(canonical_witnesses));

  // The same requirement restated in the domain where it bites: order is what
  // `DeriveBatchId` hashes, so a batch ordered differently is a batch
  // identified differently -- which is the silent failure this test exists to
  // prevent.
  AnalysisFactBatch relaid = packed;
  relaid.SetFacts(canonical_facts);
  relaid.SetWitnesses(canonical_witnesses);
  EXPECT_EQ(DeriveBatchId(relaid), packed.batch_id);
}

// `std::unique` runs on the freshly ordered vectors, so it must remove the same
// elements it removed before. The witness path can hold exact repeats -- two
// firings agreeing on the result, the rule, the input and the ordinal are one
// proof, since `derivation_key` is not part of the ordering -- and the fact
// path cannot, because the ownership pass keys on `fact_id` and a repeat never
// reaches the sort. This checks the witness case survives the change.
TEST(AnalysisFactBusTest, PackedRanksKeepTheSameUniqueBoundary) {
  auto run = TiebreakerRun();
  const std::vector<WitnessEdge> canonical_witnesses =
      CanonicalWitnessOrderByStringKeys(run);

  // An exact repeat of an edge already present, with the same rows, rule, and
  // ordinal -- so the two are equal under both the comparator and `operator==`,
  // and the collapse does not depend on which order a sort placed them in.
  AddWitness(&run.completed_components[0].result,
             Edge(Reachable("tie-alpha", "tie-beta"), "wpa.rule.alpha",
                  DirectCall("tie", "a"), 0));

  const AnalysisFactBatch packed = MakeAnalysisFactBatch(std::move(run));

  EXPECT_EQ(packed.fact_count(), 3u);
  EXPECT_EQ(packed.witness_count(), canonical_witnesses.size());
  EXPECT_EQ(RenderWitnessSortKeys(Materialize(packed.witnesses())),
            RenderWitnessSortKeys(canonical_witnesses));

  const auto db = TempDbPath();
  auto repo = wpa::WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok()) << repo.status().message();
  AnalysisFactBus bus(*repo);
  EXPECT_TRUE(bus.Publish(packed).ok());
  std::filesystem::remove_all(db);
}

// The batch holds its canonical facts and witnesses in arenas rather than rich
// vectors, and the property that has to survive the change is the batch id: it
// is a hash over the canonical rows, so an assembly that reproduces the rows in
// the same order reproduces the id, and one that does not moves every fact
// identity in every store without reporting an error. The second half of the
// case pins the other half of the contract -- assembly consumes the component's
// payload into the batch *without* emptying the component, which round 4
// section 5 requires (nothing is released, evicted or reloaded).
TEST(AnalysisFactBusTest, ArenaBatchKeepsTheCanonicalBatchIdAndOwnership) {
  auto run = DuplicateProofRun();
  const auto expected = MakeAnalysisFactBatch(run);
  auto consumed = MakeAnalysisFactBatch(std::move(run));

  EXPECT_EQ(consumed.batch_id, expected.batch_id);
  ASSERT_EQ(consumed.fact_count(), expected.fact_count());
  ASSERT_EQ(consumed.witness_count(), expected.witness_count());
  // One fact and one witness, not two of each: `DuplicateProofRun` proves the
  // same derived fact from two components and the ownership rule keeps one
  // whole proof, which is the "ownership" half of this case's name.
  ASSERT_EQ(consumed.fact_count(), 1u);
  ASSERT_EQ(consumed.witness_count(), 1u);
  const AnalysisFactRange expected_facts = expected.facts();
  auto expected_fact = expected_facts.begin();
  for (const AnalysisFact &fact : consumed.facts()) {
    ASSERT_NE(expected_fact, expected_facts.end());
    EXPECT_EQ(fact.fact_id, (*expected_fact).fact_id);
    ++expected_fact;
  }
  EXPECT_EQ(expected_fact, expected_facts.end());

  for (const auto &completion : consumed.completed_components) {
    // Retained and whole, not emptied by assembly: nothing is released, so the
    // component's own arena still holds every row it held before.
    EXPECT_FALSE(AnalysisFactRange(&completion.result.facts).empty());
    EXPECT_FALSE(WitnessRange(&completion.result.witnesses).empty());
    EXPECT_FALSE(completion.result.logical_input_hash.empty());
  }
}

TEST(AnalysisFactBusTest, ConsumesComponentPayloadIntoCanonicalBatchVectors) {
  auto run = DuplicateProofRun();
  // Every row each component's arena holds, keyed by component and captured
  // before assembly. Retention is asserted against these and not against a
  // length: an assembly that dropped rows and left the rest in place would
  // leave every arena as long as it was and pass a size check.
  std::map<wpa::WpaComponentKey, std::vector<AnalysisFact>> facts_before;
  std::map<wpa::WpaComponentKey, std::vector<WitnessEdge>> edges_before;
  for (const auto &completion : run.completed_components) {
    facts_before[completion.key] =
        Materialize(AnalysisFactRange(&completion.result.facts));
    edges_before[completion.key] =
        Materialize(WitnessRange(&completion.result.witnesses));
  }

  const auto expected = MakeAnalysisFactBatch(run);
  auto consumed = MakeAnalysisFactBatch(std::move(run));

  EXPECT_EQ(consumed.batch_id, expected.batch_id);
  EXPECT_EQ(Materialize(consumed.facts()), Materialize(expected.facts()));
  EXPECT_EQ(Materialize(consumed.witnesses()),
            Materialize(expected.witnesses()));
  ASSERT_FALSE(consumed.completed_components.empty());
  for (const auto &completion : consumed.completed_components) {
    // The payload stays whole. Assembly reads the arena and the batch appends
    // its own copy of the rows it selected, so the component's own rows are
    // still there, unchanged -- nothing is released, which is the round's
    // contract. The diagnostics are the exception: they are not payload and the
    // assembly still moves them out.
    const auto facts_it = facts_before.find(completion.key);
    ASSERT_NE(facts_it, facts_before.end());
    EXPECT_EQ(Materialize(AnalysisFactRange(&completion.result.facts)),
              facts_it->second);
    EXPECT_EQ(Materialize(WitnessRange(&completion.result.witnesses)),
              edges_before[completion.key]);
    EXPECT_TRUE(completion.result.diagnostics.empty());
    EXPECT_FALSE(completion.result.logical_input_hash.empty());
    EXPECT_FALSE(completion.result.fixpoint_hash.empty());
    EXPECT_FALSE(completion.result.external_hash.empty());
  }
}

TEST(AnalysisFactBusTest, RejectsIncompleteOrMixedRunBatch) {
  const auto db = TempDbPath();
  auto repo = wpa::WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok()) << repo.status().message();
  AnalysisFactBus bus(*repo);

  // Missing a completed component for the expected set.
  auto incomplete = SuccessfulBatch();
  incomplete.completed_components.clear();
  EXPECT_FALSE(bus.Publish(incomplete).ok());

  // A fact whose identity does not match its semantic row (mixed identity).
  // The payload is an append-only arena, so the one row is replaced by
  // rebuilding the facts from the rows the batch already holds.
  auto mixed = SuccessfulBatch();
  std::vector<AnalysisFact> mixed_rows = Materialize(mixed.facts());
  mixed_rows[0].fact_id = FunctionId("not-the-fact");
  mixed.SetFacts(mixed_rows);
  EXPECT_FALSE(bus.Publish(mixed).ok());

  std::filesystem::remove_all(db);
}

TEST(AnalysisFactBusTest, RejectsFactWithoutClosedWitness) {
  const auto db = TempDbPath();
  auto repo = wpa::WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok()) << repo.status().message();
  AnalysisFactBus bus(*repo);

  auto orphan = SuccessfulBatch();
  orphan.SetWitnesses({});
  EXPECT_EQ(bus.Publish(std::move(orphan)).code(),
            StatusCode::kFailedPrecondition);

  std::filesystem::remove_all(db);
}

TEST(AnalysisFactBusTest, RejectsClosedSubsetWithMissingComponent) {
  const auto db = TempDbPath();
  auto repo = wpa::WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok()) << repo.status().message();
  AnalysisFactBus bus(*repo);

  auto batch = SuccessfulBatch();
  batch.completed_components.pop_back();
  EXPECT_EQ(bus.Publish(std::move(batch)).code(),
            StatusCode::kFailedPrecondition);

  std::filesystem::remove_all(db);
}

TEST(AnalysisFactBusTest, RejectsWitnessLeafOutsideRootSet) {
  const auto db = TempDbPath();
  auto repo = wpa::WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok()) << repo.status().message();
  AnalysisFactBus bus(*repo);

  auto undeclared = SuccessfulBatch();
  undeclared.rooted_input_fact_ids.clear();
  // The witness still cites the direct call, which is now neither a published
  // fact nor a declared rooted input.
  EXPECT_EQ(bus.Publish(std::move(undeclared)).code(),
            StatusCode::kFailedPrecondition);

  std::filesystem::remove_all(db);
}

// One tampered batch: a copy of `batch` with exactly one identity field
// replaced, its batch id recomputed, and the rejection the un-memoized pass
// produced for it.
//
// The two tamper cases in this file iterate this same set -- the memoized case
// and the total-collision case -- so the collision case cannot guard a smaller
// set than the case it is the twin of.
struct TamperedBatch {
  std::string name;
  AnalysisFactBatch batch;
  StatusCode code;
  std::string message;
};

// A row mutation changes the batch id, which is derived over the rows, so the
// batch id is recomputed after every row mutation. Leaving it stale would
// reject the batch at the batch-id gate before the check under test could run,
// and the test would pass without exercising anything.
//
// Each mutation rebuilds the batch from the rows it already holds with one row
// replaced: an arena is append-only, so a stored row cannot be mutated in
// place. Only one row per case is touched, so every other field -- including
// the batch id, which is recomputed over the rebuilt payload -- is exactly what
// the untouched cases see.
std::vector<TamperedBatch> TamperedBatches(const AnalysisFactBatch &batch) {
  // Every case below replaces a row, so a batch with none to replace is a
  // broken fixture rather than a case under test. Both callers assert the shape
  // first; this keeps a later caller from indexing an empty payload.
  if (batch.fact_count() == 0 || batch.witness_count() == 0) {
    std::abort();
  }

  // The tamper values, through the real parser: `ParseStableId` is the only
  // producer of a `StableId` from text. `funcvar` is the spelling this codebase
  // parses for a function-variant ID, and `callsite` for a call-site ID.
  auto zero_fact = core::ParseStableId("fact:sha256:" + std::string(64, '0'));
  auto other_function =
      core::ParseStableId("funcvar:sha256:" + std::string(64, 'a'));
  auto wrong_kind_function =
      core::ParseStableId("funcvar:sha256:" + std::string(64, 'b'));
  auto other_call_site =
      core::ParseStableId("callsite:sha256:" + std::string(64, 'b'));
  if (!zero_fact.ok() || !other_function.ok() || !wrong_kind_function.ok() ||
      !other_call_site.ok()) {
    // A fixture whose own tamper values do not parse is broken, not a case
    // under test.
    std::abort();
  }

  std::vector<TamperedBatch> cases;

  // A mutated fact id no longer matches its own row. The row is unchanged, so
  // this recomputes the id the batch already had; the identity check is what
  // has to fire here, not the batch-id gate.
  std::vector<AnalysisFact> fact_rows = Materialize(batch.facts());
  fact_rows[0].fact_id = *zero_fact;
  AnalysisFactBatch bad_fact = batch;
  bad_fact.SetFacts(fact_rows);
  bad_fact.batch_id = DeriveBatchId(bad_fact);
  cases.push_back(TamperedBatch{
      .name = "mutated fact id",
      .batch = std::move(bad_fact),
      .code = StatusCode::kFailedPrecondition,
      .message = "fact_id does not match its row",
  });

  // A mutated witness result row is no longer a published fact, so it cannot
  // close the witness for the fact it claims to prove.
  std::vector<WitnessEdge> result_rows = Materialize(batch.witnesses());
  result_rows[0].result.row.cells[0] = *other_function;
  AnalysisFactBatch bad_result = batch;
  bad_result.SetWitnesses(result_rows);
  bad_result.batch_id = DeriveBatchId(bad_result);
  cases.push_back(TamperedBatch{
      .name = "mutated witness result row",
      .batch = std::move(bad_result),
      .code = StatusCode::kFailedPrecondition,
      .message = "fact without a closed witness",
  });

  // A witness input row with a cell of the wrong domain is still rejected by
  // the per-cell schema check, which runs inside the identity derivation: a
  // memo hit must never stand in for a row that does not validate.
  std::vector<WitnessEdge> wrong_kind_rows = Materialize(batch.witnesses());
  wrong_kind_rows[0].input.row.cells[0] = *wrong_kind_function;
  AnalysisFactBatch wrong_kind_input = batch;
  wrong_kind_input.SetWitnesses(wrong_kind_rows);
  wrong_kind_input.batch_id = DeriveBatchId(wrong_kind_input);
  cases.push_back(TamperedBatch{
      .name = "witness input row of the wrong domain",
      .batch = std::move(wrong_kind_input),
      .code = StatusCode::kInvalidArgument,
      .message = "stable id kind mismatch",
  });

  // A witness input row whose identity is well-formed but is neither a
  // published fact nor a declared rooted input is still rejected.
  std::vector<WitnessEdge> bad_input_rows = Materialize(batch.witnesses());
  bad_input_rows[0].input.row.cells[0] = *other_call_site;
  AnalysisFactBatch bad_input = batch;
  bad_input.SetWitnesses(bad_input_rows);
  bad_input.batch_id = DeriveBatchId(bad_input);
  cases.push_back(TamperedBatch{
      .name = "witness input row outside the root set",
      .batch = std::move(bad_input),
      .code = StatusCode::kFailedPrecondition,
      .message = "witness leaf outside the root set",
  });

  return cases;
}

// Validate memoizes row identity so that the ~3.5M derivations it performs on a
// production batch collapse to roughly the distinct row count. The memo must
// not turn a rejection into an acceptance: this test tampers with one identity
// field at a time and requires the same rejection, with the same status and
// message, that the un-memoized pass produced.
//
// `Validate` is private and `Publish` is the seam that reaches it: a batch that
// fails validation is rejected before any sink is consulted, and with no sink
// registered `Publish` returns exactly the status `Validate` returned.
TEST(AnalysisFactBusTest, ValidateStillRejectsEachTamperedIdentity) {
  const auto db = TempDbPath();
  auto repo = wpa::WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok()) << repo.status().message();
  AnalysisFactBus bus(*repo);

  const AnalysisFactBatch batch = SuccessfulBatch();
  ASSERT_EQ(batch.fact_count(), 1u);
  ASSERT_EQ(batch.witness_count(), 1u);

  // Baseline: a well-formed batch validates.
  ASSERT_TRUE(bus.Publish(batch).ok());

  const std::vector<TamperedBatch> cases = TamperedBatches(batch);
  ASSERT_EQ(cases.size(), 4u);
  for (const TamperedBatch &tampered : cases) {
    const Status status = bus.Publish(tampered.batch);
    EXPECT_EQ(status.code(), tampered.code) << tampered.name;
    EXPECT_EQ(status.message(), tampered.message) << tampered.name;
  }

  // The mutations are independent: the original batch is still accepted.
  EXPECT_TRUE(bus.Publish(batch).ok());

  std::filesystem::remove_all(db);
}

// Hashing a key instead of comparing it exactly trades a comparison for a
// bucket, so the new failure mode is a collision resolved wrongly. This forces
// the worst case: a seam that reduces every row's identity to the same value,
// so the fact index degenerates to one bucket and a lookup can only be answered
// by comparing arena bytes.
//
// Both halves of the case guard that comparison, because with one fact and one
// witness the published fact, the witness's result row and its input row all
// reduce into the same bucket. Resolving a bucket without comparing makes the
// well-formed batch below fail first -- its input row resolves to the fact's
// own entry and closes a self-cycle -- and, on any batch where that did not
// happen first, would resolve a tampered endpoint to a wrong entry and accept
// it.
//
// It runs the tamper set `ValidateStillRejectsEachTamperedIdentity` runs, so
// the two cases differ only in the hash installed on the bus.
TEST(AnalysisFactBusTest, ValidateResolvesLookupsExactlyUnderATotalHashCollision) {
  const auto db = TempDbPath();
  auto repo = wpa::WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok()) << repo.status().message();
  AnalysisFactBus bus(*repo);
  bus.SetKeyHashForTesting([](const core::StableId &) { return 0u; });

  const AnalysisFactBatch batch = SuccessfulBatch();
  ASSERT_EQ(batch.fact_count(), 1u);
  ASSERT_EQ(batch.witness_count(), 1u);
  ASSERT_TRUE(bus.Publish(batch).ok());

  const std::vector<TamperedBatch> cases = TamperedBatches(batch);
  ASSERT_EQ(cases.size(), 4u);
  for (const TamperedBatch &tampered : cases) {
    const Status status = bus.Publish(tampered.batch);
    EXPECT_EQ(status.code(), tampered.code) << tampered.name;
    EXPECT_EQ(status.message(), tampered.message) << tampered.name;
  }

  std::filesystem::remove_all(db);
}

TEST(AnalysisFactBusTest, RejectsCyclicWitnessDag) {
  const auto db = TempDbPath();
  auto repo = wpa::WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok()) << repo.status().message();
  AnalysisFactBus bus(*repo);

  auto cycle = SuccessfulBatch();
  const SemanticRow forward = Reachable("f", "g");
  const SemanticRow reverse = Reachable("g", "f");
  cycle.SetFacts({MakeFact(forward).value(), MakeFact(reverse).value()});
  cycle.rooted_input_fact_ids.clear();
  cycle.SetWitnesses({Edge(forward, kDirect, reverse, 0),
                      Edge(reverse, kDirect, forward, 0)});
  cycle.batch_id = DeriveBatchId(cycle);

  const Status status = bus.Publish(cycle);
  EXPECT_EQ(status.code(), StatusCode::kFailedPrecondition);
  EXPECT_EQ(status.message(), "witness DAG contains a cycle");

  std::filesystem::remove_all(db);
}

TEST(AnalysisFactBusTest, RetryAfterPartialFanoutIsIdempotent) {
  const auto db = TempDbPath();
  auto repo = wpa::WpaRunRepository::Open(db);
  ASSERT_TRUE(repo.ok()) << repo.status().message();

  RecordingSink first;
  FailOnceSink second;
  AnalysisFactBus bus(*repo);
  bus.AddSink("first", first);
  bus.AddSink("second", second);

  const auto batch = SuccessfulBatch();
  EXPECT_FALSE(bus.Publish(batch).ok());
  EXPECT_TRUE(bus.Publish(batch).ok());
  EXPECT_EQ(first.logical_publication_count(batch.batch_id), 1u);
  EXPECT_EQ(second.logical_publication_count(batch.batch_id), 1u);

  std::filesystem::remove_all(db);
}

} // namespace
} // namespace veritas::facts
