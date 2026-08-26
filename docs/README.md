# VERITAS Documentation

VERITAS documentation is organized by document class. Architecture documents
explain the durable system design, specifications define approved requirements,
plans describe implementation work and milestone sequencing, and developer
guides explain how to operate and extend the implemented system without
confusing approved target interfaces with commands that exist today.

## Primary Reading Order

1. [Platform architecture](architecture/01-platform-architecture.md) — system
   context, pipeline, and core principles.
2. [Whole-program analysis architecture](architecture/02-whole-program-analysis-architecture.md)
   — analysis engines and whole-program reasoning.
3. [SummaryDB storage architecture](architecture/03-summarydb-storage-architecture.md)
   — durable storage layers and backend responsibilities.
4. [Evidence IR architecture](architecture/04-evidence-ir-architecture.md) —
   the evidence language and its semantics.
5. [Engineering backbone specification](specs/veritas-engineering-backbone-design-specification.md)
   — cross-cutting invariants and milestone dependencies.
6. [Backbone milestone roadmap](plans/veritas-backbone-milestone-roadmap.md) —
   implementation sequence and current milestones.
7. [Developer guides and tutorials](guides/README.md) — generating a SummaryDB,
   extending SummaryDB and Evidence IR, and building analysis or Agent-facing
   tools.

## Document Classes

- [Architecture](architecture/README.md) contains ordered, durable system
  architecture.
- [Specifications](specs/README.md) contains cross-cutting and milestone
  design specifications.
- [Plans](plans/README.md) contains the roadmap, tooling plans, and executable
  milestone plans.
- [Guides and tutorials](guides/README.md) contains task-oriented manuals and
  worked examples. Each guide distinguishes the tested implementation from
  approved target contracts and open design work.

## Milestones

Use the [milestone specification matrix](specs/milestones/README.md) to find
the approved design for a milestone, and the [milestone plan matrix](plans/README.md)
to find its implementation plan and issue.
