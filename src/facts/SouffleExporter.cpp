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
#include <queue>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
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

// One recursive M8 domain. A derived relation is proved directly by its base
// relation stating the same fact, or transitively by composing a call edge
// with a proof of the remainder. Both M8 domains share that shape; only the
// base relation and the rule ids differ, so they belong in a table rather
// than in branches. Rule ids are the durable M8 v1 identities and must not
// change: a derived fact's tuple ID is hashed over its rule and inputs.
struct DerivationDomain {
  FactRelation derived;
  FactRelation base;
  std::string_view direct_rule;
  std::string_view transitive_rule;
};

constexpr std::array kDerivationDomains{
    DerivationDomain{FactRelation::kReachableCall, FactRelation::kDirectCall,
                     "m8.reachable.direct.v1", "m8.reachable.transitive.v1"},
    DerivationDomain{FactRelation::kMayWrite, FactRelation::kDirectWrite,
                     "m8.may_write.direct.v1", "m8.may_write.transitive.v1"},
};

// Returns nullptr for a relation this reconstructor does not derive.
const DerivationDomain *DomainFor(FactRelation derived) {
  const auto it = std::ranges::find(kDerivationDomains, derived,
                                    &DerivationDomain::derived);
  return it == kDerivationDomains.end() ? nullptr : &*it;
}

struct ProofCandidate {
  std::string_view rule_id;
  std::vector<core::StableId> inputs;
  std::size_t rank = 0;
};

struct DeferredProofCandidate {
  SemanticKey target;
  std::string_view rule_id;
  core::StableId base_input;
};

struct QueuedProofCandidate {
  SemanticKey target;
  ProofCandidate proof;
};

struct QueuedProofCandidateGreater {
  bool operator()(const QueuedProofCandidate &left,
                  const QueuedProofCandidate &right) const {
    return std::tie(left.proof.rank, left.target, left.proof.inputs,
                    left.proof.rule_id) >
           std::tie(right.proof.rank, right.target, right.proof.inputs,
                    right.proof.rule_id);
  }
};

class ProofReconstructor {
public:
  ProofReconstructor(
      std::span<const FactTuple> base_facts,
      const std::map<SemanticColumnsKey, summary::v1::EpistemicState> &rows,
      std::span<const FactTuple> derived_support = {})
      : base_facts_(base_facts), rows_(rows),
        derived_support_(derived_support) {}

  StatusOr<std::optional<FactTuple>> Reconstruct(const SemanticKey &key) {
    auto built = BuildProofs();
    if (!built.ok())
      return built;
    auto found = memo_.find(key);
    if (found == memo_.end())
      return std::optional<FactTuple>{};
    return std::optional<FactTuple>{found->second};
  }

