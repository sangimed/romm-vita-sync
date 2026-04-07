# Documentation Policy

Documentation is part of the implementation and must remain consistent with the codebase at all times.

The codebase is the source of truth for current behavior.

Documentation that describes the present state of the project must reflect what is actually implemented in code, not what is merely intended, proposed, or assumed.

Both function-level comments and MkDocs project documentation must be updated whenever behavior changes.

Any change that makes documentation inaccurate or incomplete MUST include a documentation update in the same commit.

Outdated documentation is considered a bug.

## English-Only Policy

Everything committed to this repository MUST be written in English.

This rule applies to:

- MkDocs pages and README content
- code comments and function documentation
- variable names, struct fields, function names, and other identifiers
- user-facing UI strings and log messages
- sample configuration comments and developer notes

Do not introduce mixed-language content.

If existing non-English text is touched during a change, translate it to English in the same update.

## Current State vs Future Work

Current-state documentation must describe implemented behavior only.

Use present tense only for behavior, models, architecture, workflows, and constraints that already exist in the codebase.

If a concept is not implemented yet, it must be placed in a clearly labeled section such as:

- `Roadmap`
- `Planned`
- `Future Work`
- `Design Notes`
- `Brainstorming`

Future-facing sections must make the status explicit. They must not read as if the feature, model, or architecture already exists.

Do not document hypothetical structs, modules, adapters, pipelines, or storage layouts in current-state sections.

If a future idea is important to keep, preserve it in a separate section instead of mixing it into implementation documentation.

---

# Function Documentation Rules

Every non-trivial function must include a clear comment explaining:

- what the function does
- why it exists (if not obvious)
- important inputs
- return value
- side effects
- safety constraints
- assumptions or invariants

Comments must be:

- precise
- concise
- written in clear English
- focused on behavior (not implementation details)

Avoid:

- redundant comments
- vague descriptions
- outdated documentation

If a function changes behavior, its comment must be updated immediately.

---

# MkDocs Update Policy

MkDocs documentation is the source of truth for project concepts and architecture.

Within that documentation, implemented concepts must remain clearly separated from roadmap or brainstorming material.

Any change that affects documented behavior MUST update the corresponding MkDocs pages in the same commit.

README scope policy:

- `README.md` must remain high-level and non-technical.
- Technical setup, commands, workflows, configuration details, and troubleshooting must be documented in MkDocs (`docs/`), not in `README.md`.
- When adding or moving technical documentation, prefer updating existing MkDocs pages rather than expanding `README.md`.

This includes changes to:

- synchronization logic
- upload vs download decisions
- conflict resolution rules
- save formats (VMP, SRM, MCD, etc.)
- backup strategy
- slot handling
- configuration structure
- filesystem layout
- UI workflow
- CLI behavior
- RomM integration
- supported platforms
- storage locations
- API interactions

If documentation exists for a concept, it must never become stale.

When in doubt: update MkDocs.

When docs and code diverge, either:

1. update the documentation to match the current code, or
2. implement the missing behavior before documenting it as current

Never leave speculative design written as present-day implementation.

---

# Synchronization-Specific Requirement

Functions related to save synchronization must explicitly document:

- upload vs download decisions
- comparison logic between local and remote saves
- conflict resolution behavior
- overwrite or restore operations
- backup creation or usage
- VMP / SRM manipulation rules

These behaviors must never remain implicit.

They must also remain aligned with the MkDocs synchronization documentation.

---

# Contributor Rules

When adding or modifying code:

1. write or update function comments if behavior is non-trivial
2. keep comments aligned with implementation
3. update MkDocs documentation when a documented concept changes
4. never leave outdated documentation behind
5. prioritize clarity over verbosity
6. treat documentation drift as a defect to fix immediately

---

# Build Policy

The agent MUST NOT build or package the VPK.

Building the VPK is a manual user responsibility and is intentionally excluded from automated workflows.

Specifically, the agent must NOT:

- run VPK build commands
- trigger packaging scripts
- invoke VitaSDK packaging steps
- modify build artifacts
- upload generated `.vpk` files
- attempt deployment to a PS Vita device

The agent may:

- modify source code
- update documentation
- adjust build configuration files when necessary
- improve compilation reliability
- fix build errors reported by the user

But the agent must never execute the final packaging step.

If testing requires a VPK build, the agent must stop before packaging and let the user perform it manually.
