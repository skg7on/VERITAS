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

#include "veritas/facts/SouffleExporter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <compare>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace veritas::facts {
namespace {

struct RelationFile {
  FactRelation relation;
  std::string_view name;
};

constexpr std::array kBaseRelationFiles{
    RelationFile{FactRelation::kDirectCall, "DirectCall.facts"},
    RelationFile{FactRelation::kDirectRead, "DirectRead.facts"},
    RelationFile{FactRelation::kDirectWrite, "DirectWrite.facts"},
    RelationFile{FactRelation::kLocalFlow, "LocalFlow.facts"},
    RelationFile{FactRelation::kMayAlias, "MayAlias.facts"},
};

bool IsBaseRelation(FactRelation relation) {
  return std::ranges::any_of(kBaseRelationFiles, [relation](const auto &file) {
    return file.relation == relation;
  });
}

bool SameTuple(const FactTuple &left, const FactTuple &right) {
  return left.relation == right.relation && left.columns == right.columns &&
         left.epistemic == right.epistemic && left.rule_id == right.rule_id &&
         left.input_tuple_ids == right.input_tuple_ids;
}

std::string TemporarySuffix() {
  static std::atomic<unsigned long long> sequence{0};
  const auto ticks =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return ".veritas-tmp-" + std::to_string(ticks) + "-" +
         std::to_string(sequence.fetch_add(1));
}

void RemovePaths(const std::vector<std::filesystem::path> &paths) {
  for (const auto &path : paths) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
}

Status RestoreBackups(
    const std::vector<std::pair<std::filesystem::path, std::filesystem::path>>
        &backups) {
  for (auto it = backups.rbegin(); it != backups.rend(); ++it) {
    std::error_code error;
    std::filesystem::rename(it->second, it->first, error);
    if (error) {
      return Status::Internal(
          "failed to restore prior Souffle relation file: " + error.message());
    }
  }
  return Status::Ok();
}

struct SemanticColumnsKey {
  FactRelation relation;
  std::vector<std::string> columns;

  auto operator<=>(const SemanticColumnsKey &) const = default;
};

struct SemanticKey {
  FactRelation relation;
  std::vector<std::string> columns;
  summary::v1::EpistemicState epistemic;

  auto operator<=>(const SemanticKey &) const = default;
};

StatusOr<std::vector<std::string>>
ParseDerivedRow(std::string_view relation_name, std::size_t line_number,
                const std::string &row) {
  const std::string prefix = std::string(relation_name) + " line " +
                             std::to_string(line_number) + ": ";
  if (row.find('\r') != std::string::npos) {
    return Status::InvalidArgument(prefix + "embedded CR is not allowed");
  }
  if (std::ranges::count(row, '\t') != 2) {
    return Status::InvalidArgument(prefix +
                                   "expected exactly 3 tab-separated fields");
  }
  std::array<std::string, 3> fields;
  std::size_t field = 0;
  std::size_t start = 0;
  while (field < fields.size()) {
    const std::size_t separator = row.find('\t', start);
    const std::size_t end =
        separator == std::string::npos ? row.size() : separator;
    fields[field++] = row.substr(start, end - start);
    if (separator == std::string::npos)
      break;
    start = separator + 1;
  }
  if (field != fields.size()) {
    return Status::InvalidArgument(prefix +
                                   "expected exactly 3 tab-separated fields");
  }
  if (fields[0].empty() || fields[1].empty()) {
    return Status::InvalidArgument(prefix +
                                   "semantic columns must not be empty");
  }
  int epistemic = 0;
  const auto parsed = std::from_chars(
      fields[2].data(), fields[2].data() + fields[2].size(), epistemic);
  if (parsed.ec != std::errc{} ||
      parsed.ptr != fields[2].data() + fields[2].size() ||
      (epistemic != static_cast<int>(summary::v1::EPISTEMIC_STATE_MUST) &&
       epistemic != static_cast<int>(summary::v1::EPISTEMIC_STATE_MAY))) {
    return Status::InvalidArgument(prefix + "unsupported epistemic value");
  }
  fields[2] = std::to_string(epistemic);
  return std::vector<std::string>{std::move(fields[0]), std::move(fields[1]),
                                  std::move(fields[2])};
}

Status ReadDerivedFile(
    const std::filesystem::path &path, FactRelation relation,
    std::map<SemanticColumnsKey, summary::v1::EpistemicState> *rows) {
  auto relation_name = FactRelationName(relation);
  if (!relation_name.ok())
    return relation_name.status();
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return Status::NotFound("missing Souffle derived relation file: " +
                            path.string());
  }
  std::string row;
  std::size_t line_number = 0;
  while (std::getline(input, row)) {
    ++line_number;
    auto fields = ParseDerivedRow(*relation_name, line_number, row);
    if (!fields.ok())
      return fields.status();
    const int epistemic =
        (*fields)[2] == "1"
            ? static_cast<int>(summary::v1::EPISTEMIC_STATE_MUST)
            : static_cast<int>(summary::v1::EPISTEMIC_STATE_MAY);
    SemanticColumnsKey key{
        .relation = relation,
        .columns = {std::move((*fields)[0]), std::move((*fields)[1])}};
    auto [it, inserted] = rows->emplace(
        std::move(key), static_cast<summary::v1::EpistemicState>(epistemic));
    if (!inserted) {
      auto weakened = WeakenPositiveEpistemic(
          it->second, static_cast<summary::v1::EpistemicState>(epistemic));
      if (!weakened.ok())
        return weakened.status();
      it->second = *weakened;
    }
  }
  if (input.bad()) {
    return Status::Internal("failed to read Souffle derived relation file: " +
                            path.string());
  }
  return Status::Ok();
}

