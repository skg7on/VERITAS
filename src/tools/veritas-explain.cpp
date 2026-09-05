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

#include <charconv>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <google/protobuf/util/json_util.h>

#include "veritas/core/Ids.h"
#include "veritas/core/Version.h"
#include "veritas/facts/FactStore.h"
#include "veritas/facts/ProvenanceStore.h"

namespace {

namespace fp = veritas::fact::v1;

constexpr std::string_view kUsage =
    "usage:\n"
    "  veritas-explain --version\n"
    "  veritas-explain fact <fact-id> --run <run-id> --db <dir>"
    " [--max-depth N --max-nodes N] [--json]\n";

std::string TakeValue(const std::vector<std::string>& args, std::size_t* i,
                      std::string_view flag) {
  if (*i + 1 >= args.size()) {
    std::cerr << flag << " requires a value\n";
    std::exit(1);
  }
  return args[++*i];
}

int ReportError(std::string_view message) {
  std::cerr << "veritas-explain: " << message << '\n';
  return 1;
}

// Non-throwing unsigned parse (VERITAS builds with -fno-exceptions).
bool ParseU32(std::string_view text, std::uint32_t* out) {
  const auto [ptr, ec] =
      std::from_chars(text.data(), text.data() + text.size(), *out);
  return ec == std::errc() && ptr == text.data() + text.size();
}

std::string_view EpistemicName(fp::EpistemicState state) {
  switch (state) {
    case fp::EPISTEMIC_MUST:
      return "MUST";
    case fp::EPISTEMIC_MAY:
      return "MAY";
    case fp::EPISTEMIC_MUST_NOT:
      return "MUST_NOT";
    case fp::EPISTEMIC_INFERRED:
      return "INFERRED";
    case fp::EPISTEMIC_ASSUMED:
      return "ASSUMED";
    case fp::EPISTEMIC_UNKNOWN:
      return "UNKNOWN";
    default:
      return "UNSPECIFIED";
  }
}

std::string_view ProducerName(fp::ProducerKind kind) {
  switch (kind) {
    case fp::PRODUCER_WPA_SOUFFLE:
      return "WPA_SOUFFLE";
    case fp::PRODUCER_WPA_CPP_CONFORMANCE:
      return "WPA_CPP_CONFORMANCE";
    case fp::PRODUCER_WPA_CPP_EMERGENCY:
      return "WPA_CPP_EMERGENCY";
    case fp::PRODUCER_EXTERNAL:
      return "EXTERNAL";
    default:
      return "UNSPECIFIED";
  }
}

std::string CellString(const fp::Cell& cell) {
  switch (cell.value_case()) {
    case fp::Cell::kStableId:
      return cell.stable_id();
    case fp::Cell::kInt64Value:
      return std::to_string(cell.int64_value());
    case fp::Cell::kUint64Value:
      return std::to_string(cell.uint64_value());
    case fp::Cell::kStringValue:
      return cell.string_value();
    case fp::Cell::kDispatchKind: {
      switch (cell.dispatch_kind()) {
        case fp::DISPATCH_DIRECT:
          return "direct";
        case fp::DISPATCH_INDIRECT:
          return "indirect";
        case fp::DISPATCH_VIRTUAL:
          return "virtual";
        case fp::DISPATCH_CALLBACK:
          return "callback";
        case fp::DISPATCH_EXTERNAL:
          return "external";
        default:
          return "unknown";
      }
    }
    case fp::Cell::kAliasKind: {
      switch (cell.alias_kind()) {
        case fp::ALIAS_MUST:
          return "must_alias";
        case fp::ALIAS_MAY:
          return "may_alias";
        case fp::ALIAS_NO:
          return "no_alias";
        default:
          return "unknown_alias";
      }
    }
    case fp::Cell::kByteRangeKind:
      return cell.byte_range_kind() == fp::BYTE_RANGE_KNOWN ? "known" : "unknown";
    case fp::Cell::kEpistemic:
      return std::string(EpistemicName(cell.epistemic()));
    case fp::Cell::VALUE_NOT_SET:
    default:
      return "";
  }
}

// Returns the fact's epistemic state, or UNKNOWN when the relation carries no
// epistemic column.
fp::EpistemicState FactEpistemic(const fp::Fact& fact) {
  for (const auto& cell : fact.cells()) {
    if (cell.value_case() == fp::Cell::kEpistemic) {
      return cell.epistemic();
    }
  }
  return fp::EPISTEMIC_UNKNOWN;
}

// True when a stored fact's semantic row carries the given epistemic state.
bool HasEpistemic(const veritas::facts::AnalysisFact& fact,
                  veritas::analysis::semantic::EpistemicState target) {
  for (const auto& cell : fact.row.cells) {
    if (const auto* state =
            std::get_if<veritas::analysis::semantic::EpistemicState>(&cell)) {
      return *state == target;
    }
  }
  return false;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    std::cout << veritas::FormatVersion(veritas::GetVersion()) << '\n';
    return 0;
  }
  if (argc < 2) {
    std::cerr << kUsage;
    return 1;
  }

  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
  if (args[0] != "fact") {
    std::cerr << kUsage;
    return 1;
  }

