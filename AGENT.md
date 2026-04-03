# Documentation Policy

Documentation is part of the implementation and must stay consistent with the codebase at all times.

Both function-level comments and project-level Markdown documentation must be updated whenever behavior changes.

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

If a function changes behavior, its comment must be updated in the same change.

---

# Markdown Documentation Update Policy

Markdown documentation must always reflect the current implementation.

If a change modifies an already documented concept, the documentation must be updated in the same commit.

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

Outdated documentation is not acceptable.

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

---

# Contributor Rules

When adding or modifying code:

1. write or update function comments if behavior is non-trivial
2. keep comments aligned with implementation
3. update Markdown documentation when a documented concept changes
4. never leave outdated documentation behind
5. prioritize clarity over verbosity