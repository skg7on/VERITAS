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
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "veritas/core/Ids.h"
#include "veritas/core/Version.h"
#include "veritas/cpg/CpgQuery.h"
#include "veritas/cpg/CpgRepository.h"
#include "veritas/summarydb/MetadataStore.h"

namespace {

using veritas::core::ParseStableId;
using veritas::core::StableId;

constexpr std::string_view kUsage =
    "usage:\n"
    "  veritas-query --version\n"
    "  veritas-query callees <function-id> --revision <id> --build <id> --db <dir>\n"
    "  veritas-query flow <src-id> <dst-id> --projection <id> --db <dir> "
    "[--max-depth N --max-nodes N --max-paths N]\n";

std::string TakeValue(const std::vector<std::string>& args, std::size_t* i,
                      std::string_view flag) {
  if (*i + 1 >= args.size()) {
    std::cerr << flag << " requires a value\n";
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
