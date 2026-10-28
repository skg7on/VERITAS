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

#include "veritas/review/ProgramScope.h"
#include <algorithm>
#include <filesystem>
#include <functional>
#include <set>
namespace veritas::review {
StatusOr<std::string> NormalizeReviewPath(std::string_view text) {
  if (text.empty() || text.find('\0') != std::string_view::npos ||
      text.find('\\') != std::string_view::npos)
    return Status::InvalidArgument("invalid repository-relative path");
  std::filesystem::path path{std::string(text)};
  if (path.is_absolute())
    return Status::InvalidArgument("absolute input path");
  for (const auto &part : path)
    if (part == ".." || part == ".git")
      return Status::InvalidArgument("traversal or repository metadata path");
  auto normalized = path.lexically_normal().generic_string();
  while (normalized.ends_with('/'))
    normalized.pop_back();
  if (normalized.empty() || normalized == ".")
    return Status::InvalidArgument("select a file or subdirectory");
  return normalized;
}
namespace {
using Targets = std::map<std::string, const Json::Object *>;
StatusOr<Targets> TargetsOf(const Json &manifest) {
  auto status = ValidateWireType(manifest, "ProgramManifest");
  if (!status.ok())
    return status;
  Targets targets;
  for (const auto &target : *manifest.getAsObject()->getArray("targets")) {
    auto &o = *target.getAsObject();
    auto key = *o.getString("key");
    if (!targets.emplace(key, &o).second)
      return Status::InvalidArgument("duplicate program target");
    for (auto field : {"translation_units", "artifacts"})
      for (const auto &p : *o.getArray(field))
        if (!NormalizeReviewPath(*p.getAsString()).ok())
          return Status::InvalidArgument("invalid program member path");
  }
  return targets;
}
void SortSet(std::vector<std::string> &values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
}
} // namespace
StatusOr<std::vector<std::string>>
DeclaredProgramPaths(const Json &manifest, std::string_view target_key) {
  auto targets = TargetsOf(manifest);
  if (!targets.ok())
    return targets.status();
  std::vector<std::string> paths;
  std::set<std::string> seen;
  std::function<void(std::string)> visit = [&](std::string key) {
    if (!seen.insert(key).second)
      return;
    auto it = targets->find(key);
    if (it == targets->end())
      return;
    for (auto field : {"translation_units", "artifacts"})
      for (const auto &p : *it->second->getArray(field))
        paths.push_back(*NormalizeReviewPath(*p.getAsString()));
    for (const auto &dep : *it->second->getArray("dependencies"))
      visit(*dep.getAsString());
  };
  visit(std::string(target_key));
  SortSet(paths);
  return paths;
}
StatusOr<ProgramScope> ResolveProgramScope(const ProgramScopeInput &input) {
  ProgramScope out;
  if (!input.manifest) {
    for (const auto &[path, digest] : input.translation_unit_digests)
      out.member_tu_digests.push_back(digest);
    out.coverage = out.member_tu_digests.empty() ? "UNRESOLVED" : "OPEN";
    out.frontier = {"program membership requires an explicit declaration or "
                    "resolved build/link artifact"};
    out.target_digest = HashBytes("veritas.review.anonymous-target.v1");
  } else {
    if (input.membership_source != "EXPLICIT_DECLARATION" &&
        input.membership_source != "BUILD_LINK_ARTIFACT")
      return Status::InvalidArgument(
          "manifest requires declared or build/link membership");
    auto targets = TargetsOf(*input.manifest);
    if (!targets.ok())
      return targets.status();
    out.membership_source = input.membership_source;
    out.coverage = "DECLARED_CLOSED";
    out.assumptions = {
        "program membership is declared by the selected operator artifact; it "
        "is not a discovered whole-program proof"};
    std::set<std::string> visited;
    Json::Array reachable;
    std::function<void(std::string)> visit = [&](std::string key) {
      if (!visited.insert(key).second)
        return;
      auto it = targets->find(key);
      if (it == targets->end()) {
        out.frontier.push_back("target:" + key);
        return;
      }
      const auto &target = *it->second;
      Json::Object identity = target;
      for (auto field : {"translation_units", "artifacts", "dependencies"}) {
        std::vector<std::string> values;
        for (const auto &v : *target.getArray(field))
          values.push_back(*v.getAsString());
        SortSet(values);
        Json::Array sorted;
        for (const auto &v : values)
          sorted.push_back(v);
        identity[field] = std::move(sorted);
      }
      reachable.push_back(std::move(identity));
      for (const auto &p : *target.getArray("translation_units")) {
        auto path = *NormalizeReviewPath(*p.getAsString());
        auto tu = input.translation_unit_digests.find(path);
        if (tu == input.translation_unit_digests.end())
          out.frontier.push_back("translation-unit:" + path);
        else
          out.member_tu_digests.push_back(tu->second);
      }
      for (const auto &p : *target.getArray("artifacts")) {
        auto path = *NormalizeReviewPath(*p.getAsString());
        auto a = input.artifact_digests.find(path);
        if (a == input.artifact_digests.end())
          out.frontier.push_back("artifact:" + path);
        else
          out.dependency_artifact_digests.push_back(a->second);
      }
      for (const auto &dep : *target.getArray("dependencies"))
        visit(*dep.getAsString());
    };
    visit(input.target_key);
    std::sort(reachable.begin(), reachable.end(),
              [](const Json &a, const Json &b) {
                return *a.getAsObject()->getString("key") <
                       *b.getAsObject()->getString("key");
              });
    auto canonical =
        CanonicalJson(Json::Object{{"selected_target", input.target_key},
                                   {"targets", std::move(reachable)}});
    if (!canonical.ok())
      return canonical.status();
    out.target_digest = HashBytes(*canonical);
    if (!out.frontier.empty())
      out.coverage = "UNRESOLVED";
  }
  SortSet(out.member_tu_digests);
  SortSet(out.dependency_artifact_digests);
  SortSet(out.frontier);
  auto status = ValidateWireType(ToJson(out), "ProgramScope");
  if (!status.ok())
    return status;
  return out;
}
} // namespace veritas::review
