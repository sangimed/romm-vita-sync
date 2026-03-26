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

## Project Status

**Stage:** early development

Initial milestone includes:

- Detect PS1 save folders under `ux0:/pspemu/PSP/SAVEDATA`
- Parse `PARAM.SFO`
- Detect `SCEVMC0.VMP` and `SCEVMC1.VMP`
- Display metadata (path, size, timestamp)
- Convert `.VMP` → `.SRM`
- Compare local vs remote saves
- Manual synchronization workflow
- Automatic backup before overwrite
- Zero destructive operations without confirmation

No write operations will be enabled until safety mechanisms are validated.

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

Per-save-block parsing inside memory cards is planned for a later release.

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

This avoids block-level parsing for version 1.

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
- standalone wrapper: `tools/Convert-VmpToSrm.ps1`
- shell wrapper: `tools/Convert-VmpToSrm.sh`
- example: `.\tools\Convert-VmpToSrm.ps1 .\SCEVMC0.VMP`
- example (Linux/macOS): `./tools/Convert-VmpToSrm.sh ./SCEVMC0.VMP`

The PowerShell script uses the standalone `vmp2srm` tool and can build it automatically into `build-tools/` if needed.

### SRM → VMP

Process:

1. Read `.SRM`
2. Prepend 128-byte VMP header
3. Write `SCEVMC0.VMP`

Rules:

- reuse known-good VMP headers
- never overwrite automatically
- always create backups first

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

- inspect memory card contents
- identify individual save blocks
- support granular synchronization

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