  std::string run_id_text;
  std::string db_path;
  bool json = false;
  veritas::facts::ExplainBudget budget;
  std::vector<std::string> positionals;
  for (std::size_t i = 1; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (arg == "--run") {
      run_id_text = TakeValue(args, &i, "--run");
    } else if (arg == "--db") {
      db_path = TakeValue(args, &i, "--db");
    } else if (arg == "--max-depth") {
      if (!ParseU32(TakeValue(args, &i, "--max-depth"), &budget.max_depth)) {
        return ReportError("invalid --max-depth");
      }
    } else if (arg == "--max-nodes") {
      if (!ParseU32(TakeValue(args, &i, "--max-nodes"), &budget.max_nodes)) {
        return ReportError("invalid --max-nodes");
      }
    } else if (arg == "--json") {
      json = true;
    } else {
      positionals.push_back(arg);
    }
  }

  if (positionals.size() != 1) {
    std::cerr << kUsage;
    return 1;
  }
  if (db_path.empty()) return ReportError("--db <dir> is required");
  if (run_id_text.empty()) return ReportError("--run <run-id> is required");

  const auto fact_id = veritas::core::ParseStableId(positionals[0]);
  if (!fact_id.ok()) return ReportError("invalid fact-id");
  const auto run_id = veritas::core::ParseStableId(run_id_text);
  if (!run_id.ok()) return ReportError("invalid run-id");

  auto store = veritas::facts::FactStore::Open(db_path);
  if (!store.ok()) return ReportError(store.status().message());

  veritas::facts::ProvenanceStore provenance(store->metadata_store());
  auto graph = provenance.Explain(*run_id, *fact_id, budget);
  if (!graph.ok()) return ReportError(graph.status().message());

  if (json) {
    std::string json_text;
    auto status = google::protobuf::util::MessageToJsonString(*graph, &json_text);
    if (!status.ok()) return ReportError(status.message());
    std::cout << json_text << '\n';
    return 0;
  }

  // Text output: the bounded explanation sections.
  const auto& fact = graph->fact();
  std::cout << "Fact: " << fact.relation_name();
  for (const auto& cell : fact.cells()) {
    std::cout << ' ' << CellString(cell);
  }
  std::cout << '\n';
  std::cout << "FactID: " << graph->fact_id() << '\n';
  std::cout << "Epistemic: " << EpistemicName(FactEpistemic(fact)) << '\n';
  std::cout << "Confidence: ";
  if (graph->binding().has_confidence()) {
    std::cout << graph->binding().confidence();
  } else {
    std::cout << "none";
  }
  std::cout << '\n';
  std::cout << "Producer: " << ProducerName(graph->binding().producer_kind())
            << '\n';

  // The top fact's rule comes from its witness node.
  std::string rule = "(rooted input)";
  std::vector<std::string> source_anchors;
  for (const auto& node : graph->nodes()) {
    if (node.output_fact_id() == graph->fact_id()) {
      rule = node.rule_id().empty() ? "(rooted input)" : node.rule_id();
    }
    if (!node.source_anchor_id().empty()) {
      source_anchors.push_back(node.source_anchor_id());
    }
  }
  std::cout << "Rule: " << rule << '\n';

  std::cout << "Inputs:\n";
  std::set<std::string> assumption_ids;
  std::set<std::string> unknown_ids;
  for (const auto& edge : graph->edges()) {
    std::cout << "  " << edge.input_id() << " (" << edge.input_kind() << ")\n";
    auto input_id = veritas::core::ParseStableId(edge.input_id());
    if (!input_id.ok()) {
      continue;
    }
    auto input = store->GetFact(*input_id);
    if (!input.ok()) {
      continue;
    }
    if (HasEpistemic(*input, veritas::analysis::semantic::EpistemicState::kAssumed)) {
      assumption_ids.insert(edge.input_id());
    }
    if (HasEpistemic(*input, veritas::analysis::semantic::EpistemicState::kUnknown)) {
      unknown_ids.insert(edge.input_id());
    }
  }

  std::cout << "Assumptions:\n";
  if (assumption_ids.empty()) {
    std::cout << "  (none)\n";
  } else {
    for (const auto& id : assumption_ids) std::cout << "  " << id << '\n';
  }
  std::cout << "Unknowns:\n";
  if (unknown_ids.empty()) {
    std::cout << "  (none)\n";
  } else {
    for (const auto& id : unknown_ids) std::cout << "  " << id << '\n';
  }
  std::cout << "Source anchors:\n";
  if (source_anchors.empty()) {
    std::cout << "  (none)\n";
  } else {
    for (const auto& anchor : source_anchors) std::cout << "  " << anchor << '\n';
  }
  std::cout << "Truncated: " << (graph->truncated() ? graph->truncation_reason()
                                                   : "no")
            << '\n';

  return 0;
}
