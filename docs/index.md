# romm-vita-sync

<p align="center">
    <img src="assets/logo.png" alt="RoMM Vita Sync logo" width="420">
</p>

Save synchronization client for RomM on PlayStation Vita, supporting native and emulated platforms.

## Overview

**romm-vita-sync** is a PlayStation Vita homebrew application designed to synchronize game save data with a RomM server.

The long-term objective is cross-platform save synchronization across:

- PS Vita native games
- PSP (Adrenaline)
- PS1 (POPS via Adrenaline)
- RetroArch and other emulator environments

The first milestone targets PS1 save synchronization using Adrenaline virtual memory cards.

The first implementation targets a manual synchronization workflow executed from a dedicated homebrew application.
An optional startup auto-sync path is also available in-app (`[Sync].auto_sync_on_startup = true`) and uses persistent on-screen progress feedback.
Automatic trigger integration from system plugin events remains planned for a later stage.

## Project Status

**Stage:** early development

This project is in early development. Back up your save data before installation or any manipulation.

Documentation scope:

- unless a section is explicitly labeled `Planned`, `Roadmap`, `Future Work`, or `Design Notes`, it describes the current codebase
- ideas and possible future refactors are kept separate from implemented behavior on purpose
- when documentation and code disagree, the code is the reference for current behavior until both are brought back in sync

Initial milestone includes:

- Detect PS1 save folders under `ux0:/pspemu/PSP/SAVEDATA`
- Parse `PARAM.SFO`
- Detect `SCEVMC0.VMP` and `SCEVMC1.VMP`
- Display metadata (path, size, timestamp)
- Convert `.VMP` → `.SRM`
- Convert `.SRM` → `.VMP`
- Compare local vs remote saves
- Manual synchronization workflow
- Synchronization is triggered manually from the application UI in this phase, with optional startup auto-sync (`auto_sync_on_startup`)
- Automatic backup before overwrite
- Zero destructive operations without confirmation

Current integration status:

- real HTTP device registration is implemented (`POST /api/devices`)
- upload/download conversion logic is integrated in `SyncEngine`
- real HTTP save transfer callbacks are wired (`list/upload/download`)
- local Vita saves are now mapped to RomM `rom_id` by resolving the platform via `/api/platforms`, then querying `/api/roms` with targeted `search_term` requests only


## Execution Model (Version 1)

romm-vita-sync is implemented first as a standalone PS Vita homebrew application.

Synchronization is explicitly triggered by the user from within the application UI.

This approach ensures:

- safe validation of save conversion logic
- deterministic synchronization behavior
- easier debugging during early development
- reduced risk of unintended save overwrites

Automatic synchronization based on system events (for example after exiting a game) will be introduced later through a taiHEN plugin once the synchronization engine is fully validated.

## Installation & Quick Start

This section summarizes the practical setup for both end users and developers.

### End User Prerequisites

- PlayStation Vita with homebrew support
- VitaShell (or equivalent method to install VPK files)
- Adrenaline installed (for PS1 save support)
- Network access from Vita to your RomM server
- Valid RomM username/password credentials

### Developer Build Prerequisites (Recommended: Docker)

- Docker Engine / Docker Desktop installed
- Docker daemon running
- Git with submodules support

### Host Toolchain Prerequisites (Alternative)

- VitaSDK toolchain
- CMake
- Git with submodules support
- `curl` (or `curl.exe` on Windows)

### Upload Target Configuration (Vita FTP)

The build/upload helpers read optional FTP settings from:

`tools/build-and-upload-vpk.local.env`

Create it from the sample template:

- Linux/macOS: `cp tools/build-and-upload-vpk.config.sample.env tools/build-and-upload-vpk.local.env`
- Windows PowerShell: `Copy-Item tools/build-and-upload-vpk.config.sample.env tools/build-and-upload-vpk.local.env`

Supported keys:

- `FTP_HOST`: Vita IP address (for example `192.168.1.20`)
- `FTP_PORT`: VitaShell FTP port (default `1337`)
- `FTP_REMOTE_DIR`: remote destination directory (default `ux0:/homebrews`)

CLI arguments still override values from the local env file.

### Recommended Build + Upload (Docker One-Command)

1. `git submodule update --init --recursive`
2. Linux/macOS: `./tools/docker-build-and-upload-vpk.sh`
3. Windows PowerShell: `./tools/docker-build-and-upload-vpk.ps1`

The FTP upload step uses an explicit timeout policy:

- connection timeout: `10s`
- upload stall timeout: `10s` (if speed drops below `1 B/s`)
- container lifecycle: `--rm` (container auto-stopped and removed when script exits)

### Host Toolchain Alternative

Use this method if you prefer a native VitaSDK setup on the host machine.

Commands:

1. `git submodule update --init --recursive`
2. Linux/macOS: `./tools/build-and-upload-vpk.sh`
3. Windows PowerShell: `./tools/build-and-upload-vpk.ps1`

### First Launch

1. Launch the app on Vita
2. The app automatically creates `ux0:data/romm-vita-sync/`
3. Select the connection fields and enter the RomM `url`, `username`, and `password`
4. Optionally toggle `Dry-run mode` before the first sync
5. Confirm each field saves immediately after closing the system keyboard

### First Sync

1. Ensure Vita can reach the RomM URL
2. In the game list, choose a detected PS1 game
3. Return to `Synchronize Selected Game` and press `X`
4. Follow progress in the sync modal for manual runs; automatic startup sync updates the header state and `Synchronize` panel status without opening a modal

Notes:

- The RomM URL, username, and password are currently stored in plain text (no encryption) in `settings.ini`.
- Default `dry_run = true`; you can disable it directly from the home screen or set `[Sync].dry_run = false` in `settings.ini` to execute real transfers.
- Default `auto_apply_conflicts = false`; you can enable it from the home screen or set `[Sync].auto_apply_conflicts = true` in `settings.ini` to apply recommended conflict actions automatically.
- When `auto_apply_conflicts = false`, manual conflict review shows which Vita button currently confirms the action (`X` or `O`, depending on the system enter-button setting). Declining the prompt skips that save, except for same-timestamp conflicts where the app offers upload first and then a download fallback.

## Configuration (`settings.ini`)

Connection and sync runtime options are read from:

`ux0:data/romm-vita-sync/settings.ini`

Format is INI-style (conventional in C/C++ desktop and embedded projects).

The app now creates `ux0:data/romm-vita-sync/` automatically on startup.
You can configure the URL, username, password, dry-run mode, and conflict auto-apply mode directly in the in-app UI; manual file creation is not required.

Reference template (optional, for manual editing/debug):

`samples/settings.ini.example`

Supported sections:

- `[RomM]`: `url`, `username`, `password`, `platform`, `emulator`, `verify_tls`, `timeout_seconds`
- `[Device]`: `device_id`, `device_name`, `device_platform`, `client`, `client_version`
- `[Sync]`: `state_store_path`, `backup_directory`, `dry_run`, `auto_apply_conflicts`, `auto_sync_on_startup`
- `[Log]`: `level` (`error|warn|info|debug`), `file_enabled` (`true|false`), `scan_verbose` (`true|false`)

Security notice:

- The RomM URL, username, and password are currently stored locally in `settings.ini` **without encryption** (plain text).
- This is intentional for the current milestone and will be hardened in a later iteration.

Authentication rule:

- `username` and `password` are required for all authenticated RomM requests
- if `[Device].device_id` is empty, startup calls the `RommClient` registration flow and persists the returned `device_id` into `settings.ini`
- recommended logging for troubleshooting: `level=debug` and keep `scan_verbose=false` first; enable `scan_verbose=true` only when debugging scanner issues
- file logging remains available only through `settings.ini`; the home screen no longer exposes a dedicated file logging row
- when enabled manually, file logging path is fixed to `ux0:data/romm-vita-sync/romm-vita-sync.log` with 3 rotated files max (`.log`, `.log.1`, `.log.2`), 10MB each

Current in-app UI behavior:

- The home screen exposes dedicated fields for the RomM `url`, `username`, and `password`.
- The home screen includes a `Dry-run mode` toggle with immediate save.
- The home screen includes an `Auto-apply conflicts` toggle with immediate save.
- Editing those fields opens the official PS Vita system keyboard (`SceImeDialog`).
- Confirming keyboard input persists values immediately to `ux0:data/romm-vita-sync/settings.ini`.
- The main screen keeps a visible primary `Synchronize Selected Game` action plus secondary `Synchronize All Saves` and `Rescan Local Saves` actions.
- The current sync target is selected from the detected PS1 game list and remains highlighted even when focus moves back to the sync button.
- The main screen uses three larger panels (`Connection`, `Synchronize`, `Detected PS1 Games`) instead of a separate sync-activity side panel.
- Connection values wrap inside their rows, and long single-line labels such as game-list entries and footer status text ellipsize to stay inside their bounds.
- Manual sync runs now open a blocking modal with a wider layout, wrapped text, a real progress bar, and live scrolling logs.
- While a manual sync is running, the modal cannot be closed; once complete, it shows success/failure and can be closed manually.
- Manual sync logs support held `UP/DOWN` scrolling after the viewport leaves auto-follow mode, and they can also be dragged with the front touchscreen.
- When `Auto-apply conflicts` is enabled, recommended conflict actions execute without opening a confirmation dialog, including during startup auto-sync.
- When `Auto-apply conflicts` is disabled, manual conflict dialogs show the active Vita confirm/decline button mapping and state whether declining skips the save or opens the alternate action prompt.
- Automatic startup sync keeps using the same sync pipeline, but its live state is now reflected through the header status and `Synchronize` panel messaging on the home screen.
- The current UI uses a sober single-screen Vita layout with wider panels, clearer spacing, controller navigation, and sharper text placement.

