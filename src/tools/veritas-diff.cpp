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
#include "veritas/summary/v1/summary.pb.h"
#include "veritas/summarydb/DependencyIndex.h"
#include "veritas/summarydb/SummaryDelta.h"
#include "veritas/summarydb/SummaryRepository.h"

namespace {

using veritas::core::ParseStableId;
using veritas::core::StableId;

constexpr std::string_view kUsage =
    "usage:\n"
    "  veritas-diff --version\n"
    "  veritas-diff --db <dir> --old <summary-id> --new <summary-id> "
    "[--max-consumers N --max-depth N]\n";

std::string TakeValue(const std::vector<std::string>& args, std::size_t* i,
                      std::string_view flag) {
  if (*i + 1 >= args.size()) {
    std::cerr << flag << " requires a value\n";
    std::exit(1);
  }
  return args[++*i];
}

int ReportError(std::string_view message) {
  std::cerr << "veritas-diff: " << message << '\n';
  return 1;
}

// Non-throwing unsigned parse (VERITAS builds with -fno-exceptions).
bool ParseSize(std::string_view text, std::size_t* out) {
  const auto [ptr, ec] =
      std::from_chars(text.data(), text.data() + text.size(), *out);
  return ec == std::errc() && ptr == text.data() + text.size();
}

// Human-readable component name, with the proto's COMPONENT_KIND_ prefix
// stripped so output reads "RANGE_FACTS" rather than "COMPONENT_KIND_RANGE_FACTS".
std::string ComponentName(veritas::summary::v1::ComponentKind kind) {
  const std::string name = veritas::summary::v1::ComponentKind_Name(kind);
  constexpr std::string_view kPrefix = "COMPONENT_KIND_";
  if (name.size() > kPrefix.size() &&
      name.compare(0, kPrefix.size(), kPrefix) == 0) {
    return name.substr(kPrefix.size());
  }
  return name;
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

  std::string db_path;
  std::string old_str;
  std::string new_str;
  veritas::summarydb::ImpactBudget budget;

  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (arg == "--db") {
      db_path = TakeValue(args, &i, "--db");
    } else if (arg == "--old") {
      old_str = TakeValue(args, &i, "--old");
    } else if (arg == "--new") {
      new_str = TakeValue(args, &i, "--new");
    } else if (arg == "--max-consumers") {
      if (!ParseSize(TakeValue(args, &i, "--max-consumers"),
                     &budget.max_consumers)) {
        return ReportError("invalid --max-consumers");
      }
    } else if (arg == "--max-depth") {
      if (!ParseSize(TakeValue(args, &i, "--max-depth"), &budget.max_depth)) {
        return ReportError("invalid --max-depth");
      }
    } else {
      return ReportError("unknown argument: " + arg);
    }
  }

  if (db_path.empty()) return ReportError("--db <dir> is required");
  if (old_str.empty()) return ReportError("--old <summary-id> is required");
  if (new_str.empty()) return ReportError("--new <summary-id> is required");

  auto old_id = ParseStableId(old_str);
  auto new_id = ParseStableId(new_str);
  if (!old_id.ok() || !new_id.ok()) {
    return ReportError("invalid --old or --new");
  }

  auto repo_result = veritas::summarydb::SummaryRepository::Open(db_path);
  if (!repo_result.ok()) return ReportError(repo_result.status().message());
  auto repo = std::move(*repo_result);

  auto old_summary = repo->GetSummary(*old_id);
  if (!old_summary.ok()) return ReportError(old_summary.status().message());
  auto new_summary = repo->GetSummary(*new_id);
  if (!new_summary.ok()) return ReportError(new_summary.status().message());

  auto delta = veritas::summarydb::DiffSummaries(*old_summary, *new_summary);
  if (!delta.ok()) return ReportError(delta.status().message());

  veritas::summarydb::DependencyIndex index(repo->metadata_store());
  auto impact = index.GetImpactSet(*delta, budget);
  if (!impact.ok()) return ReportError(impact.status().message());

  std::cout << "Function: " << old_summary->identity().function_variant_id()
            << '\n';
  std::cout << "Old summary: " << old_str << '\n';
  std::cout << "New summary: " << new_str << '\n';
  std::cout << "Semantic changed: " << (delta->semantic_changed ? "yes" : "no")
            << '\n';
  std::cout << "Evidence changed: " << (delta->evidence_changed ? "yes" : "no")
            << '\n';

  std::cout << "Changed components:\n";
  for (const auto& component : delta->changed_components) {
    std::cout << "  " << ComponentName(component.component_kind) << '\n';
  }

  std::cout << "Impacted consumers: " << impact->consumers.size() << '\n';
  for (const auto& consumer : impact->consumers) {
    std::cout << "  " << veritas::core::ToString(consumer.consumer_id) << ' '
              << ComponentName(consumer.consumer_component) << '\n';
  }

  std::cout << "Truncated: " << (impact->truncated ? "yes" : "no") << '\n';

  return 0;
}
