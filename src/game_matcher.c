#include "game_matcher.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/*
 * Returns non-zero when a string is non-null and non-empty.
 */
static int has_text(const char *value) {
  return (value != NULL) && (value[0] != '\0');
}

/*
 * Validates whether a slot value is one of the known PS1 card slots.
 */
static int slot_is_known(SyncSlot slot) {
  return (slot == SYNC_SLOT_0) || (slot == SYNC_SLOT_1);
}

/*
 * Returns non-zero when two slots can be considered compatible for matching.
 * Unknown slots are treated as compatible fallbacks.
 */
static int slot_is_compatible(SyncSlot local_slot, SyncSlot remote_slot) {
  if (!slot_is_known(local_slot) || !slot_is_known(remote_slot)) {
    return 1;
  }

  return local_slot == remote_slot;
}

/*
 * Matches items by filename when both descriptors provide one.
 */
static int filename_matches(const SyncSaveDescriptor *local_item, const SyncSaveDescriptor *remote_item) {
  if ((local_item == NULL) || (remote_item == NULL)) {
    return 0;
  }

  if (!has_text(local_item->filename) || !has_text(remote_item->filename)) {
    return 0;
  }

  return sync_string_ieq(local_item->filename, remote_item->filename);
}

/*
 * Matches items by RomM ROM identifier when both sides provide one.
 */
static int rom_id_matches(const SyncSaveDescriptor *local_item, const SyncSaveDescriptor *remote_item) {
  if ((local_item == NULL) || (remote_item == NULL)) {
    return 0;
  }

  if ((local_item->rom_id <= 0) || (remote_item->rom_id <= 0)) {
    return 0;
  }

  return local_item->rom_id == remote_item->rom_id;
}

/*
 * Keeps only lowercase ASCII alnum characters so identifiers compare reliably.
 */
static void normalize_identifier(const char *input, char *output, size_t output_size) {
  if ((output == NULL) || (output_size == 0U)) {
    return;
  }

  output[0] = '\0';
  if (!has_text(input)) {
    return;
  }

  size_t out = 0U;
  for (const unsigned char *cursor = (const unsigned char *)input;
       (*cursor != '\0') && ((out + 1U) < output_size);
       ++cursor) {
    unsigned char c = *cursor;
    if (isalnum(c)) {
      output[out++] = (char)tolower(c);
    }
  }
  output[out] = '\0';
}

/*
 * Returns non-zero when a platform slug likely refers to PS1 content.
 */
static int is_ps1_platform_slug(const char *platform_slug) {
  char normalized[GAME_MATCHER_MAX_PLATFORM_SLUG_LEN];
  normalize_identifier(platform_slug, normalized, sizeof(normalized));
  if (!has_text(normalized)) {
    return 0;
  }

  if (sync_string_ieq(normalized, "ps") ||
      sync_string_ieq(normalized, "psone")) {
    return 1;
  }
  if (strstr(normalized, "psx") != NULL) {
    return 1;
  }
  if ((strstr(normalized, "playstation") != NULL) ||
      (strstr(normalized, "ps1") != NULL)) {
    return 1;
  }

  return 0;
}

/*
 * Accepts only explicit PS1 platform slugs.
 */
static int candidate_is_ps1_eligible(const GameMatcherRomCandidate *candidate) {
  if (candidate == NULL) {
    return 0;
  }
  if (!has_text(candidate->platform_slug)) {
    return 0;
  }
  return is_ps1_platform_slug(candidate->platform_slug);
}

/*
 * Normalizes a filename/path by stripping extension and punctuation.
 */
static void normalize_filename_without_extension(
    const char *input,
    char *output,
    size_t output_size) {
  if ((output == NULL) || (output_size == 0U)) {
    return;
  }

  output[0] = '\0';
  if (!has_text(input)) {
    return;
  }

  char basename[ROMM_SYNC_MAX_FILENAME_LEN];
  if (sync_extract_filename(input, basename, sizeof(basename)) < 0) {
    snprintf(basename, sizeof(basename), "%s", input);
  }

  char stem[ROMM_SYNC_MAX_FILENAME_LEN];
  snprintf(stem, sizeof(stem), "%s", basename);
  char *dot = strrchr(stem, '.');
  if ((dot != NULL) && (dot != stem)) {
    *dot = '\0';
  }

  normalize_identifier(stem, output, output_size);
}

/*
 * Returns non-zero when a normalized name is the generic PS1 card filename.
 */
static int is_generic_ps1_card_filename(const char *normalized_name) {
  if (!has_text(normalized_name)) {
    return 0;
  }
  return sync_string_ieq(normalized_name, "scevmc0") ||
         sync_string_ieq(normalized_name, "scevmc1");
}