The sync engine now treats server `409 Conflict` responses as synchronization conflicts (remote newer) rather than generic transfer errors, aligned with `romm-retroarch-sync` behavior.

## Test End-to-End (Current Milestone)

Coverage:

- local PS1 save scan
- config loading/persistence
- real device registration via `POST /api/devices`
- per-game sync triggering from the Vita UI
- visible sync/log output on-device
- in-app conversion path wiring (`VMP->SRM` for upload, `SRM->VMP` + signing for download)
- real save transfer integration (`GET /api/saves`, `POST /api/saves`, `GET /api/saves/{id}/content`)

### 1. Build And Upload VPK

Docker one-command method (recommended):

1. `git submodule update --init --recursive`
2. Create `tools/build-and-upload-vpk.local.env` from `tools/build-and-upload-vpk.config.sample.env` and set `FTP_HOST` (Vita IP), optional `FTP_PORT`, and optional `FTP_REMOTE_DIR`
3. Linux/macOS: `./tools/docker-build-and-upload-vpk.sh`
4. Windows PowerShell: `./tools/docker-build-and-upload-vpk.ps1`

Host toolchain alternative:

1. Ensure VitaSDK/CMake/curl are installed on the host
2. `git submodule update --init --recursive`
3. Linux/macOS: `./tools/build-and-upload-vpk.sh`
4. Windows PowerShell: `./tools/build-and-upload-vpk.ps1`

### 2. First Launch Setup

1. Launch the app on Vita.
2. Confirm the app creates `ux0:data/romm-vita-sync/`.
3. Select each connection field and enter the RomM `url`, `username`, and `password`.
4. Confirm that each field saves immediately after closing the system keyboard.
5. Toggle `Dry-run mode` directly from the home screen if you want to execute real transfers immediately.
6. Toggle `Auto-apply conflicts` if you want recommended conflict actions to run without confirmation prompts.

### 3. Network And Auth Preconditions

1. Ensure Vita can reach the RomM URL on the same network.
2. Use valid RomM username/password credentials.
3. For self-signed HTTPS certificates, use `verify_tls = false` only for local testing.

### 4. Sync Validation

1. Move through the detected PS1 game list to choose the current sync target.
2. Return to `Synchronize Selected Game` and press `X`.
3. Confirm manual sync shows a blocking progress modal with live logs and completion state.
4. Confirm automatic startup sync (when enabled) updates the header state and `Synchronize` panel without opening a modal.
5. On first successful authenticated sync, confirm `device_id` is persisted in `settings.ini`.

### 5. Persistence Validation

1. Restart the app.
2. Confirm URL/username/password are still present and both `Dry-run mode` and `Auto-apply conflicts` keep their saved values.
3. Confirm `device_id` remains stable across launches.

### 6. Negative Path Validation

1. Invalid username/password: expect `authentication failed`.
2. Unreachable URL/network issue: expect `network error`.
3. HTTPS with invalid cert and `verify_tls = true`: expect request failure.

### 7. `dry_run` Validation

1. Default is `dry_run = true` for safety-first runs.
2. Set `[Sync].dry_run = false` in `settings.ini` to execute real transfers.
3. Upload flow sends `.SRM` through `POST /api/saves`; download flow pulls `GET /api/saves/{id}/content` then rebuilds/signs `.VMP`.

### 8. `auto_apply_conflicts` Validation

1. Default is `auto_apply_conflicts = false` so conflict actions require explicit confirmation during manual sync.
2. Set `[Sync].auto_apply_conflicts = true` in `settings.ini` or enable the home-screen toggle to apply recommended conflict actions automatically.
3. Confirm startup auto-sync no longer defers `local_newer` and `remote_newer` conflicts when this setting is enabled.

