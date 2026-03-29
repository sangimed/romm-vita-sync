# Function Documentation Policy

Every function must include a clear and precise comment describing its purpose and behavior.

The objective is not to add verbose documentation, but to make each function immediately understandable for a future maintainer.

## Mandatory rules

Each function comment must explain, when relevant:

- what the function does
- why it exists if the intent is not obvious
- the meaning of important inputs
- the returned value
- important side effects
- important safety constraints
- assumptions or invariants relied upon by the function

## Style expectations

Function comments must be:

- precise
- concise
- easy to read
- written in clear English
- focused on behavior, not line-by-line implementation details

Avoid:

- redundant comments that only restate the function name
- overly long comments
- vague comments such as "Handle sync" or "Process data"
- outdated comments that no longer match the code

## Update policy

Function comments must be reviewed whenever a function is modified.

If the behavior, purpose, inputs, outputs, side effects, or constraints change, the comment must be updated in the same change.

It is not acceptable to modify a function and leave an inaccurate or incomplete comment behind.

If a modification does not change the documented behavior, the existing comment may remain unchanged.

## Minimum expectation

No non-trivial function should be left without a meaningful comment.

Small obvious helpers may use shorter comments, but any function related to synchronization, conversion, matching, backup, conflict resolution, or RomM communication must be documented clearly.

## Synchronization-specific requirement

For functions related to save synchronization, comments must explicitly clarify the most important safety and decision rules, especially when the function:

- decides whether to upload or download
- compares local and remote save state
- handles conflict resolution
- manipulates VMP or SRM data
- performs restore or overwrite-related operations
- creates or uses backups

## Contributor rule

When adding or editing code:

1. write or update the function comment first if the function behavior is not trivial
2. keep the comment aligned with the implementation
3. prefer one accurate comment over multiple vague comments