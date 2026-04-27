#ifndef GAME_MATCHER_H
#define GAME_MATCHER_H

#include "sync_types.h"

#define GAME_MATCHER_MAX_PLATFORM_SLUG_LEN 64
#define GAME_MATCHER_MAX_ROM_LABEL_LEN 128
#define GAME_MATCHER_MAX_SERIAL_LIST_LEN 256
#define GAME_MATCHER_MAX_ALTERNATIVE_NAMES_LEN 256
#define GAME_MATCHER_NO_MATCH 0
#define GAME_MATCHER_AMBIGUOUS (-2)

typedef struct GameMatcherRomCandidate {
  int rom_id;
  char platform_slug[GAME_MATCHER_MAX_PLATFORM_SLUG_LEN];
  char name[GAME_MATCHER_MAX_ROM_LABEL_LEN];
  char fs_name[GAME_MATCHER_MAX_ROM_LABEL_LEN];
  char fs_name_no_tags[GAME_MATCHER_MAX_ROM_LABEL_LEN];
  char fs_name_no_ext[GAME_MATCHER_MAX_ROM_LABEL_LEN];
  char serial[ROMM_GAME_ID_LEN];
  char serials[GAME_MATCHER_MAX_SERIAL_LIST_LEN];
  char alternative_names[GAME_MATCHER_MAX_ALTERNATIVE_NAMES_LEN];
} GameMatcherRomCandidate;

typedef enum GameMatcherMatchStage {
  GAME_MATCHER_MATCH_STAGE_NONE = 0,
  GAME_MATCHER_MATCH_STAGE_EXISTING_ROM_ID = 1,
  GAME_MATCHER_MATCH_STAGE_SERIAL = 2,
  GAME_MATCHER_MATCH_STAGE_TITLE = 3,
  GAME_MATCHER_MATCH_STAGE_FILENAME_PATTERNS = 4
} GameMatcherMatchStage;

typedef enum GameMatcherMatchField {
  GAME_MATCHER_MATCH_FIELD_NONE = 0,
  GAME_MATCHER_MATCH_FIELD_ROM_ID = 1,
  GAME_MATCHER_MATCH_FIELD_SERIAL = 2,
  GAME_MATCHER_MATCH_FIELD_SERIALS = 3,
  GAME_MATCHER_MATCH_FIELD_FS_NAME_NO_TAGS = 4,
  GAME_MATCHER_MATCH_FIELD_NAME = 5,
  GAME_MATCHER_MATCH_FIELD_FS_NAME_NO_EXT = 6,
  GAME_MATCHER_MATCH_FIELD_ALTERNATIVE_NAMES = 7
} GameMatcherMatchField;

typedef struct GameMatcherResolution {
  int rom_id;
  int score;
  GameMatcherMatchStage stage;
  GameMatcherMatchField field;
} GameMatcherResolution;

/*
 * Finds the best remote candidate for a local save entry.
 * Returns remote index on success, or -1 when no deterministic match exists.
 */
int game_matcher_find_remote_index(
    const SyncSaveDescriptor *local_item,
    const SyncSaveDescriptor *remote_items,
    int remote_count);

/*
 * Resolves a RomM rom_id for a local save descriptor using deterministic stages:
 * serial metadata (GAME_ID), then title against cleaned RomM names, then filename patterns.
 * Returns rom_id on success, GAME_MATCHER_AMBIGUOUS when multiple candidates tie,
 * or -1 when no safe deterministic match exists.
 */
int game_matcher_resolve_rom_id_with_details(
    const SyncSaveDescriptor *local_item,
    const GameMatcherRomCandidate *catalog,
    int catalog_count,
    GameMatcherResolution *out_resolution);

/*
 * Backward-compatible wrapper around game_matcher_resolve_rom_id_with_details().
 */
int game_matcher_resolve_rom_id(
    const SyncSaveDescriptor *local_item,
    const GameMatcherRomCandidate *catalog,
    int catalog_count);

const char *game_matcher_match_stage_str(GameMatcherMatchStage stage);
const char *game_matcher_match_field_str(GameMatcherMatchField field);

/*
 * Normalizes one user-visible title for resilient matching and search:
 * lowercase, punctuation/symbol cleanup, diacritics folding, and space collapse.
 */
void game_matcher_normalize_title(
    const char *input,
    char *output,
    size_t output_size);

#endif