struct ProofCandidate {
  std::string_view rule_id;
  std::vector<core::StableId> inputs;
};

class ProofReconstructor {
public:
  ProofReconstructor(
      std::span<const FactTuple> base_facts,
      const std::map<SemanticColumnsKey, summary::v1::EpistemicState> &rows)
      : base_facts_(base_facts), rows_(rows) {}

  StatusOr<std::optional<FactTuple>> Reconstruct(const SemanticKey &key) {
    if (auto found = memo_.find(key); found != memo_.end()) {
      return std::optional<FactTuple>{found->second};
    }
    if (!active_.insert(key).second)
      return std::optional<FactTuple>{};

    std::vector<ProofCandidate> direct;
    std::vector<ProofCandidate> transitive;
    auto candidates = key.relation == FactRelation::kReachableCall
                          ? ReachableCandidates(key, &direct, &transitive)
                          : MayWriteCandidates(key, &direct, &transitive);
    if (!candidates.ok()) {
      active_.erase(key);
      return candidates;
    }
    auto &selected_kind = direct.empty() ? transitive : direct;
    if (selected_kind.empty()) {
      active_.erase(key);
      return std::optional<FactTuple>{};
    }
    std::ranges::sort(selected_kind, {}, &ProofCandidate::inputs);
    auto derived = MakeDerivedFact(key.relation, key.columns, key.epistemic,
                                   std::string(selected_kind.front().rule_id),
                                   selected_kind.front().inputs);
    active_.erase(key);
    if (!derived.ok())
      return derived.status();
    auto [it, inserted] = memo_.emplace(key, std::move(*derived));
    return std::optional<FactTuple>{it->second};
  }

private:
  Status ReachableCandidates(const SemanticKey &key,
                             std::vector<ProofCandidate> *direct,
                             std::vector<ProofCandidate> *transitive) {
    for (const auto &fact : base_facts_) {
      if (fact.relation != FactRelation::kDirectCall ||
          fact.columns[0] != key.columns[0]) {
        continue;
      }
      if (fact.columns[1] == key.columns[1] &&
          fact.epistemic == key.epistemic) {
        direct->push_back(
            {.rule_id = "m8.reachable.direct.v1", .inputs = {fact.tuple_id}});
      }
      SemanticColumnsKey sub_columns{
          .relation = FactRelation::kReachableCall,
          .columns = {fact.columns[1], key.columns[1]}};
      auto sub_state = rows_.find(sub_columns);
      if (sub_state == rows_.end())
        continue;
      SemanticKey sub_key{.relation = sub_columns.relation,
                          .columns = sub_columns.columns,
                          .epistemic = sub_state->second};
      auto subproof = Reconstruct(sub_key);
      if (!subproof.ok())
        return subproof.status();
      if (!subproof->has_value())
        continue;
      auto epistemic =
          WeakenPositiveEpistemic(fact.epistemic, (*subproof)->epistemic);
      if (!epistemic.ok())
        return epistemic.status();
      if (*epistemic != key.epistemic)
        continue;
      std::vector<core::StableId> inputs{fact.tuple_id, (*subproof)->tuple_id};
      std::ranges::sort(inputs);
      transitive->push_back({.rule_id = "m8.reachable.transitive.v1",
                             .inputs = std::move(inputs)});
    }
    return Status::Ok();
  }

