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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

#include <unistd.h>

#include "veritas/build/AnalysisManifest.h"
#include "veritas/core/Ids.h"
#include "veritas/core/Version.h"
#include "veritas/cpg/CpgQuery.h"
#include "veritas/cpg/CpgRepository.h"
#include "veritas/evidence/EirText.h"
#include "veritas/evidence/EvidenceCase.h"
#include "veritas/evidence/EvidenceCaseBuilder.h"
#include "veritas/evidence/EvidenceJson.h"
#include "veritas/evidence/EvidenceProto.h"
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
    "  veritas-query evidence overflow --sink <value> --format <fmt> --db <dir> "
    "[--level l0|l1|l2 --output <path> --max-depth N --max-nodes N "
    "--max-paths N --max-facts N --max-provenance-depth N]\n"
    "\n"
    "  --format json     the M10B diagnostic slice JSON (default level: none)\n"
    "  --format eir-t    canonical EIR-T text at --level (default l1)\n"
    "  --format eir-json full-fidelity EIR JSON at --level (default l1)\n"
    "  --format protobuf EIR Protobuf at --level (default l1); --output is "
    "required\n";

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
// `json` is M10B's diagnostic slice, and it is NOT an EIR representation: it
// carries no level, no identity, and no omissions list. It stays in this
// array, and keeps its meaning exactly, so DEM-001's failure mode ("`json`
// silently changes to full EIR") cannot happen by extending the list.
constexpr std::string_view kEvidenceFormats[] = {"json", "eir-t", "eir-json",
                                                 "protobuf"};

