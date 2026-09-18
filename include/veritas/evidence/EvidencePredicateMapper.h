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

// EvidencePredicateMapper.h — the versioned M9/M10A relation to EIR predicate
// lowering.
//
// One M9 semantic fact becomes one `eir.v1` `Fact`. The mapper is a *lowering*,
// not an analysis: it reads the typed cells of a fact that already exists and
// writes the EIR predicate that states the same thing. It never queries a
// backend, never runs an analyzer, never reconstructs a derivation, and never
// reaches the FactStore, the ProvenanceStore, or the CPG.
//
// TWO PROPERTIES ARE LOAD-BEARING.
//
//   * It copies; it does not strengthen. The epistemic state of the mapped fact
//     is the epistemic state of the M9 row, one-for-one. An `kInferred` M9 fact
//     leaves this function as `kInferred`; nothing here promotes a MAY to a
//     MUST, because promotion is the verifier's authority, not the assembler's
//     (CLAUDE.md P8, and the M10C design spec's "map typed facts without
//     epistemic strengthening").
//
//   * It refuses rather than drops. A relation the table does not know, a row
//     whose arity disagrees with the registry, and a cell whose type disagrees
//     with its column are all `InvalidArgument`. A fact that cannot be mapped
//     is never silently omitted from the case: §5 of the design spec makes
//     "unsupported relation names fail as UNSUPPORTED" the contract, so an
//     unmappable fact is a typed failure the caller must resolve.
//
// ENTITY REFERENCES ARE RESOLVED THROUGH A TABLE, NOT A STRING.
//
// The EIR predicates below name case-local handles (`@value`, `@left`, `@to`),
// and one M9 fact can name two entities (`alias(@left, @right)`,
// `reads(@function, @memory)`). A mapper that took a single local-id string
// could not express those relations at all — the arity, not the convenience,
// is the reason `MapFact` takes a resolver. `StableIdResolver` maps a stable
// `core::StableId` to the case-local handle that declares it; the builder
// populates one before mapping, so the mapper resolves every reference the
// relation carries, however many there are.

#ifndef VERITAS_EVIDENCE_EVIDENCE_PREDICATE_MAPPER_H_
#define VERITAS_EVIDENCE_EVIDENCE_PREDICATE_MAPPER_H_

#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "veritas/core/Ids.h"
#include "veritas/core/Status.h"
#include "veritas/evidence/EvidenceCase.h"
#include "veritas/facts/AnalysisFact.h"

namespace veritas::evidence {

// Maps a stable VERITAS identity to the case-local handle that declares it.
// Resolution is by canonical `core::ToString` text, so the lookup is stable and
// independent of how the caller collected the entities.
class StableIdResolver {
 public:
  virtual ~StableIdResolver() = default;

  // The case-local handle for `stable_id`, or `std::nullopt` when the case
  // declares no member for it. The mapper treats `std::nullopt` as a typed
  // failure: a referenced entity the case cannot name is a defect in the
  // caller's entity assignment, not a reason to drop the reference.
  virtual std::optional<std::string> Resolve(
      const core::StableId& stable_id) const = 0;
};

// The resolver the builder owns: a flat stable-id-text to handle table.
class LocalIdTable : public StableIdResolver {
 public:
  void Bind(const core::StableId& stable_id, std::string local_id);
  std::optional<std::string> Resolve(
      const core::StableId& stable_id) const override;

 private:
  std::map<std::string, std::string> by_text_;
};

// The per-fact inputs the caller owns: the handle the case declares the fact
// under, and the handle of the provenance record that justifies it.
struct FactMappingHandle {
  std::string local_id;
  std::string provenance_id;
};

class EvidencePredicateMapper {
 public:
  // Lowers one M9/M10A semantic fact into an `eir.v1` fact.
  //
  // `fact`'s relation selects the built-in mapping; its cells are read through
  // the column contract of that relation, and every entity cell is resolved
  // through `resolver`. `handle` supplies the two case-local handles the mapper
  // cannot invent.
  //
  // The mapped fact always carries `stable_id` = `fact.fact_id`, so the case
  // keeps the cross-run identity of the M9 fact it was lowered from.
  //
  // Fails with `InvalidArgument` when the relation is not in the built-in
  // table, when the row's cell count disagrees with the relation registry, when
  // a cell holds a value of the wrong type for its column, when the row's
  // epistemic state cannot be read, or when an entity reference has no
  // case-local handle. Never fails silently and never drops a fact.
  StatusOr<Fact> MapFact(const facts::AnalysisFact& fact,
                         const StableIdResolver& resolver,
                         const FactMappingHandle& handle) const;

  // The EIR predicate name `relation` lowers to (`range`, `capacity`,
  // `reachable`, `alias`, `reads`, `writes`), or `std::nullopt` when the
  // relation has no built-in mapping. Exposed so a caller can test
  // supportability without constructing a fact.
  static std::optional<std::string_view> PredicateName(
      facts::RelationId relation);

  // The stable producer identity a mapped fact records for `relation`
  // (`analysis.value_range`, `analysis.memory_effect`, ...). Every spelling is
  // a `QualifiedId`, so a mapped fact's producer is always EIR-T-writable.
  static std::optional<std::string_view> ProducerName(
      facts::RelationId relation);

 private:
  // One focused method per built-in relation. Each reads exactly the cells its
  // relation's registry entry declares and builds one predicate.
  StatusOr<Fact> MapRange(const facts::AnalysisFact& fact,
                          const StableIdResolver& resolver,
                          const FactMappingHandle& handle) const;
  StatusOr<Fact> MapCapacity(const facts::AnalysisFact& fact,
                             const StableIdResolver& resolver,
                             const FactMappingHandle& handle) const;
  StatusOr<Fact> MapReachability(const facts::AnalysisFact& fact,
                                 const StableIdResolver& resolver,
                                 const FactMappingHandle& handle) const;
  StatusOr<Fact> MapAlias(const facts::AnalysisFact& fact,
                          const StableIdResolver& resolver,
                          const FactMappingHandle& handle) const;
  StatusOr<Fact> MapMemoryEffect(const facts::AnalysisFact& fact,
                                 const StableIdResolver& resolver,
                                 const FactMappingHandle& handle) const;

  // The shared tail: reads the epistemic cell, copies it, and assembles the
  // `Fact`. `predicate` is already built by the relation-specific method.
  StatusOr<Fact> Assemble(const facts::AnalysisFact& fact,
                          const FactMappingHandle& handle,
                          Expression predicate) const;
};

// Copies an M9/M10A epistemic state to its EIR counterpart. Total and
// one-for-one: the two families name the same six states, so no value is
// widened, strengthened, or lost.
EpistemicState CopyEpistemicState(analysis::semantic::EpistemicState state);

// The EIR confidence a mapped fact records for `state`. M9 semantic rows carry
// no confidence column, so M10C classifies the strength the epistemic state
// already justifies — never more. `kMust`/`kMustNot` are exact, a `kMay` is
// medium, an inference or an assumption is low, and an `kUnknown` stays
// unknown. This table is a lowering, not an upgrade: it cannot produce a
// confidence stronger than the state it is given.
Confidence ConfidenceForState(EpistemicState state);

}  // namespace veritas::evidence

#endif  // VERITAS_EVIDENCE_EVIDENCE_PREDICATE_MAPPER_H_