/*
 * Extracts a useful local filename pattern while discarding generic VMP names.
 */
static void extract_local_filename_pattern(
    const SyncSaveDescriptor *local_item,
    char *output,
    size_t output_size) {
  if ((output == NULL) || (output_size == 0U)) {
    return;
  }
  output[0] = '\0';

  if (local_item == NULL) {
    return;
  }

  if (has_text(local_item->filename)) {
    normalize_filename_without_extension(local_item->filename, output, output_size);
  } else if (has_text(local_item->path)) {
    normalize_filename_without_extension(local_item->path, output, output_size);
  }

  if (is_generic_ps1_card_filename(output)) {
    output[0] = '\0';
  }
}

/*
 * Returns non-zero when one serial-like value resolves to the local GAME_ID.
 */
static int serial_value_matches(const char *serial_value, const char *normalized_game_id) {
  if (!has_text(serial_value) || !has_text(normalized_game_id)) {
    return 0;
  }

  char normalized_serial[ROMM_GAME_ID_LEN];
  normalize_identifier(serial_value, normalized_serial, sizeof(normalized_serial));
  if (!has_text(normalized_serial)) {
    return 0;
  }

  return sync_string_ieq(normalized_serial, normalized_game_id);
}

/*
 * Returns non-zero when a delimited serial list contains the local GAME_ID.
 */
static int serial_list_matches(const char *serial_list, const char *normalized_game_id) {
  if (!has_text(serial_list) || !has_text(normalized_game_id)) {
    return 0;
  }

  char token[GAME_MATCHER_MAX_SERIAL_LIST_LEN];
  size_t token_len = 0U;
  for (const char *cursor = serial_list;; ++cursor) {
    const char c = *cursor;
    const int separator = (c == '\0') || (c == '|') || (c == ',') || (c == ';') ||
                          (c == '/') || (c == '\n') || (c == '\r') || (c == '\t') ||
                          (c == '(') || (c == ')') || (c == '[') || (c == ']') ||
                          (c == '{') || (c == '}');

    if (!separator && ((token_len + 1U) < sizeof(token))) {
      token[token_len++] = c;
    }

    if (separator) {
      if (token_len > 0U) {
        token[token_len] = '\0';
        if (serial_value_matches(token, normalized_game_id)) {
          return 1;
        }
        token_len = 0U;
      }
      if (c == '\0') {
        break;
      }
    }
  }

  return 0;
}

/*
 * Tracks unique rom_id matches and marks ambiguous collisions deterministically.
 */
static void register_unique_match(int rom_id, int *io_best_rom_id, int *io_is_ambiguous) {
  if ((io_best_rom_id == NULL) || (io_is_ambiguous == NULL) || (rom_id <= 0)) {
    return;
  }

  if (*io_best_rom_id < 0) {
    *io_best_rom_id = rom_id;
    return;
  }

  if (*io_best_rom_id != rom_id) {
    *io_is_ambiguous = 1;
  }
}

/*
 * Stage 1: resolves by exact GAME_ID match against serial metadata.
 */
static int resolve_rom_id_by_serial(
    const SyncSaveDescriptor *local_item,
    const GameMatcherRomCandidate *catalog,
    int catalog_count) {
  if ((local_item == NULL) || (catalog == NULL) || (catalog_count <= 0)) {
    return GAME_MATCHER_NO_MATCH;
  }

  char normalized_game_id[ROMM_GAME_ID_LEN];
  normalize_identifier(local_item->game_id, normalized_game_id, sizeof(normalized_game_id));
  if (strlen(normalized_game_id) < 4U) {
    return GAME_MATCHER_NO_MATCH;
  }

  int best_rom_id = -1;
  int ambiguous = 0;
  for (int i = 0; i < catalog_count; ++i) {
    const GameMatcherRomCandidate *candidate = &catalog[i];
    if ((candidate->rom_id <= 0) || !candidate_is_ps1_eligible(candidate)) {
      continue;
    }

    if (serial_value_matches(candidate->serial, normalized_game_id) ||
        serial_list_matches(candidate->serials, normalized_game_id)) {
      register_unique_match(candidate->rom_id, &best_rom_id, &ambiguous);
    }
  }

  if (ambiguous) {
    return GAME_MATCHER_AMBIGUOUS;
  }
  if (best_rom_id > 0) {
    return best_rom_id;
  }
  return GAME_MATCHER_NO_MATCH;
}

/*
 * Stage 2: resolves by PARAM.SFO title matched to RomM fs_name_no_ext.
 */
