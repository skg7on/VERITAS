# Documentation Layout Policy

This policy defines the canonical locations for documentation in this repository and overrides the default output paths of the `superpowers` skills.

## Canonical paths

| Document type | Location |
| --- | --- |
| Architecture documents | `docs/architecture/` |
| Design specifications | `docs/specs/` |
| Milestone design specs | `docs/specs/milestones/` |
| Implementation plans | `docs/plans/` |

## Rules for skill-generated documents

The `superpowers:brainstorming` and `superpowers:writing-plans` skills SHALL write their outputs to the canonical paths above, overriding their built-in defaults of `docs/superpowers/specs/` and `docs/superpowers/plans/`:

- `superpowers:brainstorming` writes the design spec to `docs/specs/`.
- `superpowers:writing-plans` writes the implementation plan to `docs/plans/`.

## Naming conventions

Use the repository's existing naming style (no `YYYY-MM-DD-` date prefixes):

- Design spec: `veritas-<topic>-design.md` (or `veritas-<topic>-design-specification.md`).
- Milestone design spec: `m<num>-<topic>-design-spec.md`.
- Implementation plan: `m<num>-<topic>-implementation-plan.md`.

## Invariants

- Do not create or populate `docs/superpowers/`.
- All specs live under `docs/specs/`; all plans live under `docs/plans/`.
