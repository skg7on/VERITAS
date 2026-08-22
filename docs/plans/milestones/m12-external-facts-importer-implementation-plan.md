# M12: External-Facts Importer — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Accept already-generated analysis results (Joern CPG export, PhASAR result) and map them into VERITAS's fact store as provenance-tagged, epistemic-lowered external observations.

**Architecture:** A separate importer stage — not part of the CPG projection, SVF, or summary pipeline — parses the external format, bridges external entities to VERITAS stable IDs where possible, normalizes predicates into VERITAS's fact vocabulary, and publishes `ExternalFact` records with `producer = joern|phasar` and an enforced `INFERRED`/`ASSUMED` epistemic floor.

**Tech Stack:** C++20, a minimal GraphML/JSON reader (private to `src/`), GoogleTest.

**Spec:** `docs/specs/milestones/m11-m12-summarydb-ingest-adapters-design-spec.md` (milestone-scoped signatures + tests)
**Architecture:** `docs/architecture/01-platform-architecture.md` (§6–§11 adapter tiers, invariant B10)

## Global Constraints

- External facts enter only as `INFERRED` or `ASSUMED`; the `MUST` state is unreachable from external input (backbone P8, `INFERRED → INFERRED`).
- No external fact silently becomes a `VERIFIED_FACT`; producer is `external` with `producer_id = joern|phasar`.
- External entities resolve to VERITAS IDs only via stable inputs (mangled name → `FunctionVariantID`, `file:line` → `SourceAnchorID`); unresolved entities get `external:<producer>:<id>`, never a fabricated VERITAS semantic ID.
- External facts are terminal facts: no WPA/SCC/invalidation participation; they are queryable and citable by Evidence Builder.
- Joern/PhASAR import is atomic — a parse or schema-version error publishes no partial facts.
- Public headers expose no Joern/PhASAR native types.
- This milestone depends on **M9** (`FactStore`, `ProvenanceStore`, `EpistemicState`); it is a forward plan executed after M9 lands.

---

## Files

- Create: `include/veritas/facts/external/ExternalFact.h`
- Create: `include/veritas/facts/external/ExternalProducer.h`
- Create: `include/veritas/facts/external/ExternalIdentityBridge.h`
- Create: `include/veritas/facts/external/JoernCpgImporter.h`
- Create: `include/veritas/facts/external/PhasarResultImporter.h`
- Create: `src/facts/external/ExternalIdentityBridge.cpp`
- Create: `src/facts/external/JoernCpgImporter.cpp`
- Create: `src/facts/external/PhasarResultImporter.cpp`
- Modify: `src/tools/veritas-build.cpp`  (add `import --joern <file>` / `import --phasar <file>`)
- Test: `tests/integration/facts/external/JoernCpgImporterTest.cpp`
- Test: `tests/integration/facts/external/PhasarResultImporterTest.cpp`
- Test: `tests/integration/facts/external/ExternalIdentityBridgeTest.cpp`
- Test: `tests/integration/facts/external/VeritasBuildImportCliTest.cpp`
- Test: `tests/fixtures/external/simple.graphml`, `tests/fixtures/external/simple.phasar.txt`

## Interfaces

```cpp
// include/veritas/facts/external/ExternalProducer.h
namespace veritas::facts::external {
enum class ExternalProducer { Joern, Phasar };
}
```

```cpp
// include/veritas/facts/external/ExternalFact.h
namespace veritas::facts::external {
struct ExternalFact {
  ExternalProducer producer;
  std::string external_id;          // Joern node ID / PhASAR fact ID
  core::StableId subject_id;        // VERITAS entity if resolved, else synthetic external:<prod>:<id>
  std::string predicate_kind;       // normalized VERITAS predicate vocabulary
  std::string predicate_canonical;
  facts::EpistemicState epistemic;  // always INFERRED or ASSUMED, never MUST
  std::optional<core::StableId> source_anchor_id;
  std::string raw_record;           // verbatim source record for provenance
};
}
```

```cpp
// include/veritas/facts/external/ExternalIdentityBridge.h
namespace veritas::facts::external {
class ExternalIdentityBridge {
 public:
  veritas::StatusOr<core::StableId> Resolve(
      ExternalProducer producer, const std::string& external_ref);
};
}
```

```cpp
// include/veritas/facts/external/JoernCpgImporter.h
namespace veritas::facts::external {
class JoernCpgImporter {
 public:
  veritas::StatusOr<std::vector<ExternalFact>> ImportGraphML(const std::filesystem::path&);
  veritas::StatusOr<std::vector<ExternalFact>> ImportJson(const std::filesystem::path&);
};
}

// include/veritas/facts/external/PhasarResultImporter.h
namespace veritas::facts::external {
class PhasarResultImporter {
 public:
  veritas::StatusOr<std::vector<ExternalFact>> Import(const std::filesystem::path&);
};
}
```