  Status MayWriteCandidates(const SemanticKey &key,
                            std::vector<ProofCandidate> *direct,
                            std::vector<ProofCandidate> *transitive) {
    for (const auto &fact : base_facts_) {
      if (fact.relation == FactRelation::kDirectWrite &&
          fact.columns == key.columns && fact.epistemic == key.epistemic) {
        direct->push_back(
            {.rule_id = "m8.may_write.direct.v1", .inputs = {fact.tuple_id}});
      }
      if (fact.relation != FactRelation::kDirectCall ||
          fact.columns[0] != key.columns[0]) {
        continue;
      }
      SemanticColumnsKey sub_columns{
          .relation = FactRelation::kMayWrite,
          .columns = {fact.columns[1], key.columns[1]}};
      auto sub_state = rows_.find(sub_columns);
      if (sub_state == rows_.end())
        continue;
      SemanticKey sub_key{.relation = sub_columns.relation,
                          .columns = sub_columns.columns,
                          .epistemic = sub_state->second};
      auto subproof = Reconstruct(sub_key);
      if (!subproof.ok())
        return subproof.status();
      if (!subproof->has_value())
        continue;
      auto epistemic =
          WeakenPositiveEpistemic(fact.epistemic, (*subproof)->epistemic);
      if (!epistemic.ok())
        return epistemic.status();
      if (*epistemic != key.epistemic)
        continue;
      std::vector<core::StableId> inputs{fact.tuple_id, (*subproof)->tuple_id};
      std::ranges::sort(inputs);
      transitive->push_back({.rule_id = "m8.may_write.transitive.v1",
                             .inputs = std::move(inputs)});
    }
    return Status::Ok();
  }

  std::span<const FactTuple> base_facts_;
  const std::map<SemanticColumnsKey, summary::v1::EpistemicState> &rows_;
  std::map<SemanticKey, FactTuple> memo_;
  std::set<SemanticKey> active_;
};

} // namespace