static int resolve_rom_id_by_title(
    const SyncSaveDescriptor *local_item,
    const GameMatcherRomCandidate *catalog,
    int catalog_count) {
  if ((local_item == NULL) || (catalog == NULL) || (catalog_count <= 0)) {
    return GAME_MATCHER_NO_MATCH;
  }

  char normalized_local_title[ROMM_GAME_TITLE_LEN];
  normalize_identifier(local_item->title, normalized_local_title, sizeof(normalized_local_title));
  if (strlen(normalized_local_title) < 4U) {
    return GAME_MATCHER_NO_MATCH;
  }

  int best_rom_id = -1;
  int ambiguous = 0;
  for (int i = 0; i < catalog_count; ++i) {
    const GameMatcherRomCandidate *candidate = &catalog[i];
    if ((candidate->rom_id <= 0) || !candidate_is_ps1_eligible(candidate)) {
      continue;
    }

    char normalized_catalog_name[GAME_MATCHER_MAX_ROM_LABEL_LEN];
    char normalized_catalog_display_name[GAME_MATCHER_MAX_ROM_LABEL_LEN];
    if (has_text(candidate->fs_name_no_ext)) {
      normalize_identifier(
          candidate->fs_name_no_ext,
          normalized_catalog_name,
          sizeof(normalized_catalog_name));
    } else {
      normalize_filename_without_extension(
          candidate->fs_name,
          normalized_catalog_name,
          sizeof(normalized_catalog_name));
    }
    normalize_identifier(
        candidate->name,
        normalized_catalog_display_name,
        sizeof(normalized_catalog_display_name));

    int matched = 0;
    if (has_text(normalized_catalog_name) &&
        sync_string_ieq(normalized_local_title, normalized_catalog_name)) {
      matched = 1;
    }
    if (!matched &&
        has_text(normalized_catalog_display_name) &&
        sync_string_ieq(normalized_local_title, normalized_catalog_display_name)) {
      matched = 1;
    }
    if (!matched && (strlen(normalized_local_title) >= 8U)) {
      if (has_text(normalized_catalog_name) &&
          ((strstr(normalized_catalog_name, normalized_local_title) != NULL) ||
           (strstr(normalized_local_title, normalized_catalog_name) != NULL))) {
        matched = 1;
      }
      if (!matched &&
          has_text(normalized_catalog_display_name) &&
          ((strstr(normalized_catalog_display_name, normalized_local_title) != NULL) ||
           (strstr(normalized_local_title, normalized_catalog_display_name) != NULL))) {
        matched = 1;
      }
    }

    if (matched) {
      register_unique_match(candidate->rom_id, &best_rom_id, &ambiguous);
    }
  }

  if (ambiguous) {
    return GAME_MATCHER_AMBIGUOUS;
  }
  if (best_rom_id > 0) {
    return best_rom_id;
  }
  return GAME_MATCHER_NO_MATCH;
}

/*
 * Scores one candidate for filename-pattern fallback matching.
 */
static int score_filename_pattern_match(
    const char *normalized_local_filename,
    const char *normalized_local_game_id,
    const char *normalized_local_title,
    const GameMatcherRomCandidate *candidate) {
  if ((candidate == NULL) || (candidate->rom_id <= 0)) {
    return 0;
  }

  char normalized_catalog_name[GAME_MATCHER_MAX_ROM_LABEL_LEN];
  char normalized_catalog_display_name[GAME_MATCHER_MAX_ROM_LABEL_LEN];
  if (has_text(candidate->fs_name_no_ext)) {
    normalize_identifier(candidate->fs_name_no_ext, normalized_catalog_name, sizeof(normalized_catalog_name));
  } else {
    normalize_filename_without_extension(candidate->fs_name, normalized_catalog_name, sizeof(normalized_catalog_name));
  }
  normalize_identifier(candidate->name, normalized_catalog_display_name, sizeof(normalized_catalog_display_name));

  if (!has_text(normalized_catalog_name) && !has_text(normalized_catalog_display_name)) {
    return 0;
  }

  const char *labels[2] = {
      normalized_catalog_name,
      normalized_catalog_display_name};

  int score = 0;
  for (size_t i = 0U; i < (sizeof(labels) / sizeof(labels[0])); ++i) {
    const char *label = labels[i];
    if (!has_text(label)) {
      continue;
    }

    if (has_text(normalized_local_filename) && (strlen(normalized_local_filename) >= 4U)) {
      if (sync_string_ieq(normalized_local_filename, label)) {
        if (score < 100) {
          score = 100;
        }
      } else if ((strstr(label, normalized_local_filename) != NULL) ||
                 (strstr(normalized_local_filename, label) != NULL)) {
        if (score < 70) {
          score = 70;
        }
      }
    }

    if (has_text(normalized_local_game_id) && (strlen(normalized_local_game_id) >= 4U)) {
      if (sync_string_ieq(normalized_local_game_id, label)) {
        if (score < 95) {
          score = 95;
        }
      } else if (strstr(label, normalized_local_game_id) != NULL) {
        if (score < 90) {
          score = 90;
        }
      }
    }

    if (has_text(normalized_local_title) && (strlen(normalized_local_title) >= 8U)) {
      if (sync_string_ieq(normalized_local_title, label)) {
        if (score < 85) {
          score = 85;
        }
      } else if ((strstr(label, normalized_local_title) != NULL) ||
                 (strstr(normalized_local_title, label) != NULL)) {
        if (score < 75) {
          score = 75;
        }
      }
    }
  }

  return score;
}

