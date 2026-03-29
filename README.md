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

The first implementation targets a manual synchronization workflow executed from a dedicated homebrew application. Automatic background synchronization via system plugins is planned for a later stage.

## Project Status

**Stage:** early development

This project is in early development. Back up your save data before installation or any manipulation.

Initial milestone includes:

- Detect PS1 save folders under `ux0:/pspemu/PSP/SAVEDATA`
- Parse `PARAM.SFO`
- Detect `SCEVMC0.VMP` and `SCEVMC1.VMP`
- Display metadata (path, size, timestamp)
- Convert `.VMP` → `.SRM`
- Convert `.SRM` → `.VMP`
- Compare local vs remote saves
- Manual synchronization workflow
- Synchronization is triggered manually from the application UI in this phase (automatic triggers planned later)
- Automatic backup before overwrite
- Zero destructive operations without confirmation

No write operations will be enabled until safety mechanisms are validated.


## Execution Model (Version 1)

romm-vita-sync is implemented first as a standalone PS Vita homebrew application.

Synchronization is explicitly triggered by the user from within the application UI.

This approach ensures:

- safe validation of save conversion logic
- deterministic synchronization behavior
- easier debugging during early development
- reduced risk of unintended save overwrites

Automatic synchronization based on system events (for example after exiting a game) will be introduced later through a taiHEN plugin once the synchronization engine is fully validated.

## Configuration (`settings.ini`)

Credentials and sync runtime options are read from:

`ux0:data/romm-vita-sync/settings.ini`

Format is INI-style (conventional in C/C++ desktop and embedded projects).

Reference template:

`samples/settings.ini.example`

Supported sections:

- `[RomM]`: `url`, `token`, `username`, `password`, `verify_tls`, `timeout_seconds`
- `[Device]`: `device_id`, `device_name`, `device_platform`, `client`, `client_version`
- `[Sync]`: `state_store_path`, `backup_directory`, `dry_run`

Credential rule:

- either `token`, or `username` + `password`
- if `[Device].device_id` is empty, startup calls the `RommClient` registration flow and persists the returned `device_id` into `settings.ini`

The sync engine now treats server `409 Conflict` responses as synchronization conflicts (remote newer) rather than generic transfer errors, aligned with `romm-retroarch-sync` behavior.

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
- Support manual upload/download workflows
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

## Save Model

### Local Save Representation

```
LocalPs1Save
├─ game_id
├─ title
├─ save_dir
├─ icon_path
├─ config_path
├─ vmc0_path
├─ vmc1_path
└─ updated_at
```

### Remote Save Representation

```
RemotePs1Save
├─ platform = psx
├─ emulator = pcsx_rearmed
├─ filename
├─ remote_path
├─ hash
├─ updated_at
└─ size
```

### Canonical Internal Model

```
CanonicalPs1MemoryCard
├─ raw_memory_card_data (131072 bytes)
├─ metadata
└─ origin
```

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
→ VMP → SRM conversion
→ RomM upload / compare
```

Restore flow:

```
RomM SRM file
→ SRM → VMP conversion
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

## Architecture (Planned)

Core modules:

- RomMClient
- SaveScanner
- SyncEngine
- BackupManager
- ConflictResolver
- UILayer

Adapters:

- PS1SaveFolderScanner
- PS1VMPAdapter
- PS1SRMAdapter
- PSPAdapter (planned)
- VitaSaveAdapter (planned)
- RetroArchAdapter (planned)

Converters:

- VmpToSrmConverter
- SrmToVmpConverter
- PsfParser

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

- PlayStation Vita with homebrew support
- Adrenaline installed (for PS1 support)
- RomM server instance with save sync enabled
- VitaSDK toolchain

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

RomM API integration will follow once local scanning and conversion are stable.

## Contributing

Contributions, ideas, and feedback are welcome.

Suggested areas:

- PARAM.SFO parsing
- VMP/SRM conversion
- RomM API integration
- UI improvements
- save metadata mapping
- emulator adapter support

## Disclaimer

This project does not provide or distribute game content.

It only synchronizes user-owned save data between a PS Vita device and a RomM server.

Users are responsible for complying with applicable laws and platform policies.