Status
SouffleExporter::WriteBaseRelations(const std::filesystem::path &directory,
                                    std::span<const FactTuple> facts) {
  std::map<std::string, const FactTuple *> tuples_by_id;
  std::map<FactRelation, std::vector<const FactTuple *>> grouped;
  for (const auto &fact : facts) {
    auto valid = ValidateFactTuple(fact);
    if (!valid.ok())
      return valid;
    const std::string tuple_id = core::ToString(fact.tuple_id);
    auto [it, inserted] = tuples_by_id.emplace(tuple_id, &fact);
    if (!inserted && !SameTuple(*it->second, fact)) {
      return Status::InvalidArgument("conflicting duplicate fact tuple ID");
    }
    if (inserted && IsBaseRelation(fact.relation)) {
      grouped[fact.relation].push_back(&fact);
    }
  }
  for (auto &[relation, relation_facts] : grouped) {
    std::ranges::sort(relation_facts, {},
                      [](const FactTuple *fact) { return fact->tuple_id; });
  }

  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    return Status::Internal("failed to create Souffle relation directory: " +
                            error.message());
  }

  const std::string suffix = TemporarySuffix();
  std::vector<std::filesystem::path> temporary_paths;
  temporary_paths.reserve(kBaseRelationFiles.size());
  for (const auto &relation_file : kBaseRelationFiles) {
    const auto final_path = directory / relation_file.name;
    const auto temporary_path = final_path.string() + suffix;
    temporary_paths.emplace_back(temporary_path);
    std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
    if (!output) {
      RemovePaths(temporary_paths);
      return Status::Internal("failed to open temporary Souffle relation file");
    }
    for (const FactTuple *fact : grouped[relation_file.relation]) {
      output << core::ToString(fact->tuple_id);
      for (const auto &column : fact->columns)
        output << '\t' << column;
      output << '\t' << static_cast<int>(fact->epistemic) << '\n';
    }
    output.close();
    if (!output) {
      RemovePaths(temporary_paths);
      return Status::Internal(
          "failed to write temporary Souffle relation file");
    }
  }

  std::vector<std::pair<std::filesystem::path, std::filesystem::path>> backups;
  backups.reserve(kBaseRelationFiles.size());
  for (const auto &relation_file : kBaseRelationFiles) {
    const auto final_path = directory / relation_file.name;
    if (!std::filesystem::exists(final_path, error)) {
      if (error) {
        RemovePaths(temporary_paths);
        auto restored = RestoreBackups(backups);
        return restored.ok() ? Status::Internal(
                                   "failed to inspect Souffle relation file: " +
                                   error.message())
                             : restored;
      }
      continue;
    }
    const auto backup_path = final_path.string() + suffix + ".backup";
    std::filesystem::rename(final_path, backup_path, error);
    if (error) {
      RemovePaths(temporary_paths);
      auto restored = RestoreBackups(backups);
      return restored.ok()
                 ? Status::Internal("failed to stage Souffle relation file: " +
                                    error.message())
                 : restored;
    }
    backups.emplace_back(final_path, backup_path);
  }

  std::vector<std::filesystem::path> installed;
  for (std::size_t index = 0; index < kBaseRelationFiles.size(); ++index) {
    const auto final_path = directory / kBaseRelationFiles[index].name;
    std::filesystem::rename(temporary_paths[index], final_path, error);
    if (error) {
      RemovePaths(temporary_paths);
      RemovePaths(installed);
      auto restored = RestoreBackups(backups);
      return restored.ok() ? Status::Internal(
                                 "failed to publish Souffle relation file: " +
                                 error.message())
                           : restored;
    }
    installed.push_back(final_path);
  }
  std::vector<std::filesystem::path> backup_paths;
  backup_paths.reserve(backups.size());
  for (const auto &[final_path, backup_path] : backups) {
    backup_paths.push_back(backup_path);
  }
  RemovePaths(backup_paths);
  return Status::Ok();
}

StatusOr<std::vector<FactTuple>>
SouffleExporter::ReadDerivedRelations(const std::filesystem::path &directory,
                                      std::span<const FactTuple> base_facts) {
  std::map<std::string, const FactTuple *> tuples_by_id;
  for (const auto &fact : base_facts) {
    auto valid = ValidateFactTuple(fact);
    if (!valid.ok())
      return valid;
    if (!IsBaseRelation(fact.relation)) {
      return Status::InvalidArgument(
          "Souffle provenance reconstruction requires base facts");
    }
    auto [it, inserted] =
        tuples_by_id.emplace(core::ToString(fact.tuple_id), &fact);
    if (!inserted && !SameTuple(*it->second, fact)) {
      return Status::InvalidArgument("conflicting duplicate fact tuple ID");
    }
  }

  std::map<SemanticColumnsKey, summary::v1::EpistemicState> rows;
  auto status = ReadDerivedFile(directory / "ReachableCall.csv",
                                FactRelation::kReachableCall, &rows);
  if (!status.ok())
    return status;
  status = ReadDerivedFile(directory / "MayWrite.csv", FactRelation::kMayWrite,
                           &rows);
  if (!status.ok())
    return status;

  ProofReconstructor reconstructor(base_facts, rows);
  std::vector<FactTuple> derived;
  derived.reserve(rows.size());
  for (const auto &[columns, epistemic] : rows) {
    SemanticKey key{.relation = columns.relation,
                    .columns = columns.columns,
                    .epistemic = epistemic};
    auto proof = reconstructor.Reconstruct(key);
    if (!proof.ok())
      return proof.status();
    if (!proof->has_value()) {
      auto relation_name = FactRelationName(columns.relation);
      if (!relation_name.ok())
        return relation_name.status();
      return Status::FailedPrecondition(
          "no acyclic provenance proof for " + std::string(*relation_name) +
          "(" + columns.columns[0] + ", " + columns.columns[1] + ")");
    }
    derived.push_back(std::move(**proof));
  }
  std::ranges::sort(derived, {}, &FactTuple::tuple_id);
  return derived;
}

} // namespace veritas::facts