/*
 * Stage 3: resolves by local filename/game-id patterns against ROM filenames.
 */
static int resolve_rom_id_by_filename_patterns(
    const SyncSaveDescriptor *local_item,
    const GameMatcherRomCandidate *catalog,
    int catalog_count) {
  if ((local_item == NULL) || (catalog == NULL) || (catalog_count <= 0)) {
    return GAME_MATCHER_NO_MATCH;
  }

  char normalized_local_filename[ROMM_SYNC_MAX_FILENAME_LEN];
  extract_local_filename_pattern(local_item, normalized_local_filename, sizeof(normalized_local_filename));

  char normalized_local_game_id[ROMM_GAME_ID_LEN];
  normalize_identifier(local_item->game_id, normalized_local_game_id, sizeof(normalized_local_game_id));
  char normalized_local_title[ROMM_GAME_TITLE_LEN];
  normalize_identifier(local_item->title, normalized_local_title, sizeof(normalized_local_title));

  if (!has_text(normalized_local_filename) &&
      !has_text(normalized_local_game_id) &&
      !has_text(normalized_local_title)) {
    return GAME_MATCHER_NO_MATCH;
  }

  int best_score = 0;
  int best_rom_id = -1;
  int ambiguous = 0;
  for (int i = 0; i < catalog_count; ++i) {
    const GameMatcherRomCandidate *candidate = &catalog[i];
    if (!candidate_is_ps1_eligible(candidate)) {
      continue;
    }

    int score = score_filename_pattern_match(
        normalized_local_filename,
        normalized_local_game_id,
        normalized_local_title,
        candidate);
    if (score <= 0) {
      continue;
    }

    if (score > best_score) {
      best_score = score;
      best_rom_id = candidate->rom_id;
      ambiguous = 0;
      continue;
    }

    if ((score == best_score) && (best_rom_id != candidate->rom_id)) {
      ambiguous = 1;
    }
  }

  if ((best_score > 0) && !ambiguous && (best_rom_id > 0)) {
    return best_rom_id;
  }
  if (ambiguous) {
    return GAME_MATCHER_AMBIGUOUS;
  }
  return GAME_MATCHER_NO_MATCH;
}

/*
 * Finds the best remote candidate using stable identity rules:
 * exact rom+slot+filename, then rom+slot compatibility.
 */
int game_matcher_find_remote_index(
    const SyncSaveDescriptor *local_item,
    const SyncSaveDescriptor *remote_items,
    int remote_count) {
  if ((local_item == NULL) || (remote_items == NULL) || (remote_count <= 0)) {
    return -1;
  }

  for (int i = 0; i < remote_count; ++i) {
    const SyncSaveDescriptor *candidate = &remote_items[i];
    if (rom_id_matches(local_item, candidate) &&
        slot_is_known(local_item->slot) &&
        slot_is_known(candidate->slot) &&
        (local_item->slot == candidate->slot) &&
        filename_matches(local_item, candidate)) {
      return i;
    }
  }

  for (int i = 0; i < remote_count; ++i) {
    const SyncSaveDescriptor *candidate = &remote_items[i];
    if (rom_id_matches(local_item, candidate) && slot_is_compatible(local_item->slot, candidate->slot)) {
      return i;
    }
  }

  return -1;
}

/*
 * Resolves a local PS1 save to one RomM rom_id through deterministic fallbacks.
 */
int game_matcher_resolve_rom_id(
    const SyncSaveDescriptor *local_item,
    const GameMatcherRomCandidate *catalog,
    int catalog_count) {
  if ((local_item == NULL) || (catalog == NULL) || (catalog_count <= 0)) {
    return -1;
  }

  if (local_item->rom_id > 0) {
    return local_item->rom_id;
  }

  int rom_id = resolve_rom_id_by_serial(local_item, catalog, catalog_count);
  if (rom_id > 0) {
    return rom_id;
  }

  rom_id = resolve_rom_id_by_title(local_item, catalog, catalog_count);
  if (rom_id > 0) {
    return rom_id;
  }

  rom_id = resolve_rom_id_by_filename_patterns(local_item, catalog, catalog_count);
  if (rom_id > 0) {
    return rom_id;
  }

  return -1;
}
