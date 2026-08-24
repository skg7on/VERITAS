# M12 External-Provider Ingestion Plan Index

> **Status: Superseded compatibility index.** The former combined Joern/PhASAR
> `ExternalFact` plan has been removed. Do not execute this file as an
> implementation plan.

The approved M12 architecture is implemented through three independently
reviewable and testable plans:

1. [M12A SummaryDB External-Provider Substrate](m12a-summarydb-external-provider-substrate-implementation-plan.md)
   defines provider-neutral identities, graph/fact contracts, SummaryDB
   storage, atomic publication, history, snapshots, and scoped invalidation.
2. [M12B Joern GraphSON/GraphML Importer](m12b-joern-graphson-graphml-importer-implementation-plan.md)
   directly parses supported whole-graph Joern exports, validates context,
   normalizes identities/topology/memory/facts, and publishes through M12A.
3. [M12C Provider Fusion and Evidence Integration](m12c-provider-fusion-evidence-integration-implementation-plan.md)
   adds the unified pinned query view, non-destructive provider comparison,
   provider-aware M10B completion provenance, and M10C end-to-end tests.

All three implement the approved
[M12 Joern CPG SummaryDB Importer design specification](../../specs/milestones/m12-joern-cpg-summarydb-importer-design-spec.md).

M12D/PhASAR is not part of these plans. It depends on M12A but requires its own
detailed design and implementation plan because it imports analysis-result
facts rather than a Joern-style whole-program graph.
