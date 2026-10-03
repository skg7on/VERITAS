# VERITAS Architecture

Start with architecture 06 for the proposed review product direction. Documents
01–05 describe the platform, analysis, storage, evidence and identity capabilities
that support it.

1. [01 Platform architecture](01-platform-architecture.md) — defines the
   end-to-end VERITAS pipeline, principles, and input boundaries.
2. [02 Whole-program analysis architecture](02-whole-program-analysis-architecture.md)
   — defines the analysis stack, recursive reasoning, and engine roles.
3. [03 SummaryDB storage architecture](03-summarydb-storage-architecture.md)
   — defines the physical storage layers and pluggable backend contracts.
4. [04 Evidence IR architecture](04-evidence-ir-architecture.md) — defines the
   typed, provenance-preserving evidence language and its semantics.
5. [05 Portable analysis target and identity architecture](05-portable-analysis-target-identity-architecture.md)
   — defines canonical target resolution, compiler injection, portable identity,
   and non-destructive migration from host-scoped identities.

6. [06 Review Agent architecture](06-review-agent-architecture.md) — proposed
   product goal, bounded context, dual candidate discovery and selective analysis.

For current product direction, read 06 first, then the capability documents as
needed. Earlier target pipelines are not claims that every capability is shipped.