  StatusOr<std::vector<FactTuple>>
  Closure(std::span<const SemanticKey> targets) {
    auto built = BuildProofs();
    if (!built.ok())
      return built;

    std::map<core::StableId, const FactTuple *> proofs_by_id;
    for (const auto &[key, fact] : memo_) {
      static_cast<void>(key);
      proofs_by_id.emplace(fact.tuple_id, &fact);
    }

    std::set<core::StableId> selected;
    std::vector<core::StableId> pending;
    for (const auto &target : targets) {
      auto found = memo_.find(target);
      if (found == memo_.end())
        continue;
      if (selected.insert(found->second.tuple_id).second)
        pending.push_back(found->second.tuple_id);
    }
    while (!pending.empty()) {
      const auto tuple_id = pending.back();
      pending.pop_back();
      const auto proof = proofs_by_id.find(tuple_id);
      if (proof == proofs_by_id.end())
        continue;
      for (const auto &input : proof->second->input_tuple_ids) {
        if (proofs_by_id.contains(input) && selected.insert(input).second)
          pending.push_back(input);
      }
    }

    std::vector<FactTuple> closure;
    closure.reserve(selected.size());
    for (const auto &tuple_id : selected)
      closure.push_back(*proofs_by_id.find(tuple_id)->second);
    return closure;
  }

private:
  Status BuildProofs() {
    if (built_)
      return Status::Ok();

    constexpr std::array positive_states{summary::v1::EPISTEMIC_STATE_MUST,
                                         summary::v1::EPISTEMIC_STATE_MAY};
    std::set<SemanticKey> proof_keys;
    for (const auto &[columns, final_epistemic] : rows_) {
      static_cast<void>(final_epistemic);
      for (const auto epistemic : positive_states)
        proof_keys.insert({columns.relation, columns.columns, epistemic});
    }

    // Transitive derivation always walks call edges, whichever domain is
    // being proved, so calls stay indexed by caller. Direct derivation reads
    // whichever base relation the domain declares, indexed by the exact
    // columns it must match.
    std::map<std::string, std::vector<const FactTuple *>> calls_by_source;
    std::map<SemanticColumnsKey, std::vector<const FactTuple *>>
        base_by_columns;
    for (const auto &fact : base_facts_) {
      if (fact.relation == FactRelation::kDirectCall)
        calls_by_source[fact.columns[0]].push_back(&fact);
      base_by_columns[{fact.relation, fact.columns}].push_back(&fact);
    }
    std::map<SemanticColumnsKey, std::vector<const FactTuple *>>
        support_by_columns;
    for (const auto &support : derived_support_) {
      support_by_columns[{support.relation, support.columns}].push_back(
          &support);
    }

    std::priority_queue<QueuedProofCandidate, std::vector<QueuedProofCandidate>,
                        QueuedProofCandidateGreater>
        ready;
    auto enqueue = [&](const SemanticKey &target, std::string_view rule_id,
                       std::vector<core::StableId> inputs, std::size_t rank) {
      std::ranges::sort(inputs);
      ready.push({target, {rule_id, std::move(inputs), rank}});
    };

    std::map<SemanticKey, std::vector<DeferredProofCandidate>> dependents;
    for (const auto &key : proof_keys) {
      const DerivationDomain *domain = DomainFor(key.relation);
      if (domain == nullptr)
        continue;

      // Direct: the domain's base relation stating exactly this fact. Both
      // domains share this shape -- a DirectCall from f to g directly proves
      // ReachableCall(f, g), just as a DirectWrite proves MayWrite.
      auto directs = base_by_columns.find({domain->base, key.columns});
      if (directs != base_by_columns.end()) {
        for (const auto *base : directs->second) {
          if (base->epistemic == key.epistemic) {
            enqueue(key, domain->direct_rule, {base->tuple_id}, 0u);
          }
        }
      }

      auto calls = calls_by_source.find(key.columns[0]);
      if (calls == calls_by_source.end())
        continue;
      for (const auto *call : calls->second) {
        SemanticColumnsKey sub_columns{
            .relation = key.relation,
            .columns = {call->columns[1], key.columns[1]}};
        const std::string_view transitive_rule = domain->transitive_rule;
        auto supports = support_by_columns.find(sub_columns);
        if (supports != support_by_columns.end()) {
          for (const auto *support : supports->second) {
            auto epistemic =
                WeakenPositiveEpistemic(call->epistemic, support->epistemic);
            if (!epistemic.ok())
              return epistemic.status();
            if (*epistemic == key.epistemic) {
              enqueue(key, transitive_rule, {call->tuple_id, support->tuple_id},
                      1u);
            }
          }
        }
        if (rows_.contains(sub_columns)) {
          for (const auto sub_epistemic : positive_states) {
            SemanticKey sub_key{.relation = sub_columns.relation,
                                .columns = sub_columns.columns,
                                .epistemic = sub_epistemic};
            auto epistemic =
                WeakenPositiveEpistemic(call->epistemic, sub_epistemic);
            if (!epistemic.ok())
              return epistemic.status();
            if (*epistemic == key.epistemic && proof_keys.contains(sub_key)) {
              dependents[sub_key].push_back(
                  {key, transitive_rule, call->tuple_id});
            }
          }
        }
      }
    }

    while (!ready.empty()) {
      QueuedProofCandidate candidate = ready.top();
      ready.pop();
      if (memo_.contains(candidate.target))
        continue;
      auto derived = MakeDerivedFact(
          candidate.target.relation, candidate.target.columns,
          candidate.target.epistemic, std::string(candidate.proof.rule_id),
          candidate.proof.inputs);
      if (!derived.ok())
        return derived.status();
      auto [position, inserted] =
          memo_.emplace(candidate.target, std::move(*derived));
      if (!inserted)
        continue;
      auto dependent = dependents.find(candidate.target);
      if (dependent == dependents.end())
        continue;
      for (const auto &next : dependent->second) {
        enqueue(next.target, next.rule_id,
                {next.base_input, position->second.tuple_id},
                candidate.proof.rank + 1u);
      }
    }

    auto closure = ValidateClosure();
    if (!closure.ok())
      return closure;
    built_ = true;
    return Status::Ok();
  }