struct EvidenceOptions {
  std::string db_path;
  std::string sink;
  std::string format;
  std::string output;
  veritas::evidence::EvidenceLevel level = veritas::evidence::EvidenceLevel::kL1;
  bool level_given = false;
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
      out->format = TakeValue(args, &i, flag);
      if (!IsSupported(out->format, kEvidenceFormats)) {
        return Status::InvalidArgument("unsupported --format '" + out->format +
                                       "'");
      }
    } else if (flag == "--level") {
      const std::string value = TakeValue(args, &i, flag);
      auto level = veritas::evidence::ParseEvidenceLevel(value);
      if (!level.ok()) {
        return Status::InvalidArgument("unsupported --level '" + value + "'");
      }
      out->level = *level;
      out->level_given = true;
    } else if (flag == "--output") {
      out->output = TakeValue(args, &i, flag);
      if (out->output.empty()) {
        return Status::InvalidArgument("--output requires a value");
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
  if (out->format.empty()) {
    return Status::InvalidArgument("--format <value> is required");
  }
  if (out->db_path.empty()) {
    return Status::InvalidArgument("--db <dir> is required");
  }
  // The binary representation is the one format whose payload is not text, so
  // it is the one format that must be told where to put it. Writing raw
  // Protobuf to stdout would corrupt a terminal or a pipe, and DEM-004 names
  // "binary stdout" as the forbidden outcome.
  if (out->format == "protobuf" && out->output.empty()) {
    return Status::InvalidArgument("--format protobuf requires --output <path>");
  }
  if (out->format != "protobuf" && !out->output.empty()) {
    return Status::InvalidArgument("--output is only supported with "
                                   "--format protobuf");
  }
  // `json` is the level-less M10B slice; a level would have nothing to select
  // and accepting one silently would be the first step toward DEM-001's
  // failure mode.
  if (out->format == "json" && out->level_given) {
    return Status::InvalidArgument("--level is not supported with --format "
                                   "json: the M10B slice carries no EIR level");
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

// The M10C program identity, read back from the store the analysis published
// into rather than reconstructed from the checkout path.
//
// `repository_id`, `revision_id`, and `build_variant_id` come from the same
// rows the M10B slice already resolves, so the case cannot disagree with the
// projection it was built from. `target_triple` and `type_layout_hash` are the
// two `ProgramContext` members no other read surface carries; they are stored
// on the `build_variants` row the projection's `build_variant_id` names, which
// is where M2 published them. Nothing here is invented: a store missing that
// row is reported rather than defaulted, because a case bound to a fabricated
// toolchain would be unverifiable.
StatusOr<veritas::build::ProgramContext> BuildProgramContext(
    veritas::summarydb::MetadataStore& metadata, const std::string& repository,
    const std::string& revision, const std::string& build_variant) {
  auto rows = metadata.Query(
      "SELECT target_triple, type_layout_hash FROM build_variants "
      "WHERE build_variant_id = ?",
      {build_variant});
  if (!rows.ok()) {
    return rows.status();
  }
  if (rows->size() != 1 || (*rows)[0].size() != 2) {
    return Status::NotFound(
        "the store holds no build-variant row for '" + build_variant +
        "', so the evidence case cannot be bound to a target triple");
  }
  veritas::build::ProgramContext context;
  context.repository_id = repository;
  context.revision_id = revision;
  context.build_variant_id = build_variant;
  context.target_triple = (*rows)[0][0];
  context.type_layout_hash = (*rows)[0][1];
  return context;
}

// ---------------------------------------------------------------------------
// Failure-atomic binary output (DEM-004)
// ---------------------------------------------------------------------------
//
// The destination is never opened for writing. The bytes go to a uniquely
// named sibling temporary first and reach the destination only through one
// `rename`, which is atomic within a filesystem — so a reader sees either the
// previous destination or the complete new one, and never a truncated file.
//
// The temporary is the only path any failure removes. Removing the destination
// instead would delete the prior artifact a failed run was supposed to leave
// intact, which is the outcome DEM-004 forbids.
Status WriteProtobufAtomically(const std::string& destination,
                               const std::string& bytes) {
  namespace fs = std::filesystem;
  static std::size_t counter = 0;
  std::error_code ignored;

  fs::path temporary(destination);
  temporary += ".tmp.";
  temporary += std::to_string(static_cast<long long>(::getpid()));
  temporary += ".";
  temporary += std::to_string(++counter);

  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      return Status::Internal("cannot create the temporary file beside '" +
                              destination + "'");
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    if (!out.good()) {
      out.close();
      fs::remove(temporary, ignored);
      return Status::Internal("cannot write the temporary file beside '" +
                              destination + "'");
    }
    out.close();
    if (out.fail()) {
      fs::remove(temporary, ignored);
      return Status::Internal("cannot close the temporary file beside '" +
                              destination + "'");
    }
  }

  fs::rename(temporary, fs::path(destination), ignored);
  if (ignored) {
    // Only the temporary this call created is removed. The destination is left
    // exactly as the caller found it.
    fs::remove(temporary, ignored);
    return Status::Internal("cannot replace '" + destination +
                            "': " + ignored.message());
  }
  return Status::Ok();
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

  auto seed = veritas::evidence::ResolveOverflowClaimSeed(
      *cpg, *current_facts, options.sink);
  if (!seed.ok()) return ReportError(seed.status().message());

  veritas::evidence::FactStoreEvidenceBackend backend(*fact_store, descriptor);
  veritas::evidence::EvidenceQueryService service(*cpg, backend, *run_id);
  auto input = service.BuildEvidenceInput(*seed, options.budget);
  if (!input.ok()) return ReportError(input.status().message());

  // M10B's slice JSON is emitted here, before any M10C step, so extending the
  // format list cannot change it: this branch is the same code path, over the
  // same `EvidenceBuildInput`, that DEM-001 pins.
  if (options.format == "json") {
    std::cout << veritas::evidence::ToDiagnosticJson(*input);
    return 0;
  }

  // The M10C boundary. The request carries the two program-identity values
  // `build::ProgramContext` cannot: the configuration the snapshot ran under,
  // and (deliberately) no analyzer versions, because M10C reports the analyzers
  // it was told about rather than synthesizing a set.
  auto context = BuildProgramContext(metadata, (*repository)[0][0], revision,
                                     build_variant);
  if (!context.ok()) return ReportError(context.status().message());

  veritas::evidence::EvidenceBuildRequest request;
  request.context = std::move(*context);
  request.input = std::move(*input);
  request.level = options.level;
  request.analysis_configuration_id = descriptor.analysis_config;

  auto built = veritas::evidence::EvidenceCaseBuilder().Build(request);
  if (!built.ok()) return ReportError(built.status().message());
  const veritas::evidence::EvidenceCase& value = *built;

  // One dispatch, three writers. The CLI never assembles EIR itself: each
  // representation is the model's own serializer, so the text, the JSON, and
  // the wire bytes are three views of one case that a single builder produced
  // once. `Build` finalizes, so every writer's finalized-case precondition
  // holds here rather than being restored by a second `FinalizeEvidenceIdentity`.
  if (options.format == "eir-t") {
    auto text = veritas::evidence::WriteEirText(
        value, veritas::evidence::EirTextStyle::kCanonical);
    if (!text.ok()) return ReportError(text.status().message());
    std::cout << *text;
    return 0;
  }
  if (options.format == "eir-json") {
    auto json = veritas::evidence::ToEvidenceJson(value);
    if (!json.ok()) return ReportError(json.status().message());
    std::cout << *json;
    return 0;
  }
  if (options.format == "protobuf") {
    auto bytes = veritas::evidence::EncodeEvidenceProto(value);
    if (!bytes.ok()) return ReportError(bytes.status().message());
    if (Status status = WriteProtobufAtomically(options.output, *bytes);
        !status.ok()) {
      return ReportError(status.message());
    }
    return 0;
  }

  return ReportError("unsupported --format '" + options.format + "'");
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
