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

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/core/Version.h"
#include "veritas/cpg/CpgQuery.h"
#include "veritas/cpg/CpgRepository.h"
#include "veritas/evidence/EvidenceQueryService.h"
#include "veritas/evidence/OverflowClaimSeed.h"
#include "veritas/evidence/FactStoreEvidenceBackend.h"
#include "veritas/evidence/SliceTypes.h"
#include "veritas/facts/AnalysisFact.h"
#include "veritas/facts/FactStore.h"
#include "veritas/summarydb/MetadataStore.h"

namespace {

using veritas::Status;
using veritas::StatusOr;
using veritas::core::ParseStableId;
using veritas::core::StableId;

constexpr std::string_view kUsage =
    "usage:\n"
    "  veritas-query --version\n"
    "  veritas-query callees <function-id> --revision <id> --build <id> --db <dir>\n"
    "  veritas-query flow <src-id> <dst-id> --projection <id> --db <dir> "
    "[--max-depth N --max-nodes N --max-paths N]\n"
    "  veritas-query evidence overflow --sink <value> --format json --db <dir> "
    "[--max-depth N --max-nodes N --max-paths N --max-facts N "
    "--max-provenance-depth N]\n";

std::string TakeValue(const std::vector<std::string>& args, std::size_t* i,
                      std::string_view flag) {
  if (*i + 1 >= args.size()) {
    std::cerr << "veritas-query: " << flag << " requires a value\n";
    std::exit(1);
  }
  return args[++*i];
}

int ReportError(std::string_view message) {
  std::cerr << "veritas-query: " << message << '\n';
  return 1;
}

// Non-throwing unsigned parse (VERITAS builds with -fno-exceptions).
bool ParseSize(std::string_view text, std::size_t* out) {
  const auto [ptr, ec] =
      std::from_chars(text.data(), text.data() + text.size(), *out);
  return ec == std::errc() && ptr == text.data() + text.size();
}

// ---------------------------------------------------------------------------
// `evidence overflow` — the M10B demonstration query
// ---------------------------------------------------------------------------

// The evidence command's supported option surface. Any other `--flag` is
// rejected as unsupported rather than silently ignored.
constexpr std::string_view kEvidenceSinks[] = {"memcpy"};
constexpr std::string_view kEvidenceFormats[] = {"json"};

struct EvidenceOptions {
  std::string db_path;
  std::string sink;
  veritas::evidence::EvidenceQueryBudget budget{/*max_depth=*/8,
                                                /*max_nodes=*/256,
                                                /*max_paths=*/5,
                                                /*max_facts_per_query=*/64,
                                                /*max_provenance_depth=*/8};
};

bool IsSupported(std::string_view value,
                 const std::span<const std::string_view> allowed) {
  for (const std::string_view candidate : allowed) {
    if (candidate == value) {
      return true;
    }
  }
  return false;
}

// Parses the option tail of `evidence overflow`. Every supported option may
// appear at most once; unknown options, missing values, empty values, zero
// budgets, and overflowing integers are rejected with a stable message.
Status ParseEvidenceOptions(const std::vector<std::string>& args,
                            const std::size_t first, EvidenceOptions* out) {
  std::string format;
  std::set<std::string> seen;
  for (std::size_t i = first; i < args.size(); ++i) {
    const std::string flag = args[i];
    if (flag.empty() || flag[0] != '-') {
      return Status::InvalidArgument("unexpected argument '" + flag + "'");
    }
    if (!seen.insert(flag).second) {
      return Status::InvalidArgument("duplicate option " + flag);
    }
    const auto assign_size = [&](std::size_t* target) -> Status {
      const std::string value = TakeValue(args, &i, flag);
      if (value.empty() || !ParseSize(value, target)) {
        return Status::InvalidArgument("invalid " + flag);
      }
      if (*target == 0) {
        return Status::InvalidArgument(flag + " must be positive");
      }
      return Status::Ok();
    };
    if (flag == "--db") {
      out->db_path = TakeValue(args, &i, flag);
      if (out->db_path.empty()) {
        return Status::InvalidArgument("--db requires a value");
      }
    } else if (flag == "--sink") {
      out->sink = TakeValue(args, &i, flag);
      if (out->sink.empty()) {
        return Status::InvalidArgument("--sink requires a value");
      }
      if (!IsSupported(out->sink, kEvidenceSinks)) {
        return Status::InvalidArgument("unsupported --sink '" + out->sink +
                                       "'");
      }
    } else if (flag == "--format") {
      format = TakeValue(args, &i, flag);
      if (!IsSupported(format, kEvidenceFormats)) {
        return Status::InvalidArgument("unsupported --format '" + format + "'");
      }
    } else if (flag == "--max-depth") {
      if (Status s = assign_size(&out->budget.max_depth); !s.ok()) return s;
    } else if (flag == "--max-nodes") {
      if (Status s = assign_size(&out->budget.max_nodes); !s.ok()) return s;
    } else if (flag == "--max-paths") {
      if (Status s = assign_size(&out->budget.max_paths); !s.ok()) return s;
    } else if (flag == "--max-facts") {
      if (Status s = assign_size(&out->budget.max_facts_per_query); !s.ok()) {
        return s;
      }
    } else if (flag == "--max-provenance-depth") {
      if (Status s = assign_size(&out->budget.max_provenance_depth); !s.ok()) {
        return s;
      }
    } else {
      return Status::InvalidArgument("unsupported option " + flag);
    }
  }
  if (out->sink.empty()) {
    return Status::InvalidArgument("--sink <value> is required");
  }
  if (format.empty()) {
    return Status::InvalidArgument("--format <value> is required");
  }
  if (out->db_path.empty()) {
    return Status::InvalidArgument("--db <dir> is required");
  }
  return Status::Ok();
}

// Discovers the single materialized analysis the store holds: its current CPG
// projection, the analysis run bound to it, and the repository the projection's
// revision belongs to. Several runs or projections make the store ambiguous and
// are reported rather than resolved by guessing.
StatusOr<StableId> SingleRunId(veritas::summarydb::MetadataStore& metadata) {
  auto rows = metadata.Query(
      "SELECT DISTINCT run_id FROM run_fact_bindings WHERE is_current = 1", {});
  if (!rows.ok()) {
    return rows.status();
  }
  if (rows->size() != 1 || (*rows)[0].empty()) {
    return Status::NotFound(
        "expected exactly one analysis run in the fact store");
  }
  return ParseStableId((*rows)[0][0]);
}

int RunEvidenceOverflow(const std::vector<std::string>& args) {
  if (args.size() < 2 || args[1] != "overflow") {
    std::cerr << kUsage;
    return 1;
  }
  EvidenceOptions options;
  if (Status status = ParseEvidenceOptions(args, /*first=*/2, &options);
      !status.ok()) {
    return ReportError(status.message());
  }

  auto metadata_result =
      veritas::summarydb::MetadataStore::Open(options.db_path + "/metadata.db");
  if (!metadata_result.ok()) return ReportError(metadata_result.status().message());
  auto metadata = std::move(*metadata_result);
  if (auto schema = metadata.ApplySchema(); !schema.ok()) {
    return ReportError(schema.message());
  }

  auto projections = metadata.Query(
      "SELECT revision_id, build_variant_id, projection_id "
      "FROM current_cpg_projections",
      {});
  if (!projections.ok()) return ReportError(projections.status().message());
  if (projections->size() != 1 || (*projections)[0].size() != 3) {
    return ReportError(
        "expected exactly one current CPG projection in the store");
  }
  const std::string& revision = (*projections)[0][0];
  const std::string& build_variant = (*projections)[0][1];
  auto projection_id = ParseStableId((*projections)[0][2]);
  if (!projection_id.ok()) return ReportError("invalid stored projection id");

  auto repository = metadata.Query(
      "SELECT repository_id FROM revisions WHERE revision_id = ?", {revision});
  if (!repository.ok()) return ReportError(repository.status().message());
  if (repository->size() != 1 || (*repository)[0].empty()) {
    return ReportError("the projection's revision has no repository");
  }

  auto run_id = SingleRunId(metadata);
  if (!run_id.ok()) return ReportError(run_id.status().message());

  veritas::cpg::CpgRepository cpg_repository(metadata);
  auto cpg = cpg_repository.LoadProjection(*projection_id);
  if (!cpg.ok()) return ReportError(cpg.status().message());

  auto fact_store = veritas::facts::FactStore::Open(options.db_path);
  if (!fact_store.ok()) return ReportError(fact_store.status().message());

  auto current_facts = fact_store->GetCurrentFacts(*run_id);
  if (!current_facts.ok()) return ReportError(current_facts.status().message());

  const veritas::evidence::SnapshotDescriptor descriptor =
      veritas::evidence::DeriveSnapshotDescriptor(*cpg, (*repository)[0][0],
                                                  *run_id, *current_facts);
  (void)revision;
  (void)build_variant;

  auto seed = veritas::evidence::ResolveOverflowClaimSeed(
      *cpg, *current_facts, options.sink);
  if (!seed.ok()) return ReportError(seed.status().message());

  veritas::evidence::FactStoreEvidenceBackend backend(*fact_store, descriptor);
  veritas::evidence::EvidenceQueryService service(*cpg, backend, *run_id);
  auto input = service.BuildEvidenceInput(*seed, options.budget);
  if (!input.ok()) return ReportError(input.status().message());

  std::cout << veritas::evidence::ToDiagnosticJson(*input);
  return 0;
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
  const std::string command = args[0];

  if (command == "evidence") {
    return RunEvidenceOverflow(args);
  }

  std::string db_path;
  std::string revision;
  std::string build;
  std::string projection;
  veritas::cpg::QueryBudget budget{/*max_depth=*/10, /*max_nodes=*/1000,
                                   /*max_paths=*/1000};

  std::vector<std::string> positionals;
  for (std::size_t i = 1; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (arg == "--db") {
      db_path = TakeValue(args, &i, "--db");
    } else if (arg == "--revision") {
      revision = TakeValue(args, &i, "--revision");
    } else if (arg == "--build") {
      build = TakeValue(args, &i, "--build");
    } else if (arg == "--projection") {
      projection = TakeValue(args, &i, "--projection");
    } else if (arg == "--max-depth") {
      if (!ParseSize(TakeValue(args, &i, "--max-depth"), &budget.max_depth)) {
        return ReportError("invalid --max-depth");
      }
    } else if (arg == "--max-nodes") {
      if (!ParseSize(TakeValue(args, &i, "--max-nodes"), &budget.max_nodes)) {
        return ReportError("invalid --max-nodes");
      }
    } else if (arg == "--max-paths") {
      if (!ParseSize(TakeValue(args, &i, "--max-paths"), &budget.max_paths)) {
        return ReportError("invalid --max-paths");
      }
    } else {
      positionals.push_back(arg);
    }
  }

  if (db_path.empty()) return ReportError("--db <dir> is required");

  auto md_result = veritas::summarydb::MetadataStore::Open(db_path + "/metadata.db");
  if (!md_result.ok()) return ReportError(md_result.status().message());
  auto metadata = std::move(*md_result);
  if (auto schema = metadata.ApplySchema(); !schema.ok()) {
    return ReportError(schema.message());
  }
  veritas::cpg::CpgRepository repository(metadata);

  if (command == "callees") {
    if (positionals.size() != 1) {
      std::cerr << kUsage;
      return 1;
    }
    auto revision_id = ParseStableId(revision);
    auto build_id = ParseStableId(build);
    if (!revision_id.ok() || !build_id.ok()) {
      return ReportError("invalid --revision or --build");
    }
    auto query =
        veritas::cpg::CpgQuery::OpenCurrent(repository, *revision_id, *build_id);
    if (!query.ok()) return ReportError(query.status().message());
    auto function_id = ParseStableId(positionals[0]);
    if (!function_id.ok()) return ReportError("invalid function-id");
    auto callees = query->GetCallees(*function_id);
    if (!callees.ok()) return ReportError(callees.status().message());
    for (const auto& callee : *callees) {
      std::cout << veritas::core::ToString(callee.node_id) << " " << callee.label
                << '\n';
    }
    return 0;
  }

  if (command == "flow") {
    if (positionals.size() != 2) {
      std::cerr << kUsage;
      return 1;
    }
    auto projection_id = ParseStableId(projection);
    if (!projection_id.ok()) return ReportError("invalid --projection");
    auto query = veritas::cpg::CpgQuery::OpenProjection(repository, *projection_id);
    if (!query.ok()) return ReportError(query.status().message());
    auto src = ParseStableId(positionals[0]);
    auto dst = ParseStableId(positionals[1]);
    if (!src.ok() || !dst.ok()) return ReportError("invalid flow endpoint");
    auto result = query->GetValueFlow(*src, *dst, budget);
    if (!result.ok()) return ReportError(result.status().message());

    std::cout << "Projection: " << projection << '\n';
    std::cout << "Paths: " << result->items.size() << '\n';
    std::cout << "Explored nodes: " << result->explored_nodes << '\n';
    std::cout << "Explored paths: " << result->explored_paths << '\n';
    std::cout << "Truncated by:";
    for (auto reason : result->truncation_reasons) {
      std::cout << ' ' << static_cast<int>(reason);
    }
    std::cout << (result->truncation_reasons.empty() ? " none" : "") << '\n';
    return 0;
  }

  std::cerr << kUsage;
  return 1;
}
