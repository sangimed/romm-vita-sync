# Project context

This repository contains a PlayStation Vita homebrew application built with VitaSDK.

Agents must keep changes compatible with VitaSDK, vita2d, PS Vita system modules, controller input, and the constraints of running on real PS Vita hardware.

# UI principles

The UI is designed for handheld console use on the PS Vita screen resolution.

- Layouts must remain readable on the physical PS Vita display.
- Text must use sizes that remain legible at handheld distance.
- Long text must wrap or ellipsize correctly inside its UI container.
- Text must not overlap controls, borders, status areas, or adjacent content.
- UI changes must prioritize clarity, predictable focus state, and controller ergonomics over dense desktop-style layouts.

# Interaction constraints

D-pad navigation is required for every interactive UI flow.

- Every screen, modal, list, and actionable control must be reachable and usable with D-pad controls.
- Touch input may be supported as an enhancement, but it must never be required.
- Controller confirm/cancel behavior must respect the active PS Vita button assignment where applicable.
- Logs and long viewports must be scrollable with controller input.
- Focus state must be visible enough to use on the PS Vita screen without touch.

# Sync behavior expectations

Synchronization behavior must remain explicit and conservative.

- Token-based authentication is preferred over username/password authentication.
- Username/password authentication may remain available as a fallback where supported.
- The UI includes a dry-run sync mode; changes to sync actions must preserve its meaning and visibility.
- Sync changes must preserve backup, conflict, upload/download, and overwrite safety expectations.
- Any change that affects sync behavior must keep user-facing status and log output accurate.

# Documentation policy

Documentation is part of the implementation.

- Update documentation whenever behavior changes.
- Keep current-state documentation aligned with implemented behavior.
- Function comments must describe behavior, important inputs, outputs or return values, side effects, and constraints.
- Existing function comments must not be removed unless they become incorrect.
- Incorrect comments must be updated in the same change that makes them inaccurate.

# Agent rules

Agents must keep changes narrow and Vita-compatible.

- Do not modify build scripts unless explicitly requested.
- Do not build or package the VPK unless explicitly requested.
- Do not introduce dependencies or runtime assumptions that are incompatible with VitaSDK.
- Prefer existing project patterns, modules, and UI helpers.
- Preserve controller-first interaction behavior.
- Keep user-facing strings, comments, and documentation in English.
- Do not remove safety checks, backups, conflict handling, logging, or dry-run behavior without explicit instruction.

## Agent skills

### Issue tracker

Issues and PRDs are tracked in GitHub Issues for `sangimed/romm-vita-sync`. See `docs/agents/issue-tracker.md`.

### Triage labels

Use the default triage label vocabulary: `needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, and `wontfix`. See `docs/agents/triage-labels.md`.

### Domain docs

This is a single-context repo: read root `CONTEXT.md` when present and repo ADRs under `docs/adr/` when relevant. See `docs/agents/domain.md`.
