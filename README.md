# romm-vita-sync

Save synchronization client for RomM on PS Vita, supporting native and emulated platforms.

## Overview

**romm-vita-sync** is a homebrew application for PlayStation Vita that synchronizes game save data with a RomM server.

The long-term goal is to support save synchronization across:

- PS Vita native games
- PSP (Adrenaline)
- PS1 (POPS via Adrenaline)
- RetroArch and other emulator environments

The initial version focuses on **PS1 save synchronization** through Adrenaline virtual memory cards.

---

## Current status

Project stage: **early development**

Planned first milestone:

- detect PS1 virtual memory cards (VMP)
- display metadata (path, size, timestamp)
- compare local vs RomM saves
- manual sync only
- automatic backup before overwrite
- zero destructive operations without confirmation

No write operations will be performed until safety mechanisms are fully implemented.

---

## Why this project exists

RomM already provides save sync support for several platforms and clients (e.g. Android launchers).

However, there is currently no native save sync solution for PS Vita.

This project aims to provide:

- reliable save backup
- bidirectional synchronization with RomM
- conflict-safe restore workflows
- compatibility with Adrenaline-based PS1 saves

---

## Scope (v1)

Version 1 targets **PS1 saves via Adrenaline**.

Specifically:

- detect `.VMP` files
- treat each memory card as an atomic sync unit
- compute metadata for comparison
- support manual upload/download workflows
- always create backups before overwrite

Per-game PS1 save parsing inside memory cards is planned for a later version.

---

## Roadmap

### v1 – PS1 support (Adrenaline)

- scan PS1 VMC directories
- detect available VMP files
- display memory card metadata
- compare with RomM remote saves
- manual sync workflow
- automatic backup before overwrite

### v2 – PSP saves

- scan `ux0:/pspemu/PSP/SAVEDATA`
- map saves by title ID
- bidirectional sync
- improved conflict detection

### v3 – PS Vita native saves

- evaluate encryption constraints
- investigate safe extraction workflows
- integrate native save support where possible

### v4 – Emulator environments

- RetroArch save support
- additional emulator integrations
- per-core save mapping

---

## Safety model

Save synchronization is designed to be **non-destructive by default**.

Rules:

- never overwrite local saves automatically
- always create backups before restore
- compare timestamps and hashes when possible
- require explicit confirmation before sync operations
- keep previous versions for rollback

---

## Architecture (planned)

Core modules:

RomMClient
SaveScanner
SyncEngine
BackupManager
ConflictResolver
UI Layer

Adapters:

PS1VMPAdapter
PSPAdapter (planned)
VitaSaveAdapter (planned)
RetroArchAdapter (planned)

---

## Requirements

- PlayStation Vita with homebrew support
- Adrenaline installed (for PS1 support)
- RomM server instance with save sync enabled
- VitaSDK toolchain

---

## Development status

This project is currently in its bootstrap phase.

Initial goals:

- setup VitaSDK project structure
- implement filesystem scanning
- detect VMP files
- display save inventory UI

RomM API integration will follow once local scanning is stable.

---

## Contributing

Contributions, ideas, and feedback are welcome.

Planned contribution areas:

- VMP parsing
- RomM API integration
- UI improvements
- save metadata mapping
- emulator adapter support

---

## Disclaimer

This project does not provide or distribute game content.

It only synchronizes user-owned save data between a PS Vita device and a RomM server.

Users are responsible for complying with applicable laws and platform policies.
