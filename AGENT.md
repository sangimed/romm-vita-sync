# AGENT.md

## Purpose

This document defines the synchronization philosophy and architectural constraints of **romm-vita-sync (RVS)**.

It serves as guidance for contributors and automation agents interacting with the codebase.

RVS is currently designed as a **manual synchronization homebrew application for PlayStation Vita** aligned with the RomM save synchronization ecosystem.

Future automation layers are intentionally out of scope at this stage.

---

# Synchronization Philosophy

RVS follows a safe, deterministic, device-aware synchronization model.

Key principles:

- never overwrite saves automatically
- never assume timestamp authority without comparison
- always preserve backups before restore
- prefer explicit user-triggered synchronization
- maintain compatibility with RomM device-based sync architecture
- avoid destructive operations without confirmation

The synchronization engine must remain deterministic and testable from a standalone execution context.

---

# Execution Model (Version 1)

Version 1 runs as a **manual homebrew application**.

Synchronization is triggered explicitly by the user from within the application UI.

Example workflow:

Scan local saves
→ Map saves to RomM entries
→ Compare metadata
→ Suggest actions
→ Execute upload/download
→ Persist sync state

No automatic filesystem watchers are used.

No background execution is expected.

No lifecycle hooks are implemented.

---

# Sync Authority Model

RomM is evolving toward a server-driven synchronization model.

RVS must remain compatible with this direction.

The client should:

- describe local save state
- query remote save state
- execute safe transfer operations
- avoid enforcing unilateral overwrite decisions

Until RomM exposes full sync sessions, RVS uses a hybrid comparison model:

client-side comparison
+
device-aware metadata
+
timestamp reconciliation

---

# Device Awareness

Each RVS instance represents a RomM device participant.

Version 1 prepares support for:

- persistent device_id storage
- device registration compatibility
- tracking uploaded saves per device
- avoiding redundant downloads from same-origin saves
- conflict-aware synchronization logic

Device identity must survive application restarts once implemented.

---

# Save Detection Strategy

Version 1 supports:

ux0:/pspemu/PSP/SAVEDATA/<GAME_ID>/

Detected files:

PARAM.SFO
ICON0.PNG
CONFIG.BIN
SCEVMC0.VMP
SCEVMC1.VMP

Only `.VMP` containers are synchronized.

Conversion pipeline:

VMP → SRM → RomM
RomM → SRM → VMP

---

# Conversion Rules

Observed structure:

VMP = 128-byte header + raw memory card data
SRM = raw memory card data

Conversion requirements:

- never generate synthetic headers
- reuse trusted VMP templates
- always re-sign generated VMP files
- always backup original containers

Conversion correctness is critical.

Invalid VMP reconstruction may cause emulator rejection.

---

# Sync Decision Pipeline

Synchronization must follow this deterministic flow:

scan local saves
→ identify RomM match
→ compare timestamps
→ compare origin device (when available)
→ detect conflicts
→ request confirmation if needed
→ execute transfer
→ record sync state

Uploads must be skipped when:

size unchanged
AND
timestamp unchanged

Future versions may introduce hashing-based validation.

---

# Conflict Resolution Strategy

Conflict cases include:

remote newer than local
local newer than remote
same timestamp different content
same-origin device upload

Resolution rules:

prefer manual confirmation
never overwrite silently
always create backups

---

# Slot Awareness

PS1 saves use memory card slots:

SCEVMC0.VMP
SCEVMC1.VMP

RomM supports slot metadata.

RVS must preserve slot identity during conversion and transfer.

Slot mismatch must never occur silently.

---

# Sync State Persistence

RVS stores metadata describing previous synchronization events.

Minimum tracked attributes:

filename
size
timestamp
origin device
last upload time

Purpose:

avoid redundant uploads
avoid download loops
support conflict detection

Persistent storage format is implementation-defined.

---

# Supported Platforms Roadmap

Current:

PS1 via Adrenaline

Planned:

PSP saves
PS Vita native saves
RetroArch cores

Each platform requires a dedicated adapter module.

---

# Module Responsibilities

Core modules:

SaveScanner
GameMatcher
SyncEngine
RomMClient
BackupManager
ConflictResolver
SyncStateStore

Platform adapters:

PS1VMPAdapter
PSPSaveAdapter
VitaSaveAdapter
RetroArchAdapter

Converters:

VmpToSrmConverter
SrmToVmpConverter

SyncEngine must remain platform-agnostic.

Adapters translate platform-specific formats.

---

# Non-Goals (Version 1)

The following are intentionally out of scope:

background synchronization
filesystem watchers
automatic overwrite
lifecycle event triggers
native Vita save decryption
multi-device arbitration

These features belong to later phases.

---

# Design Constraints

RVS must remain:

deterministic
recoverable
device-aware
RomM-compatible
conversion-safe
extensible

Every synchronization operation must be reversible.

Backups are mandatory before restore operations.

---

# Future Compatibility Target

The architecture must remain compatible with:

RomM device registration
RomM save tracking
RomM slot metadata
RomM sync session API (future)

SyncEngine must not depend on UI execution context.

It must support:

manual invocation
scheduled invocation (future)

without modification.