## Why This Project Exists

RomM already supports save synchronization for several platforms and clients (for example Android launchers). However, there is currently no native PS Vita sync client.

This project aims to provide:

- reliable save backup
- bidirectional synchronization with RomM
- conflict-safe restore workflows
- compatibility with Adrenaline-based PS1 saves

## Scope (Version 1)

Version 1 focuses exclusively on PS1 saves via Adrenaline.

Features:

- Detect save folders in `ux0:/pspemu/PSP/SAVEDATA/<GAME_ID>`
- Parse `PARAM.SFO`
- Detect `.VMP` memory card files
- Convert `.VMP` → `.SRM`
- Convert `.SRM` → `.VMP`
- Support manual upload/download workflow orchestration in `SyncEngine` with real RomM HTTP transport
- Always create backups before overwrite

## PS1 Save Architecture

### Local PS Vita / Adrenaline Layout

PS1 saves are stored inside:

`ux0:/pspemu/PSP/SAVEDATA/<GAME_ID>/`

Typical contents:

- `PARAM.SFO`
- `ICON0.PNG`
- `CONFIG.BIN`
- `SCEVMC0.VMP`
- `SCEVMC1.VMP`

File roles:

| File | Purpose |
|------|---------|
| PARAM.SFO | Sony metadata |
| ICON0.PNG | Save icon |
| CONFIG.BIN | POPS configuration |
| SCEVMC0.VMP | Memory card slot 1 |
| SCEVMC1.VMP | Memory card slot 2 |

Adrenaline / POPS stores PS1 saves as virtual memory card containers.

### RomM / Emulator-Side Layout

RomM stores PS1 saves as `.SRM` files:

`.../saves/psx/<rom_id>/pcsx_rearmed/<game>.srm`

RomM does not store `.VMP` containers directly. Instead, it uses emulator-facing raw save formats.

### VMP vs SRM Relationship

Observed sizes:

- `SCEVMC0.VMP = 131200 bytes`
- `SAVE.SRM = 131072 bytes`
- Difference: `128 bytes`

Working model:

- VMP = 128-byte header + raw memory card data
- SRM = raw memory card data

Conversion strategy:

| Direction | Operation |
|-----------|-----------|
| Vita → RomM | Remove first 128 bytes |
| RomM → Vita | Prepend valid VMP header |

## Save Models (Current Implementation)

The current PS1 implementation does not use a single `LocalPs1Save` struct. The codebase uses three concrete representations instead, depending on the stage of the pipeline.

### Scanner Inventory (`SaveItem`)

Defined in `src/save_item.h`.

```
SaveItem
├─ game_id
├─ game_title
├─ path
├─ size_bytes
└─ timestamp
```

Notes:

- one `SaveItem` represents one detected `.VMP` file, not one aggregated save directory
- `game_id` is extracted from the parent directory name
- `game_title` is read from `PARAM.SFO` when available
- `path` points directly to the detected `.VMP` file
- `timestamp` is stored as a formatted local string (`YYYY-MM-DD HH:MM:SS`)

### Sync Inventory (`SyncSaveDescriptor`)

Defined in `src/sync_types.h`.

```
SyncSaveDescriptor
├─ rom_id
├─ remote_id
├─ game_id
├─ title
├─ filename
├─ path
├─ remote_path
├─ size_bytes
├─ timestamp_unix
├─ hash
├─ origin_device
├─ device_is_current
├─ device_is_untracked
└─ slot
```

Notes:

- local scan results are converted from `SaveItem` into `SyncSaveDescriptor` by `src/scan_to_sync_adapter.c`
- remote saves fetched from RomM are also parsed into `SyncSaveDescriptor`
- this shared descriptor is the canonical synchronization model currently used by `SyncEngine`
- `slot` is inferred from `SCEVMC0.VMP` or `SCEVMC1.VMP`

### UI Game Aggregation (`UiGameEntry`)

Defined in `src/main.c`.

```
UiGameEntry
├─ key
├─ game_id
├─ title
└─ save_count
```

Notes:

- the UI groups multiple `SyncSaveDescriptor` items by game to render the selectable PS1 game list
- `save_count` is the number of detected memory-card files associated with that game

### Design Notes (Not Implemented Yet)

The current codebase does not yet expose:

- a dedicated directory-level `LocalPs1Save` model with `save_dir`, `icon_path`, `config_path`, `vmc0_path`, and `vmc1_path`
- a dedicated `RemotePs1Save` struct separate from `SyncSaveDescriptor`
- a dedicated `CanonicalPs1MemoryCard` struct holding parsed raw card data plus metadata

Those ideas may still be useful later if the project evolves toward a richer per-directory or per-card abstraction, but they are not part of the current implementation.

## Conversion Strategy

### VMP → SRM

Process:

1. Read `SCEVMC0.VMP`
2. Remove first 128 bytes
3. Write `.SRM` payload

CLI helper:

- reusable C module: `src/vmp_srm_converter.c`
- standalone wrapper: `tools/convert-vmp-to-srm.ps1`
- shell wrapper: `tools/convert-vmp-to-srm.sh`
- example (Windows): `.\tools\convert-vmp-to-srm.ps1 .\SCEVMC0.VMP`
- example (Linux/macOS): `./tools/convert-vmp-to-srm.sh ./SCEVMC0.VMP`

The PowerShell script uses the standalone `vmp2srm` tool and can build it automatically into `build-tools/` if needed.

### SRM → VMP

Process:

1. Read `.SRM`
2. Prepend 128-byte VMP header
3. Write `SCEVMC0.VMP`

CLI helper:

- reusable C module: `src/vmp_srm_converter.c`
- standalone wrapper: `tools/convert-srm-to-vmp.ps1`
- shell wrapper: `tools/convert-srm-to-vmp.sh`
- example (Windows): `.\tools\convert-srm-to-vmp.ps1 .\SAVE.SRM .\SAVE.VMP .\SCEVMC0.VMP`
- example (Linux/macOS): `./tools/convert-srm-to-vmp.sh ./SAVE.SRM ./SAVE.VMP ./SCEVMC0.VMP`
- signing: the wrappers always re-sign using `vita-mcr2vmp` (submodule `tools/vita-mcr2vmp`) to avoid checksum error (`80010005`).
- the app now uses the same signing logic directly in `SyncEngine` after `SRM -> VMP` reconstruction.

Required setup:

- `git submodule update --init tools/vita-mcr2vmp`

Build behavior:

- the wrappers look for `srm2vmp` and `vita-mcr2vmp` in `build-tools/`
- if either tool is missing (or `--rebuild` is passed), the wrappers run `cmake -S tools -B build-tools` then `cmake --build build-tools --config Release`
- configuration fails fast when `tools/vita-mcr2vmp` is missing

Usage:

- Linux/macOS: `./tools/convert-srm-to-vmp.sh <INPUT.SRM> [OUTPUT.VMP] <TEMPLATE.VMP>`
- Windows: `.\tools\convert-srm-to-vmp.ps1 <INPUT.SRM> [OUTPUT.VMP] <TEMPLATE.VMP>`
- Slot selection: defaults to slot 0; override with `--slot 0` or `--slot 1` (`-Slot 0|1` on PowerShell). If no output name is provided, the script writes `SCEVMC<slot>.VMP` beside the input.
- The wrappers perform a VMP→MCR→VMP round-trip to recompute the signature with `vita-mcr2vmp` and then replace the unsigned output with the signed VMP.

Rules:

- reuse known-good VMP headers
- never overwrite automatically
- always create backups first

The `SRM -> VMP` wrappers require a known-good `.VMP` template header. Pass an existing Vita/Adrenaline `SCEVMC0.VMP` or `SCEVMC1.VMP`, or set `ROMM_VMP_TEMPLATE_PATH`. Templates are provided in `samples/vmp-templates/`.

## PS1 Sync Pipeline

Upload flow:

```
PS Vita save folder
→ PARAM.SFO parser
→ VMP reader
→ VMP → SRM conversion (in-app, temp file)
→ RomM upload / compare
```

Restore flow:

```
RomM SRM file
→ download to temp SRM
→ SRM → VMP conversion
→ VMP re-sign (in-app)
→ backup existing VMP
→ restore VMP
```

## Safety Model

Synchronization is non-destructive by default.

Rules:

- never overwrite local saves automatically
- always create backups before restore
- compare timestamps and hashes when possible
- require explicit confirmation before sync operations
- preserve original `.VMP` files before rebuilding

## Architecture (Current Codebase)

Current implementation modules:

- `src/save_scanner.c`: recursively scans candidate roots, detects `.VMP` files, and reads `PARAM.SFO` metadata
- `src/save_item.h`: defines the scanner inventory item returned by the local scan
- `src/scan_to_sync_adapter.c`: converts scanner output into synchronization descriptors
- `src/sync_types.h` and `src/sync_types.c`: define shared sync data structures and helpers such as slot and timestamp parsing
- `src/game_matcher.c`: resolves local saves to RomM `rom_id` candidates using serials, titles, and filename patterns
- `src/romm_http_client.c`: handles device registration, platform lookup, RomM ROM lookup, remote save listing, uploads, and downloads
- `src/conflict_resolver.c`: compares local and remote saves and classifies conflicts
- `src/backup_manager.c`: creates local backups before overwrite operations
- `src/sync_state_store.c`: persists sync state used to track prior uploads and origins
- `src/sync_engine.c`: plans and executes upload/download actions
- `src/main.c`: owns UI state, scan orchestration, sync triggering, and on-screen feedback

Implementation notes:

- PS1 support is the only implemented save adapter today
- VMP and SRM conversion is implemented through `src/vmp_srm_converter.c` and used by the sync flow
- multi-platform adapters remain roadmap items and are not part of the current codebase yet

## Roadmap

### v1 — PS1 (Adrenaline)

- scan SAVEDATA folders
- detect PS1 save directories
- parse PARAM.SFO
- detect VMP files
- convert VMP ↔ SRM
- compare remote saves
- manual sync workflow
- automatic backup system
- manual sync button inside homebrew UI

### v2 — PSP Saves

- scan SAVEDATA folders
- map saves by TitleID
- bidirectional sync
- improved conflict detection

### v3 — PS Vita Native Saves

- evaluate encryption constraints
- investigate extraction workflows
- integrate native save support where possible

### v4 — Emulator Environments

- RetroArch save support
- additional emulator integrations
- per-core save mapping

### v5 — Advanced PS1 Support

- improve memory card tooling
- extend diagnostics and validation
- refine synchronization workflows


## Future Plugin-Based Automation (Planned)

After the synchronization engine is validated in the standalone homebrew version, a taiHEN plugin will be introduced to support automatic synchronization triggers.

Planned triggers include:

- after exiting a game
- when returning to LiveArea
- during safe idle system states

The plugin will act as a lightweight event listener and delegate synchronization work to the existing SyncEngine.

## Requirements

Runtime:

- PlayStation Vita with homebrew support
- Adrenaline installed (for PS1 support)
- RomM server instance with save sync enabled

Build:

- VitaSDK toolchain
- CMake
- submodules initialized (`git submodule update --init --recursive`)

## Development Status

This project is currently in its bootstrap phase.

Initial goals:

- setup VitaSDK project structure
- implement filesystem scanning
- detect PS1 save folders
- parse PARAM.SFO
- detect `.VMP` files
- implement `.VMP ↔ .SRM` conversion helpers
- display save inventory UI

RomM device registration is now wired to the real HTTP endpoint (`POST /api/devices`).
Save listing/upload/download are now wired to real HTTP callbacks in `main.c` (`/api/saves` endpoints).
Local Vita items are resolved to server `rom_id` before sync decisions by:

- resolving the configured platform through `/api/platforms`
- trying a targeted RomM lookup first through `/api/roms?platform_ids=...&search_term=...`
- matching candidates by serial metadata first, then title (`fs_name_no_tags`, `name`, `fs_name_no_ext`), then filename patterns
- surfacing an explicit user-facing warning when the targeted lookup returns no candidate

Conversion is now wired in the app sync flow:

- upload path: local `.VMP` is converted to temporary `.SRM` before transfer callback
- download path: remote `.SRM` is reconstructed to local `.VMP` and re-signed in-app

## End-to-End Sync Sequence Diagram

Current Rom ID mapping flow:

1. The app resolves the configured RomM platform through `/api/platforms`.
2. It first narrows candidates with `/api/roms?platform_ids=...&search_term=...`.
3. Each local save is matched by serial metadata first.
4. If serial matching is not unique, the matcher tries title-based candidates in this order: `fs_name_no_tags`, `name`, `fs_name_no_ext`.
5. If title matching is still not unique, the matcher falls back to filename-pattern scoring inside the targeted candidate set.
6. If the targeted lookup returns no candidate, the UI reports that explicitly instead of loading the full platform catalog.

## Contributing

Contributions, ideas, and feedback are welcome.

Suggested areas:

- UI improvements
- emulator adapter support

## Disclaimer

This project does not provide or distribute game content.

It only synchronizes user-owned save data between a PS Vita device and a RomM server.

Users are responsible for complying with applicable laws and platform policies.
