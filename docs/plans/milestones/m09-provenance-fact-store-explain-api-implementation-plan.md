# M9 Provenance-Aware Fact Store and Explain API Implementation Plan

> **Status:** Delivered. The M9 provenance/fact-store/explain surface shipped as
> part of the M8R–M9 contract reconciliation (issue #109, PR #110) and is
> reconciled against `docs/specs/milestones/m09-provenance-fact-store-explain-api-design-spec.md`.
> This plan is retained as the historical milestone plan; the executable
> reconciliation plan is
> [`m08r-m09-contract-reconciliation-implementation-plan.md`](m08r-m09-contract-reconciliation-implementation-plan.md).

**Goal:** Store current and historical facts with their provenance DAGs, and expose a budgeted `veritas-explain` command.

**Architecture:** The production analyzer reduces a successful WPA run to one
canonical `AnalysisFactBatch` (content-addressed `BatchId`), validates it, and
publishes it through the `AnalysisFactBus` to a `FactStore` sink on the shared
SummaryDB metadata database. Facts carry a witness-independent semantic
`FactID`; the selected proof carries a witness-dependent `witness_id` so
distinct derivations of the same fact are retained. A schema-v4 receipt keyed by
`(run_id, batch_id)` makes redelivery idempotent.

**Tech Stack:** C++20, Protobuf, SQLite (schema v1–v4), GoogleTest.

**Spec:** `docs/specs/milestones/m09-provenance-fact-store-explain-api-design-spec.md`

## Delivered surface

| Component | Location | Notes |
| --- | --- | --- |
| Fact/provenance wire contract | `proto/veritas/fact/v1/fact.proto` | `Fact`, `ProvenanceGraph`, `ExplainBudget` |
| Schema v3 | `src/summarydb/schema/v3.sql` | `analysis_facts`, `run_fact_bindings`, `provenance_nodes`, `provenance_edges` |
| Schema v4 | `src/summarydb/schema/v4.sql` | `fact_batch_receipts (run_id, batch_id, wpa_run_id)` |
| Fact Store | `src/facts/FactStore.cpp` | `Publish` (atomic receipt + facts + bindings + witnesses) |
| Provenance Store | `src/facts/ProvenanceStore.cpp` | `Explain` (current binding, selected witness, retained alternatives) |
| Fact Bus | `src/facts/AnalysisFactBus.cpp` | `MakeAnalysisFactBatch`, `DeriveBatchId`, `Validate`, `Publish` |
| Explain CLI | `src/tools/veritas-explain.cpp` | `veritas-explain fact <id>` |

## Tests

- `FactStoreTest` — publication, current-binding history, idempotent redelivery, cross-run `FactID` sharing.
- `ProvenanceStoreTest` — budgeted explanation and current-binding selection.
- `AnalysisFactBusTest` — batch validation (batch-id recomputation, duplicate keys, witness closure, acyclicity).
- `ResultCanonicalizerTest` — witness.v2 derivation identity and rule arity.
- `VeritasExplainTest` — text and JSON explain output.
- `ProjectAnalyzerWpaTest` — end-to-end fact publication through the production analyzer.

The M9 entry gate (`tools/check_m9_entry.py`) asserts these members and all ten criteria pass from a clean build.
