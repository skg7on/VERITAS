# Documentation Layout Policy

This policy defines the canonical locations for documentation in this
repository and overrides the default output paths of the `superpowers` skills.

## Canonical Paths

| Document type | Canonical filename pattern |
| --- | --- |
| Architecture documents | `docs/architecture/NN-topic-slug-architecture.md` |
| Cross-cutting design specs | `docs/specs/topic-slug-design-spec.md` |
| Milestone design specs | `docs/specs/milestones/mNN-or-mNNsuffix-topic-slug-design-spec.md` |
| Project/tooling plans | `docs/plans/topic-slug-implementation-plan.md` |
| Milestone plans | `docs/plans/milestones/mNN-or-mNNsuffix-topic-slug-implementation-plan.md` |

## Rules for Skill-Generated Documents

The `superpowers:brainstorming` and `superpowers:writing-plans` skills SHALL
write their outputs to the canonical paths above:

- `superpowers:brainstorming` writes a design spec to `docs/specs/`.
- `superpowers:writing-plans` writes an implementation plan to `docs/plans/`.

## Naming Conventions

Use zero-padded milestone numbers below ten. Preserve approved suffixes such as
`m08r` and `m10b`. Do not use date prefixes.

## Invariants

- Forbidden: `docs/superpowers/`.
- All specs live under `docs/specs/`; all plans live under `docs/plans/`.
- Required indexes: `docs/README.md` plus `README.md` in `docs/architecture/`,
  `docs/specs/`, `docs/specs/milestones/`, and `docs/plans/`.
