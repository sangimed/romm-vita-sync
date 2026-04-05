#ifndef GAME_MATCHER_H
#define GAME_MATCHER_H

#include "sync_types.h"

#define GAME_MATCHER_MAX_PLATFORM_SLUG_LEN 64
#define GAME_MATCHER_MAX_ROM_LABEL_LEN 128
#define GAME_MATCHER_MAX_SERIAL_LIST_LEN 256
#define GAME_MATCHER_NO_MATCH 0
#define GAME_MATCHER_AMBIGUOUS (-2)

typedef struct GameMatcherRomCandidate {
  int rom_id;
  char platform_slug[GAME_MATCHER_MAX_PLATFORM_SLUG_LEN];
  char name[GAME_MATCHER_MAX_ROM_LABEL_LEN];
  char fs_name[GAME_MATCHER_MAX_ROM_LABEL_LEN];
  char fs_name_no_ext[GAME_MATCHER_MAX_ROM_LABEL_LEN];
  char serial[ROMM_GAME_ID_LEN];
  char serials[GAME_MATCHER_MAX_SERIAL_LIST_LEN];
} GameMatcherRomCandidate;

/*
 * Finds the best remote candidate for a local save entry.
 * Returns remote index on success, or -1 when no deterministic match exists.
 */
int game_matcher_find_remote_index(
    const SyncSaveDescriptor *local_item,
    const SyncSaveDescriptor *remote_items,
    int remote_count);

/*
 * Resolves a RomM rom_id for a local PS1 save descriptor using deterministic stages:
 * serial metadata (GAME_ID), then title->fs_name_no_ext, then filename patterns.
 * Returns rom_id on success, or -1 when no safe deterministic match exists.
 */
int game_matcher_resolve_rom_id(
    const SyncSaveDescriptor *local_item,
    const GameMatcherRomCandidate *catalog,
    int catalog_count);

#endif