  Status ValidateClosure() const {
    std::set<core::StableId> available;
    for (const auto &fact : base_facts_)
      available.insert(fact.tuple_id);
    for (const auto &fact : derived_support_)
      available.insert(fact.tuple_id);
    for (const auto &[key, fact] : memo_) {
      static_cast<void>(key);
      available.insert(fact.tuple_id);
    }
    for (const auto &[key, fact] : memo_) {
      static_cast<void>(key);
      if (std::ranges::any_of(fact.input_tuple_ids, [&](const auto &input) {
            return !available.contains(input);
          })) {
        return Status::FailedPrecondition(
            "canonical provenance proof has unavailable immediate input");
      }
    }
    return Status::Ok();
  }

  std::span<const FactTuple> base_facts_;
  const std::map<SemanticColumnsKey, summary::v1::EpistemicState> &rows_;
  std::span<const FactTuple> derived_support_;
  std::map<SemanticKey, FactTuple> memo_;
  bool built_ = false;
};

StatusOr<std::vector<FactTuple>> ReconstructRows(
    std::span<const FactTuple> base_facts,
    const std::map<SemanticColumnsKey, summary::v1::EpistemicState> &rows) {
  ProofReconstructor reconstructor(base_facts, rows);
  std::vector<SemanticKey> targets;
  targets.reserve(rows.size());
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
    targets.push_back(std::move(key));
  }
  return reconstructor.Closure(targets);
}

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

  return ReconstructRows(base_facts, rows);
}

StatusOr<std::vector<FactTuple>> SouffleExporter::ReconstructCanonicalProofs(
    std::span<const FactTuple> base_facts,
    std::span<const FactTuple> derived_semantics,
    std::span<const FactTuple> derived_support) {
  std::map<std::string, const FactTuple *> tuples_by_id;
  for (const auto &fact : base_facts) {
    auto valid = ValidateFactTuple(fact);
    if (!valid.ok())
      return valid;
    if (!IsBaseRelation(fact.relation)) {
      return Status::InvalidArgument(
          "canonical proof reconstruction requires base facts");
    }
    auto [position, inserted] =
        tuples_by_id.emplace(core::ToString(fact.tuple_id), &fact);
    if (!inserted && !SameTuple(*position->second, fact)) {
      return Status::InvalidArgument("conflicting duplicate fact tuple ID");
    }
  }
  for (const auto &fact : derived_support) {
    auto valid = ValidateFactTuple(fact);
    if (!valid.ok())
      return valid;
    if (DomainFor(fact.relation) == nullptr) {
      return Status::InvalidArgument(
          "canonical proof support requires a derived M8 relation");
    }
    auto [position, inserted] =
        tuples_by_id.emplace(core::ToString(fact.tuple_id), &fact);
    if (!inserted && !SameTuple(*position->second, fact)) {
      return Status::InvalidArgument("conflicting duplicate fact tuple ID");
    }
  }
  std::map<SemanticColumnsKey, summary::v1::EpistemicState> rows;
  for (const auto &fact : derived_semantics) {
    auto valid = ValidateFactTuple(fact);
    if (!valid.ok())
      return valid;
    if (DomainFor(fact.relation) == nullptr) {
      return Status::InvalidArgument(
          "canonical proof reconstruction requires a derived M8 relation");
    }
    SemanticColumnsKey key{.relation = fact.relation, .columns = fact.columns};
    auto [position, inserted] = rows.emplace(std::move(key), fact.epistemic);
    if (!inserted) {
      auto weakened = WeakenPositiveEpistemic(position->second, fact.epistemic);
      if (!weakened.ok())
        return weakened.status();
      position->second = *weakened;
    }
  }
  ProofReconstructor reconstructor(base_facts, rows, derived_support);
  std::vector<SemanticKey> targets;
  targets.reserve(rows.size());
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
    targets.push_back(std::move(key));
  }
  return reconstructor.Closure(targets);
}

} // namespace veritas::facts