`facts::EpistemicState` (`Must, May, MustNot, Inferred, Assumed, Unknown`) is defined by M9. `FactStore::PublishFacts` and `ProvenanceStore::PutNode`/`PutEdge` are the M9 consumers.

## Implementation Plan

### Task 1: `ExternalFact` and `ExternalProducer`

- [ ] Create `ExternalProducer.h` and `ExternalFact.h` with the types above.
- [ ] Write a unit test that an `ExternalFact` can be constructed with `EpistemicState::Inferred` and rejects (in the validation path) `EpistemicState::Must`.
- [ ] Implement the validation helper that enforces the epistemic floor at construction/publish time.
- [ ] Run tests. Expected: PASS.
- [ ] Commit: `feat: add external fact type and epistemic floor`.

### Task 2: `ExternalIdentityBridge`

- [ ] Write tests: a mangled name that resolves to a known `FunctionVariantID` returns that ID; a `file:line` that resolves to a `SourceAnchorID` returns it; an unknown ref returns a synthetic `external:<producer>:<ref>` subject.
- [ ] Implement `Resolve` against M2 identity tables (mangled name → `FunctionVariantID`) and M4 source anchors (`file:line` → `SourceAnchorID`); fall back to the synthetic subject.
- [ ] Run tests. Expected: PASS.
- [ ] Commit: `feat: bridge external entities to VERITAS IDs`.

### Task 3: `JoernCpgImporter`

- [ ] Write tests: a GraphML fixture with a `CALL` edge → a `CallFact`-normalized `ExternalFact` with `producer=Joern`, `epistemic=Inferred`; a `REACHING_DEF` edge → `ValueFlowFact`-normalized; an unmappable predicate → opaque `external_observation` with `raw_record` retained.
- [ ] Implement GraphML and JSON parsing (private minimal reader); map Joern node/edge kinds to VERITAS predicate vocabulary via a fixed table; retain the raw record.
- [ ] Write a test that a malformed file returns `Status::InvalidArgument` and yields no facts (atomic).
- [ ] Run tests. Expected: PASS.
- [ ] Commit: `feat: import Joern CPG export as external facts`.

### Task 4: `PhasarResultImporter`

- [ ] Write tests: a PhASAR result fixture with an IFDS/IDE fact → normalized `ValueFlowFact` or opaque observation, `epistemic=Inferred`, `raw_record` retained; unknown schema version → `Status::InvalidArgument`.
- [ ] Implement the parser and normalization; retain verbatim records for provenance.
- [ ] Run tests. Expected: PASS.
- [ ] Commit: `feat: import PhASAR result as external facts`.

### Task 5: Publish via M9 `FactStore`/`ProvenanceStore`

- [ ] Write an integration test that imported facts are published through `FactStore::PublishFacts` with `producer_kind=external`, `producer_id=joern|phasar`, and a provenance node per fact.
- [ ] Implement the publish path; verify no external fact is promoted to `Must`/`VERIFIED_FACT`.
- [ ] Run tests. Expected: PASS.
- [ ] Commit: `feat: publish external facts with provenance`.

### Task 6: CLI `veritas-build import`

- [ ] Add `import --joern <file>` and `import --phasar <file>`; reject both flags together.
- [ ] Write a CLI test that `veritas-build import --joern tests/fixtures/external/simple.graphml` publishes facts and exits zero; malformed input exits nonzero with no partial publication.
- [ ] Run tests. Expected: PASS.
- [ ] Commit: `feat: add external fact import CLI`.

### Task 7: Re-scope M6 and register milestones

- [ ] Re-scope M6 §2 in `docs/specs/milestones/m06-thin-cpg-projection-design-spec.md`: the CPG *projection stage* accepts no artifacts; Joern/PhASAR arrive via the M12 importer as external facts (M6 still builds VERITAS's own CPG).
- [ ] Add M11/M12 to the milestone map in `docs/plans/veritas-backbone-milestone-roadmap.md` and to `docs/plans/README.md`.
- [ ] Commit: `docs: re-scope M6 inputs and register M11/M12`.

## Tests (required assertions)

```text
Joern CALL edge -> CallFact (producer=joern, epistemic=INFERRED)
PhASAR result -> ValueFlowFact / opaque observation, epistemic=INFERRED
identity bridge resolves mangled name -> FunctionVariantID
unresolvable entity -> synthetic external: ID
malformed file -> atomic failure, no partial facts
external fact never reaches MUST / never becomes VERIFIED_FACT
provenance records producer_kind=external, producer_id=joern|phasar
public headers expose no Joern/PhASAR native types (boundary scan)
importer is isolated from CPG projection and SVF stages
```

## Exit Criteria

```text
veritas-build import --joern <export> and --phasar <result> publish epistemic-lowered
  external facts into the M9 fact/provenance store.
External entities bridge to VERITAS IDs only via stable inputs, else synthetic IDs.
No external fact participates in WPA or is promoted past INFERRED/ASSUMED.
```
