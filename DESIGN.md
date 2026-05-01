# Design.md

## Product context

This project is a PlayStation Vita homebrew application built with VitaSDK.

The product helps users synchronize local PS Vita save data with a RomM server. The UI must make configuration, authentication, sync selection, dry-run review, progress, results, logs, and errors understandable on a handheld console.

## Target device constraints

The target device is the PlayStation Vita.

- The screen is small and used at handheld distance.
- Interaction is controller-first.
- D-pad navigation is required for all interactive elements.
- Touch input is optional only and must never be required.
- Screen space is limited.
- Readable text is more important than dense UI.
- Layouts must fit the PS Vita resolution without overlapping text, controls, or status areas.

## Design principles

- Prefer clarity over visual complexity.
- Use console-style navigation patterns.
- Keep screens minimal and task-focused.
- Use strong visual hierarchy for title, primary action, selection, status, and secondary details.
- Avoid desktop-like layouts with too many columns, tiny labels, or dense controls.
- Avoid tiny text and tiny buttons.
- Do not rely only on color for status; include text labels or clear state indicators.
- Long text must wrap or ellipsize correctly inside its container.
- Prefer predictable placement over decorative composition.

## Navigation rules

- Every interactive element must be reachable with D-pad controls.
- Focus order must match the visible layout.
- Primary actions should be obvious and easy to reach.
- Cancel and back behavior must be predictable on every screen and popup.
- Destructive or overwrite actions require confirmation.
- Logs and long content must be scrollable with controller input.
- Touch scrolling may be added as a convenience, but controller scrolling must remain available.

## Screens and layout expectations

Authentication screens must clearly distinguish token-based authentication from username/password fallback. Token entry should be the preferred path.

Server configuration must show the current server address, authentication state, and save platform in readable rows. Missing required settings must be visible before sync starts.

The sync screen must show selected games, target count, readiness, and the primary sync action. Users must be able to understand what will be uploaded or downloaded before destructive sync actions.

Dry-run mode must be visible and understandable. It must read as a safe preview mode, not as a background implementation detail.

Sync results must summarize success, warning, error, skipped, upload, and download outcomes in short readable text.

Logs and errors must be shown in scrollable panels. Log text must wrap within the panel and remain usable with controller input.

Confirmation popups must use concise copy, clear confirm/cancel labels, and predictable focus. Destructive confirmations must state what is affected.

## Sync UX expectations

- Token-based authentication is preferred over username/password.
- Dry-run mode must be visible and understandable before sync runs.
- Sync status must clearly distinguish success, warning, error, and skipped operations.
- Error messages must be short but actionable.
- Users should understand what will be uploaded or downloaded before destructive sync actions.
- Conflict, overwrite, backup, upload, and download behavior must be surfaced accurately in status text and logs.

## Visual style

The visual direction is a clean handheld-console UI.

- Use compact but readable panels.
- Keep spacing consistent.
- Use restrained colors.
- Maintain high contrast between text and background.
- Avoid unnecessary decoration.
- Avoid clutter and dense desktop-style information layouts.
- Use visual accents to guide focus, not to compete with content.

## Component guidelines

Buttons must be large enough for clear focus and readable labels. Primary buttons should be visually stronger than secondary buttons.

Toggles must show both current value and focus state. Binary settings should not depend on color alone.

Lists must use readable row heights, clear selected state, and clear checked or active state. Long item labels must ellipsize or wrap inside the row without overlapping metadata.

Log panels must wrap long lines and support controller scrolling. Auto-scroll behavior must not prevent manual review.

Popups must be concise, centered, readable, and controller-operable. Confirm and cancel actions must be explicit.

Status badges must include readable text and a visible state marker. They must not rely on color alone.

Error messages must explain the problem and the next useful action when possible.

## Agent instructions

- Follow this file for all UI and UX decisions.
- Do not introduce UI patterns that break PS Vita usability.
- Do not require touch input.
- Do not build the VPK unless explicitly requested.
- Do not modify build scripts unless explicitly requested.
- Update this file when a UI behavior or design rule changes.
