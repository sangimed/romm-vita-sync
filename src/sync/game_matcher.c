#include "game_matcher.h"

#include "app_log.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAME_MATCHER_TITLE_MIN_LEN 2U
#define GAME_MATCHER_TITLE_CONFIDENCE_THRESHOLD 210
#define GAME_MATCHER_TITLE_AMBIGUITY_MARGIN 18
#define GAME_MATCHER_MAX_TITLE_TOKENS 24
#define GAME_MATCHER_MAX_TITLE_TOKEN_LEN 32

typedef struct TitleSimilarityBreakdown {
  int exact_bonus;
  int contains_bonus;
  int token_set_score;
  int token_sort_score;
  int levenshtein_score;
  int serial_bonus;
  int sequel_penalty;
  int overlap_tokens;
  int local_token_count;
  int label_token_count;
  int total_score;
} TitleSimilarityBreakdown;

/*
 * Returns non-zero when a string is non-null and non-empty.
 */
static int has_text(const char *value) {
  return (value != NULL) && (value[0] != '\0');
}

/*
 * Resets one resolution record so callers always receive stable defaults.
 */
static void game_matcher_resolution_init(GameMatcherResolution *resolution) {
  if (resolution == NULL) {
    return;
  }

  resolution->rom_id = -1;
  resolution->score = 0;
  resolution->stage = GAME_MATCHER_MATCH_STAGE_NONE;
  resolution->field = GAME_MATCHER_MATCH_FIELD_NONE;
}

/*
 * Stores one successful or ambiguous resolution detail for later logging.
 */
static void game_matcher_resolution_set(
    GameMatcherResolution *resolution,
    int rom_id,
    int score,
    GameMatcherMatchStage stage,
    GameMatcherMatchField field) {
  if (resolution == NULL) {
    return;
  }

  resolution->rom_id = rom_id;
  resolution->score = score;
  resolution->stage = stage;
  resolution->field = field;
}

const char *game_matcher_match_stage_str(GameMatcherMatchStage stage) {
  switch (stage) {
    case GAME_MATCHER_MATCH_STAGE_EXISTING_ROM_ID:
      return "existing-rom-id";
    case GAME_MATCHER_MATCH_STAGE_SERIAL:
      return "serial";
    case GAME_MATCHER_MATCH_STAGE_TITLE:
      return "title";
    case GAME_MATCHER_MATCH_STAGE_FILENAME_PATTERNS:
      return "filename-patterns";
    case GAME_MATCHER_MATCH_STAGE_NONE:
    default:
      return "none";
  }
}

const char *game_matcher_match_field_str(GameMatcherMatchField field) {
  switch (field) {
    case GAME_MATCHER_MATCH_FIELD_ROM_ID:
      return "rom_id";
    case GAME_MATCHER_MATCH_FIELD_SERIAL:
      return "serial";
    case GAME_MATCHER_MATCH_FIELD_SERIALS:
      return "serials";
    case GAME_MATCHER_MATCH_FIELD_FS_NAME_NO_TAGS:
      return "fs_name_no_tags";
    case GAME_MATCHER_MATCH_FIELD_NAME:
      return "name";
    case GAME_MATCHER_MATCH_FIELD_FS_NAME_NO_EXT:
      return "fs_name_no_ext";
    case GAME_MATCHER_MATCH_FIELD_ALTERNATIVE_NAMES:
      return "alternative_names";
    case GAME_MATCHER_MATCH_FIELD_NONE:
    default:
      return "none";
  }
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
 * Decodes one UTF-8 codepoint and returns bytes consumed.
 * Invalid sequences are treated as one-byte fallback.
 */
static size_t decode_utf8_codepoint(const unsigned char *cursor, uint32_t *out_codepoint) {
  if ((cursor == NULL) || (out_codepoint == NULL)) {
    return 0U;
  }

  const unsigned char lead = cursor[0];
  if ((lead & 0x80U) == 0U) {
    *out_codepoint = (uint32_t)lead;
    return 1U;
  }

  if (((lead & 0xE0U) == 0xC0U) &&
      ((cursor[1] & 0xC0U) == 0x80U)) {
    *out_codepoint = ((uint32_t)(lead & 0x1FU) << 6) |
                     (uint32_t)(cursor[1] & 0x3FU);
    return 2U;
  }

  if (((lead & 0xF0U) == 0xE0U) &&
      ((cursor[1] & 0xC0U) == 0x80U) &&
      ((cursor[2] & 0xC0U) == 0x80U)) {
    *out_codepoint = ((uint32_t)(lead & 0x0FU) << 12) |
                     ((uint32_t)(cursor[1] & 0x3FU) << 6) |
                     (uint32_t)(cursor[2] & 0x3FU);
    return 3U;
  }

  if (((lead & 0xF8U) == 0xF0U) &&
      ((cursor[1] & 0xC0U) == 0x80U) &&
      ((cursor[2] & 0xC0U) == 0x80U) &&
      ((cursor[3] & 0xC0U) == 0x80U)) {
    *out_codepoint = ((uint32_t)(lead & 0x07U) << 18) |
                     ((uint32_t)(cursor[1] & 0x3FU) << 12) |
                     ((uint32_t)(cursor[2] & 0x3FU) << 6) |
                     (uint32_t)(cursor[3] & 0x3FU);
    return 4U;
  }

  *out_codepoint = (uint32_t)lead;
  return 1U;
}

/*
 * Returns non-zero when codepoint is one combining diacritic mark.
 */
static int is_combining_mark(uint32_t codepoint) {
  return ((codepoint >= 0x0300U) && (codepoint <= 0x036FU)) ||
         ((codepoint >= 0x1AB0U) && (codepoint <= 0x1AFFU)) ||
         ((codepoint >= 0x1DC0U) && (codepoint <= 0x1DFFU)) ||
         ((codepoint >= 0x20D0U) && (codepoint <= 0x20FFU)) ||
         ((codepoint >= 0xFE20U) && (codepoint <= 0xFE2FU));
}

/*
 * Folds common latin letters with diacritics into lowercase ASCII.
 * Returns number of bytes written (0 when unsupported).
 */
static size_t fold_latin_diacritic(uint32_t codepoint, char *out, size_t out_size) {
  if ((out == NULL) || (out_size == 0U)) {
    return 0U;
  }

  if (((codepoint >= 0x00C0U) && (codepoint <= 0x00C5U)) ||
      ((codepoint >= 0x00E0U) && (codepoint <= 0x00E5U)) ||
      (codepoint == 0x0100U) || (codepoint == 0x0101U) ||
      (codepoint == 0x0102U) || (codepoint == 0x0103U) ||
      (codepoint == 0x0104U) || (codepoint == 0x0105U) ||
      (codepoint == 0x01CDU) || (codepoint == 0x01CEU)) {
    out[0] = 'a';
    return 1U;
  }

  if ((codepoint == 0x00C6U) || (codepoint == 0x00E6U)) {
    if (out_size < 3U) {
      return 0U;
    }
    out[0] = 'a';
    out[1] = 'e';
    return 2U;
  }

  if ((codepoint == 0x00C7U) || (codepoint == 0x00E7U) ||
      (codepoint == 0x0106U) || (codepoint == 0x0107U) ||
      (codepoint == 0x0108U) || (codepoint == 0x0109U) ||
      (codepoint == 0x010AU) || (codepoint == 0x010BU) ||
      (codepoint == 0x010CU) || (codepoint == 0x010DU)) {
    out[0] = 'c';
    return 1U;
  }

  if ((codepoint == 0x010EU) || (codepoint == 0x010FU) ||
      (codepoint == 0x0110U) || (codepoint == 0x0111U)) {
    out[0] = 'd';
    return 1U;
  }

  if (((codepoint >= 0x00C8U) && (codepoint <= 0x00CBU)) ||
      ((codepoint >= 0x00E8U) && (codepoint <= 0x00EBU)) ||
      (codepoint == 0x0112U) || (codepoint == 0x0113U) ||
      (codepoint == 0x0114U) || (codepoint == 0x0115U) ||
      (codepoint == 0x0116U) || (codepoint == 0x0117U) ||
      (codepoint == 0x0118U) || (codepoint == 0x0119U) ||
      (codepoint == 0x011AU) || (codepoint == 0x011BU)) {
    out[0] = 'e';
    return 1U;
  }

  if ((codepoint == 0x011CU) || (codepoint == 0x011DU) ||
      (codepoint == 0x011EU) || (codepoint == 0x011FU) ||
      (codepoint == 0x0120U) || (codepoint == 0x0121U) ||
      (codepoint == 0x0122U) || (codepoint == 0x0123U)) {
    out[0] = 'g';
    return 1U;
  }

  if ((codepoint == 0x0124U) || (codepoint == 0x0125U) ||
      (codepoint == 0x0126U) || (codepoint == 0x0127U)) {
    out[0] = 'h';
    return 1U;
  }

  if (((codepoint >= 0x00CCU) && (codepoint <= 0x00CFU)) ||
      ((codepoint >= 0x00ECU) && (codepoint <= 0x00EFU)) ||
      (codepoint == 0x0128U) || (codepoint == 0x0129U) ||
      (codepoint == 0x012AU) || (codepoint == 0x012BU) ||
      (codepoint == 0x012CU) || (codepoint == 0x012DU) ||
      (codepoint == 0x012EU) || (codepoint == 0x012FU) ||
      (codepoint == 0x0130U) || (codepoint == 0x0131U)) {
    out[0] = 'i';
    return 1U;
  }

  if ((codepoint == 0x0134U) || (codepoint == 0x0135U)) {
    out[0] = 'j';
    return 1U;
  }

  if ((codepoint == 0x0136U) || (codepoint == 0x0137U) || (codepoint == 0x0138U)) {
    out[0] = 'k';
    return 1U;
  }

  if ((codepoint == 0x0139U) || (codepoint == 0x013AU) ||
      (codepoint == 0x013BU) || (codepoint == 0x013CU) ||
      (codepoint == 0x013DU) || (codepoint == 0x013EU) ||
      (codepoint == 0x013FU) || (codepoint == 0x0140U) ||
      (codepoint == 0x0141U) || (codepoint == 0x0142U)) {
    out[0] = 'l';
    return 1U;
  }

  if ((codepoint == 0x00D1U) || (codepoint == 0x00F1U) ||
      (codepoint == 0x0143U) || (codepoint == 0x0144U) ||
      (codepoint == 0x0145U) || (codepoint == 0x0146U) ||
      (codepoint == 0x0147U) || (codepoint == 0x0148U)) {
    out[0] = 'n';
    return 1U;
  }

  if (((codepoint >= 0x00D2U) && (codepoint <= 0x00D6U)) ||
      (codepoint == 0x00D8U) ||
      ((codepoint >= 0x00F2U) && (codepoint <= 0x00F6U)) ||
      (codepoint == 0x00F8U) ||
      (codepoint == 0x014CU) || (codepoint == 0x014DU) ||
      (codepoint == 0x014EU) || (codepoint == 0x014FU) ||
      (codepoint == 0x0150U) || (codepoint == 0x0151U) ||
      (codepoint == 0x01A0U) || (codepoint == 0x01A1U)) {
    out[0] = 'o';
    return 1U;
  }

  if ((codepoint == 0x0152U) || (codepoint == 0x0153U)) {
    if (out_size < 3U) {
      return 0U;
    }
    out[0] = 'o';
    out[1] = 'e';
    return 2U;
  }

  if ((codepoint == 0x0154U) || (codepoint == 0x0155U) ||
      (codepoint == 0x0156U) || (codepoint == 0x0157U) ||
      (codepoint == 0x0158U) || (codepoint == 0x0159U)) {
    out[0] = 'r';
    return 1U;
  }

  if ((codepoint == 0x015AU) || (codepoint == 0x015BU) ||
      (codepoint == 0x015CU) || (codepoint == 0x015DU) ||
      (codepoint == 0x015EU) || (codepoint == 0x015FU) ||
      (codepoint == 0x0160U) || (codepoint == 0x0161U) ||
      (codepoint == 0x00DFU)) {
    out[0] = 's';
    return 1U;
  }

  if ((codepoint == 0x0162U) || (codepoint == 0x0163U) ||
      (codepoint == 0x0164U) || (codepoint == 0x0165U) ||
      (codepoint == 0x0166U) || (codepoint == 0x0167U)) {
    out[0] = 't';
    return 1U;
  }

  if (((codepoint >= 0x00D9U) && (codepoint <= 0x00DCU)) ||
      ((codepoint >= 0x00F9U) && (codepoint <= 0x00FCU)) ||
      (codepoint == 0x0168U) || (codepoint == 0x0169U) ||
      (codepoint == 0x016AU) || (codepoint == 0x016BU) ||
      (codepoint == 0x016CU) || (codepoint == 0x016DU) ||
      (codepoint == 0x016EU) || (codepoint == 0x016FU) ||
      (codepoint == 0x0170U) || (codepoint == 0x0171U) ||
      (codepoint == 0x0172U) || (codepoint == 0x0173U) ||
      (codepoint == 0x01AFU) || (codepoint == 0x01B0U)) {
    out[0] = 'u';
    return 1U;
  }

  if ((codepoint == 0x00DDU) || (codepoint == 0x00FDU) ||
      (codepoint == 0x00FFU) || (codepoint == 0x0176U) ||
      (codepoint == 0x0177U) || (codepoint == 0x0178U)) {
    out[0] = 'y';
    return 1U;
  }

  if ((codepoint == 0x0179U) || (codepoint == 0x017AU) ||
      (codepoint == 0x017BU) || (codepoint == 0x017CU) ||
      (codepoint == 0x017DU) || (codepoint == 0x017EU)) {
    out[0] = 'z';
    return 1U;
  }

  if ((codepoint == 0x00DEU) || (codepoint == 0x00FEU)) {
    if (out_size < 3U) {
      return 0U;
    }
    out[0] = 't';
    out[1] = 'h';
    return 2U;
  }

  return 0U;
}

/*
 * Maps unicode roman numeral codepoints into lowercase ASCII equivalent.
 */
static size_t fold_roman_codepoint(uint32_t codepoint, char *out, size_t out_size) {
  if ((out == NULL) || (out_size == 0U)) {
    return 0U;
  }

  switch (codepoint) {
    case 0x2160U:
    case 0x2170U:
      out[0] = 'i';
      return 1U;
    case 0x2161U:
    case 0x2171U:
      if (out_size < 3U) {
        return 0U;
      }
      out[0] = 'i';
      out[1] = 'i';
      return 2U;
    case 0x2162U:
    case 0x2172U:
      if (out_size < 4U) {
        return 0U;
      }
      out[0] = 'i';
      out[1] = 'i';
      out[2] = 'i';
      return 3U;
    case 0x2163U:
    case 0x2173U:
      if (out_size < 3U) {
        return 0U;
      }
      out[0] = 'i';
      out[1] = 'v';
      return 2U;
    case 0x2164U:
    case 0x2174U:
      out[0] = 'v';
      return 1U;
    case 0x2165U:
    case 0x2175U:
      if (out_size < 3U) {
        return 0U;
      }
      out[0] = 'v';
      out[1] = 'i';
      return 2U;
    case 0x2166U:
    case 0x2176U:
      if (out_size < 4U) {
        return 0U;
      }
      out[0] = 'v';
      out[1] = 'i';
      out[2] = 'i';
      return 3U;
    case 0x2167U:
    case 0x2177U:
      if (out_size < 5U) {
        return 0U;
      }
      out[0] = 'v';
      out[1] = 'i';
      out[2] = 'i';
      out[3] = 'i';
      return 4U;
    case 0x2168U:
    case 0x2178U:
      if (out_size < 3U) {
        return 0U;
      }
      out[0] = 'i';
      out[1] = 'x';
      return 2U;
    case 0x2169U:
    case 0x2179U:
      out[0] = 'x';
      return 1U;
    case 0x216AU:
    case 0x217AU:
      if (out_size < 3U) {
        return 0U;
      }
      out[0] = 'x';
      out[1] = 'i';
      return 2U;
    case 0x216BU:
    case 0x217BU:
      if (out_size < 4U) {
        return 0U;
      }
      out[0] = 'x';
      out[1] = 'i';
      out[2] = 'i';
      return 3U;
    default:
      return 0U;
  }
}

/*
 * Appends one separator when output currently ends with non-space content.
 */
static void append_collapsed_space(char *output, size_t output_size, size_t *io_out, int *io_last_was_space) {
  if ((output == NULL) || (io_out == NULL) || (io_last_was_space == NULL)) {
    return;
  }
  if ((*io_last_was_space != 0) || (*io_out == 0U) || ((*io_out + 1U) >= output_size)) {
    return;
  }

  output[*io_out] = ' ';
  *io_out += 1U;
  *io_last_was_space = 1;
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
 * Builds a robust title-normalized string used for fuzzy title matching.
 */
void game_matcher_normalize_title(
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

  size_t out = 0U;
  int last_was_space = 1;
  for (const unsigned char *cursor = (const unsigned char *)input; *cursor != '\0';) {
    uint32_t codepoint = 0U;
    size_t advance = decode_utf8_codepoint(cursor, &codepoint);
    if (advance == 0U) {
      break;
    }
    cursor += advance;

    if (is_combining_mark(codepoint)) {
      continue;
    }

    if (codepoint < 0x80U) {
      unsigned char c = (unsigned char)codepoint;
      if (isalnum(c)) {
        if ((out + 1U) < output_size) {
          output[out++] = (char)tolower(c);
          last_was_space = 0;
        }
      } else {
        append_collapsed_space(output, output_size, &out, &last_was_space);
      }
      continue;
    }

    if ((codepoint == 0x00A9U) || (codepoint == 0x00AEU) || (codepoint == 0x2122U)) {
      append_collapsed_space(output, output_size, &out, &last_was_space);
      continue;
    }

    if ((codepoint >= 0xFF10U) && (codepoint <= 0xFF19U)) {
      if ((out + 1U) < output_size) {
        output[out++] = (char)('0' + (char)(codepoint - 0xFF10U));
        last_was_space = 0;
      }
      continue;
    }
    if ((codepoint >= 0xFF21U) && (codepoint <= 0xFF3AU)) {
      if ((out + 1U) < output_size) {
        output[out++] = (char)('a' + (char)(codepoint - 0xFF21U));
        last_was_space = 0;
      }
      continue;
    }
    if ((codepoint >= 0xFF41U) && (codepoint <= 0xFF5AU)) {
      if ((out + 1U) < output_size) {
        output[out++] = (char)('a' + (char)(codepoint - 0xFF41U));
        last_was_space = 0;
      }
      continue;
    }

    char folded[8];
    size_t folded_len = fold_roman_codepoint(codepoint, folded, sizeof(folded));
    if (folded_len == 0U) {
      folded_len = fold_latin_diacritic(codepoint, folded, sizeof(folded));
    }
    if (folded_len > 0U) {
      for (size_t i = 0U; (i < folded_len) && ((out + 1U) < output_size); ++i) {
        output[out++] = folded[i];
        last_was_space = 0;
      }
      continue;
    }

    append_collapsed_space(output, output_size, &out, &last_was_space);
  }

  while ((out > 0U) && (output[out - 1U] == ' ')) {
    out--;
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
 * qsort helper for fixed-width lowercase token buffers.
 */
static int compare_token_buffers(const void *lhs, const void *rhs) {
  return strcmp((const char *)lhs, (const char *)rhs);
}

/*
 * Returns non-zero when token contains only decimal digits.
 */
static int token_is_digits(const char *token) {
  if (!has_text(token)) {
    return 0;
  }

  for (const char *cursor = token; *cursor != '\0'; ++cursor) {
    if (!isdigit((unsigned char)*cursor)) {
      return 0;
    }
  }
  return 1;
}

/*
 * Returns roman numeral value for one ascii char, or 0 when unsupported.
 */
static int roman_char_value(char c) {
  switch ((char)tolower((unsigned char)c)) {
    case 'i':
      return 1;
    case 'v':
      return 5;
    case 'x':
      return 10;
    case 'l':
      return 50;
    case 'c':
      return 100;
    case 'd':
      return 500;
    case 'm':
      return 1000;
    default:
      return 0;
  }
}

/*
 * Parses one roman numeral token into integer.
 */
static int parse_roman_numeral(const char *token, int *out_value) {
  if (!has_text(token) || (out_value == NULL)) {
    return -1;
  }

  int total = 0;
  int prev = 0;
  int length = 0;
  for (const char *cursor = token; *cursor != '\0'; ++cursor) {
    int value = roman_char_value(*cursor);
    if (value <= 0) {
      return -1;
    }
    if (value > prev) {
      total += value - (2 * prev);
    } else {
      total += value;
    }
    prev = value;
    length++;
  }

  if ((length <= 0) || (length > 10) || (total <= 0) || (total > 3999)) {
    return -1;
  }

  *out_value = total;
  return 0;
}

/*
 * Normalizes number-like token variants (roman <-> arabic) for set comparison.
 */
static void canonicalize_numeric_token(const char *token, char *output, size_t output_size) {
  if ((output == NULL) || (output_size == 0U)) {
    return;
  }
  output[0] = '\0';
  if (!has_text(token)) {
    return;
  }

  int numeric_value = 0;
  if (token_is_digits(token)) {
    numeric_value = atoi(token);
    if (numeric_value > 0) {
      snprintf(output, output_size, "%d", numeric_value);
      return;
    }
  } else if (parse_roman_numeral(token, &numeric_value) == 0) {
    snprintf(output, output_size, "%d", numeric_value);
    return;
  }

  snprintf(output, output_size, "%s", token);
}

/*
 * Splits one normalized space-delimited string into unique canonical tokens.
 */
static int split_normalized_tokens(
    const char *normalized_text,
    char out_tokens[GAME_MATCHER_MAX_TITLE_TOKENS][GAME_MATCHER_MAX_TITLE_TOKEN_LEN]) {
  if (out_tokens == NULL) {
    return 0;
  }
  for (int i = 0; i < GAME_MATCHER_MAX_TITLE_TOKENS; ++i) {
    out_tokens[i][0] = '\0';
  }

  if (!has_text(normalized_text)) {
    return 0;
  }

  int token_count = 0;
  char token[GAME_MATCHER_MAX_TITLE_TOKEN_LEN];
  size_t token_len = 0U;
  for (const char *cursor = normalized_text;; ++cursor) {
    const char c = *cursor;
    if ((c != ' ') && (c != '\0')) {
      if ((token_len + 1U) < sizeof(token)) {
        token[token_len++] = c;
      }
      continue;
    }

    if (token_len > 0U) {
      token[token_len] = '\0';
      char canonical[GAME_MATCHER_MAX_TITLE_TOKEN_LEN];
      canonicalize_numeric_token(token, canonical, sizeof(canonical));
      if (has_text(canonical)) {
        int exists = 0;
        for (int i = 0; i < token_count; ++i) {
          if (sync_string_ieq(out_tokens[i], canonical)) {
            exists = 1;
            break;
          }
        }
        if (!exists && (token_count < GAME_MATCHER_MAX_TITLE_TOKENS)) {
          snprintf(out_tokens[token_count], GAME_MATCHER_MAX_TITLE_TOKEN_LEN, "%s", canonical);
          token_count++;
        }
      }
      token_len = 0U;
    }

    if (c == '\0') {
      break;
    }
  }

  return token_count;
}

/*
 * Builds one deterministic sorted-token string for token-sort fuzzy matching.
 */
static void build_sorted_token_string(
    char tokens[GAME_MATCHER_MAX_TITLE_TOKENS][GAME_MATCHER_MAX_TITLE_TOKEN_LEN],
    int token_count,
    char *output,
    size_t output_size) {
  if ((output == NULL) || (output_size == 0U)) {
    return;
  }
  output[0] = '\0';

  if ((tokens == NULL) || (token_count <= 0)) {
    return;
  }

  qsort(tokens, (size_t)token_count, sizeof(tokens[0]), compare_token_buffers);
  size_t out = 0U;
  for (int i = 0; i < token_count; ++i) {
    const size_t token_len = strlen(tokens[i]);
    if (token_len == 0U) {
      continue;
    }

    if ((i > 0) && ((out + 1U) < output_size)) {
      output[out++] = ' ';
    }
    for (size_t j = 0U; (j < token_len) && ((out + 1U) < output_size); ++j) {
      output[out++] = tokens[i][j];
    }
  }
  output[out] = '\0';
}

/*
 * Computes a similarity percentage using bounded Levenshtein distance.
 */
static int levenshtein_similarity_percent(const char *lhs, const char *rhs) {
  if (!has_text(lhs) && !has_text(rhs)) {
    return 100;
  }
  if (!has_text(lhs) || !has_text(rhs)) {
    return 0;
  }

  const size_t lhs_len = strlen(lhs);
  const size_t rhs_len = strlen(rhs);
  if ((lhs_len == 0U) || (rhs_len == 0U)) {
    return 0;
  }
  if ((lhs_len > ROMM_GAME_TITLE_LEN) || (rhs_len > ROMM_GAME_TITLE_LEN)) {
    return 0;
  }

  int prev[ROMM_GAME_TITLE_LEN + 1];
  int curr[ROMM_GAME_TITLE_LEN + 1];
  for (size_t j = 0U; j <= rhs_len; ++j) {
    prev[j] = (int)j;
  }

  for (size_t i = 1U; i <= lhs_len; ++i) {
    curr[0] = (int)i;
    for (size_t j = 1U; j <= rhs_len; ++j) {
      int substitution_cost = (lhs[i - 1U] == rhs[j - 1U]) ? 0 : 1;
      int deletion = prev[j] + 1;
      int insertion = curr[j - 1U] + 1;
      int substitution = prev[j - 1U] + substitution_cost;
      int best = deletion;
      if (insertion < best) {
        best = insertion;
      }
      if (substitution < best) {
        best = substitution;
      }
      curr[j] = best;
    }
    for (size_t j = 0U; j <= rhs_len; ++j) {
      prev[j] = curr[j];
    }
  }

  const int distance = prev[rhs_len];
  const int max_len = (lhs_len > rhs_len) ? (int)lhs_len : (int)rhs_len;
  if (max_len <= 0) {
    return 0;
  }

  int similarity = 100 - ((distance * 100) / max_len);
  if (similarity < 0) {
    similarity = 0;
  }
  if (similarity > 100) {
    similarity = 100;
  }
  return similarity;
}

/*
 * Counts overlapping tokens between two canonical token sets.
 */
static int count_token_overlap(
    char lhs_tokens[GAME_MATCHER_MAX_TITLE_TOKENS][GAME_MATCHER_MAX_TITLE_TOKEN_LEN],
    int lhs_count,
    char rhs_tokens[GAME_MATCHER_MAX_TITLE_TOKENS][GAME_MATCHER_MAX_TITLE_TOKEN_LEN],
    int rhs_count) {
  if ((lhs_tokens == NULL) || (rhs_tokens == NULL) || (lhs_count <= 0) || (rhs_count <= 0)) {
    return 0;
  }

  int overlap = 0;
  for (int i = 0; i < lhs_count; ++i) {
    for (int j = 0; j < rhs_count; ++j) {
      if (sync_string_ieq(lhs_tokens[i], rhs_tokens[j])) {
        overlap++;
        break;
      }
    }
  }
  return overlap;
}

/*
 * Attempts to parse a number from short alias token patterns like "r4".
 */
static int parse_alias_token_number(const char *token, int *out_value) {
  if (!has_text(token) || (out_value == NULL)) {
    return -1;
  }

  const size_t length = strlen(token);
  if (length < 2U) {
    return -1;
  }

  size_t split = 0U;
  while ((split < length) && isalpha((unsigned char)token[split])) {
    split++;
  }
  if ((split == 0U) || (split > 2U) || (split >= length)) {
    return -1;
  }

  for (size_t i = split; i < length; ++i) {
    if (!isdigit((unsigned char)token[i])) {
      return -1;
    }
  }

  *out_value = atoi(token + split);
  return (*out_value > 0) ? 0 : -1;
}

/*
 * Extracts up to max_values normalized sequel/series numbers from one title.
 */
static int extract_title_numbers(const char *normalized_title, int *out_values, int max_values) {
  if ((out_values == NULL) || (max_values <= 0)) {
    return 0;
  }
  for (int i = 0; i < max_values; ++i) {
    out_values[i] = 0;
  }

  char tokens[GAME_MATCHER_MAX_TITLE_TOKENS][GAME_MATCHER_MAX_TITLE_TOKEN_LEN];
  int token_count = split_normalized_tokens(normalized_title, tokens);
  int value_count = 0;
  for (int i = 0; i < token_count; ++i) {
    int value = 0;
    if (token_is_digits(tokens[i])) {
      value = atoi(tokens[i]);
    } else if (parse_roman_numeral(tokens[i], &value) == 0) {
      /* Parsed as roman numeral. */
    } else if (parse_alias_token_number(tokens[i], &value) < 0) {
      continue;
    }

    if (value <= 0) {
      continue;
    }

    int exists = 0;
    for (int j = 0; j < value_count; ++j) {
      if (out_values[j] == value) {
        exists = 1;
        break;
      }
    }
    if (!exists && (value_count < max_values)) {
      out_values[value_count++] = value;
    }
  }

  return value_count;
}

/*
 * Returns non-zero when both numeric sets share at least one value.
 */
static int title_numbers_overlap(
    const int *lhs_values,
    int lhs_count,
    const int *rhs_values,
    int rhs_count) {
  if ((lhs_values == NULL) || (rhs_values == NULL) || (lhs_count <= 0) || (rhs_count <= 0)) {
    return 0;
  }

  for (int i = 0; i < lhs_count; ++i) {
    for (int j = 0; j < rhs_count; ++j) {
      if (lhs_values[i] == rhs_values[j]) {
        return 1;
      }
    }
  }
  return 0;
}

/*
 * Computes one title similarity score with detailed component breakdown.
 */
static int score_title_similarity(
    const char *normalized_local_title,
    const char *normalized_candidate_label,
    const char *normalized_local_game_id,
    const GameMatcherRomCandidate *candidate,
    TitleSimilarityBreakdown *out_breakdown) {
  if ((out_breakdown == NULL) || !has_text(normalized_local_title) || !has_text(normalized_candidate_label)) {
    return 0;
  }

  memset(out_breakdown, 0, sizeof(*out_breakdown));

  char local_tokens[GAME_MATCHER_MAX_TITLE_TOKENS][GAME_MATCHER_MAX_TITLE_TOKEN_LEN];
  char label_tokens[GAME_MATCHER_MAX_TITLE_TOKENS][GAME_MATCHER_MAX_TITLE_TOKEN_LEN];
  int local_count = split_normalized_tokens(normalized_local_title, local_tokens);
  int label_count = split_normalized_tokens(normalized_candidate_label, label_tokens);
  if ((local_count <= 0) || (label_count <= 0)) {
    return 0;
  }

  const int overlap = count_token_overlap(local_tokens, local_count, label_tokens, label_count);
  const int union_count = local_count + label_count - overlap;
  const int jaccard = (union_count > 0) ? ((overlap * 100) / union_count) : 0;
  const int local_coverage = (local_count > 0) ? ((overlap * 100) / local_count) : 0;
  const int label_coverage = (label_count > 0) ? ((overlap * 100) / label_count) : 0;

  out_breakdown->overlap_tokens = overlap;
  out_breakdown->local_token_count = local_count;
  out_breakdown->label_token_count = label_count;
  out_breakdown->token_set_score = jaccard + ((local_coverage + label_coverage) / 2);

  char local_sorted[ROMM_GAME_TITLE_LEN];
  char label_sorted[ROMM_GAME_TITLE_LEN];
  build_sorted_token_string(local_tokens, local_count, local_sorted, sizeof(local_sorted));
  build_sorted_token_string(label_tokens, label_count, label_sorted, sizeof(label_sorted));
  out_breakdown->token_sort_score = levenshtein_similarity_percent(local_sorted, label_sorted) / 2;
  out_breakdown->levenshtein_score =
      levenshtein_similarity_percent(normalized_local_title, normalized_candidate_label) / 2;

  if (sync_string_ieq(normalized_local_title, normalized_candidate_label)) {
    out_breakdown->exact_bonus = 260;
  } else if ((strlen(normalized_local_title) >= 5U) &&
             (strlen(normalized_candidate_label) >= 5U) &&
             ((strstr(normalized_local_title, normalized_candidate_label) != NULL) ||
              (strstr(normalized_candidate_label, normalized_local_title) != NULL))) {
    out_breakdown->contains_bonus = 70;
  }

  if (has_text(normalized_local_game_id) && (candidate != NULL) &&
      (serial_value_matches(candidate->serial, normalized_local_game_id) ||
       serial_list_matches(candidate->serials, normalized_local_game_id))) {
    out_breakdown->serial_bonus = 35;
  }

  int local_numbers[8];
  int candidate_numbers[8];
  int local_number_count = extract_title_numbers(normalized_local_title, local_numbers, 8);
  int candidate_number_count = extract_title_numbers(normalized_candidate_label, candidate_numbers, 8);
  if ((local_number_count > 0) &&
      (candidate_number_count > 0) &&
      !title_numbers_overlap(local_numbers, local_number_count, candidate_numbers, candidate_number_count)) {
    out_breakdown->sequel_penalty = 180;
  }

  int total = out_breakdown->exact_bonus +
              out_breakdown->contains_bonus +
              out_breakdown->token_set_score +
              out_breakdown->token_sort_score +
              out_breakdown->levenshtein_score +
              out_breakdown->serial_bonus -
              out_breakdown->sequel_penalty;

  if (total < 0) {
    total = 0;
  }
  out_breakdown->total_score = total;
  return total;
}

/*
 * Scores one candidate title across all relevant catalog labels/aliases.
 */
static int score_candidate_title_match(
    const SyncSaveDescriptor *local_item,
    const GameMatcherRomCandidate *candidate,
    const char *normalized_local_title,
    const char *normalized_local_game_id,
    GameMatcherMatchField *out_field,
    TitleSimilarityBreakdown *out_breakdown,
    char *out_normalized_label,
    size_t out_normalized_label_size) {
  if ((local_item == NULL) || (candidate == NULL) ||
      (out_field == NULL) || (out_breakdown == NULL) ||
      (out_normalized_label == NULL) || (out_normalized_label_size == 0U)) {
    return 0;
  }

  *out_field = GAME_MATCHER_MATCH_FIELD_NONE;
  memset(out_breakdown, 0, sizeof(*out_breakdown));
  out_normalized_label[0] = '\0';

  int best_score = 0;
  const struct {
    const char *label;
    GameMatcherMatchField field;
  } direct_labels[] = {
      {candidate->fs_name_no_tags, GAME_MATCHER_MATCH_FIELD_FS_NAME_NO_TAGS},
      {candidate->name, GAME_MATCHER_MATCH_FIELD_NAME},
      {has_text(candidate->fs_name_no_ext) ? candidate->fs_name_no_ext : candidate->fs_name, GAME_MATCHER_MATCH_FIELD_FS_NAME_NO_EXT}};

  for (size_t i = 0U; i < (sizeof(direct_labels) / sizeof(direct_labels[0])); ++i) {
    char normalized_label[GAME_MATCHER_MAX_ROM_LABEL_LEN];
    game_matcher_normalize_title(direct_labels[i].label, normalized_label, sizeof(normalized_label));
    if (strlen(normalized_label) < GAME_MATCHER_TITLE_MIN_LEN) {
      continue;
    }

    TitleSimilarityBreakdown details;
    int score = score_title_similarity(
        normalized_local_title,
        normalized_label,
        normalized_local_game_id,
        candidate,
        &details);
    if (score > best_score) {
      best_score = score;
      *out_field = direct_labels[i].field;
      *out_breakdown = details;
      snprintf(out_normalized_label, out_normalized_label_size, "%s", normalized_label);
    }
  }

  if (has_text(candidate->alternative_names)) {
    char alias[GAME_MATCHER_MAX_ALTERNATIVE_NAMES_LEN];
    size_t alias_len = 0U;
    for (const char *cursor = candidate->alternative_names;; ++cursor) {
      const char c = *cursor;
      if ((c != '|') && (c != '\0') && ((alias_len + 1U) < sizeof(alias))) {
        alias[alias_len++] = c;
      }
      if ((c == '|') || (c == '\0')) {
        if (alias_len > 0U) {
          alias[alias_len] = '\0';
          char normalized_alias[GAME_MATCHER_MAX_ROM_LABEL_LEN];
          game_matcher_normalize_title(alias, normalized_alias, sizeof(normalized_alias));
          if (strlen(normalized_alias) >= GAME_MATCHER_TITLE_MIN_LEN) {
            TitleSimilarityBreakdown details;
            int score = score_title_similarity(
                normalized_local_title,
                normalized_alias,
                normalized_local_game_id,
                candidate,
                &details);
            if (score > best_score) {
              best_score = score;
              *out_field = GAME_MATCHER_MATCH_FIELD_ALTERNATIVE_NAMES;
              *out_breakdown = details;
              snprintf(out_normalized_label, out_normalized_label_size, "%s", normalized_alias);
            }
          }
          alias_len = 0U;
        }
        if (c == '\0') {
          break;
        }
      }
    }
  }

  if (app_log_is_enabled(APP_LOG_LEVEL_DEBUG)) {
    app_log_write(
        APP_LOG_LEVEL_DEBUG,
        "matcher",
        "title-candidate game=%s rom_id=%d score=%d field=%s label_norm=%s exact=%d contains=%d token=%d sort=%d lev=%d serial=%d sequel_penalty=%d overlap=%d/%d-%d",
        local_item->game_id,
        candidate->rom_id,
        best_score,
        game_matcher_match_field_str(*out_field),
        out_normalized_label,
        out_breakdown->exact_bonus,
        out_breakdown->contains_bonus,
        out_breakdown->token_set_score,
        out_breakdown->token_sort_score,
        out_breakdown->levenshtein_score,
        out_breakdown->serial_bonus,
        out_breakdown->sequel_penalty,
        out_breakdown->overlap_tokens,
        out_breakdown->local_token_count,
        out_breakdown->label_token_count);
  }

  return best_score;
}

/*
 * Tracks unique rom_id matches and marks ambiguous collisions deterministically.
 */
static void register_unique_match(
    int rom_id,
    GameMatcherMatchField field,
    int *io_best_rom_id,
    GameMatcherMatchField *io_best_field,
    int *io_is_ambiguous) {
  if ((io_best_rom_id == NULL) ||
      (io_best_field == NULL) ||
      (io_is_ambiguous == NULL) ||
      (rom_id <= 0)) {
    return;
  }

  if (*io_best_rom_id < 0) {
    *io_best_rom_id = rom_id;
    *io_best_field = field;
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
    int catalog_count,
    GameMatcherResolution *out_resolution) {
  if ((local_item == NULL) || (catalog == NULL) || (catalog_count <= 0)) {
    return GAME_MATCHER_NO_MATCH;
  }

  char normalized_game_id[ROMM_GAME_ID_LEN];
  normalize_identifier(local_item->game_id, normalized_game_id, sizeof(normalized_game_id));
  if (strlen(normalized_game_id) < 4U) {
    return GAME_MATCHER_NO_MATCH;
  }

  int best_rom_id = -1;
  GameMatcherMatchField best_field = GAME_MATCHER_MATCH_FIELD_NONE;
  int ambiguous = 0;
  for (int i = 0; i < catalog_count; ++i) {
    const GameMatcherRomCandidate *candidate = &catalog[i];
    if ((candidate->rom_id <= 0) || !candidate_is_ps1_eligible(candidate)) {
      continue;
    }

    GameMatcherMatchField field = GAME_MATCHER_MATCH_FIELD_NONE;
    if (serial_value_matches(candidate->serial, normalized_game_id)) {
      field = GAME_MATCHER_MATCH_FIELD_SERIAL;
    } else if (serial_list_matches(candidate->serials, normalized_game_id)) {
      field = GAME_MATCHER_MATCH_FIELD_SERIALS;
    }

    if (field != GAME_MATCHER_MATCH_FIELD_NONE) {
      register_unique_match(candidate->rom_id, field, &best_rom_id, &best_field, &ambiguous);
    }
  }

  if (ambiguous) {
    game_matcher_resolution_set(
        out_resolution,
        GAME_MATCHER_AMBIGUOUS,
        0,
        GAME_MATCHER_MATCH_STAGE_SERIAL,
        best_field);
    return GAME_MATCHER_AMBIGUOUS;
  }
  if (best_rom_id > 0) {
    game_matcher_resolution_set(
        out_resolution,
        best_rom_id,
        0,
        GAME_MATCHER_MATCH_STAGE_SERIAL,
        best_field);
    return best_rom_id;
  }
  return GAME_MATCHER_NO_MATCH;
}

/*
 * Stage 2: resolves by PARAM.SFO title matched to cleaned RomM labels.
 * Uses exact+fuzzy scoring (labels + aliases) with ambiguity guards.
 */
static int resolve_rom_id_by_title(
    const SyncSaveDescriptor *local_item,
    const GameMatcherRomCandidate *catalog,
    int catalog_count,
    GameMatcherResolution *out_resolution) {
  if ((local_item == NULL) || (catalog == NULL) || (catalog_count <= 0)) {
    return GAME_MATCHER_NO_MATCH;
  }

  char normalized_local_title[ROMM_GAME_TITLE_LEN];
  game_matcher_normalize_title(local_item->title, normalized_local_title, sizeof(normalized_local_title));
  if (strlen(normalized_local_title) < GAME_MATCHER_TITLE_MIN_LEN) {
    return GAME_MATCHER_NO_MATCH;
  }
  char normalized_local_game_id[ROMM_GAME_ID_LEN];
  normalize_identifier(local_item->game_id, normalized_local_game_id, sizeof(normalized_local_game_id));

  if (app_log_is_enabled(APP_LOG_LEVEL_DEBUG)) {
    app_log_write(
        APP_LOG_LEVEL_DEBUG,
        "matcher",
        "title-stage start game=%s title=%s normalized=%s threshold=%d margin=%d",
        local_item->game_id,
        local_item->title,
        normalized_local_title,
        GAME_MATCHER_TITLE_CONFIDENCE_THRESHOLD,
        GAME_MATCHER_TITLE_AMBIGUITY_MARGIN);
  }

  int best_rom_id = -1;
  int best_score = 0;
  int second_score = 0;
  int second_rom_id = -1;
  GameMatcherMatchField best_field = GAME_MATCHER_MATCH_FIELD_NONE;
  TitleSimilarityBreakdown best_breakdown;
  memset(&best_breakdown, 0, sizeof(best_breakdown));
  for (int i = 0; i < catalog_count; ++i) {
    const GameMatcherRomCandidate *candidate = &catalog[i];
    if ((candidate->rom_id <= 0) || !candidate_is_ps1_eligible(candidate)) {
      continue;
    }

    TitleSimilarityBreakdown details;
    GameMatcherMatchField field = GAME_MATCHER_MATCH_FIELD_NONE;
    char best_label[GAME_MATCHER_MAX_ROM_LABEL_LEN];
    int score = score_candidate_title_match(
        local_item,
        candidate,
        normalized_local_title,
        normalized_local_game_id,
        &field,
        &details,
        best_label,
        sizeof(best_label));

    if (score <= 0) {
      continue;
    }

    if (score > best_score) {
      second_score = best_score;
      second_rom_id = best_rom_id;
      best_score = score;
      best_rom_id = candidate->rom_id;
      best_field = field;
      best_breakdown = details;
      continue;
    }
    if ((candidate->rom_id != best_rom_id) && (score > second_score)) {
      second_score = score;
      second_rom_id = candidate->rom_id;
    }
  }

  if (best_score < GAME_MATCHER_TITLE_CONFIDENCE_THRESHOLD) {
    if (app_log_is_enabled(APP_LOG_LEVEL_DEBUG)) {
      app_log_write(
          APP_LOG_LEVEL_DEBUG,
          "matcher",
          "title-stage reject game=%s: best_score=%d below threshold=%d",
          local_item->game_id,
          best_score,
          GAME_MATCHER_TITLE_CONFIDENCE_THRESHOLD);
    }
    return GAME_MATCHER_NO_MATCH;
  }

  if ((second_rom_id > 0) &&
      (second_rom_id != best_rom_id) &&
      ((best_score - second_score) <= GAME_MATCHER_TITLE_AMBIGUITY_MARGIN)) {
    game_matcher_resolution_set(
        out_resolution,
        GAME_MATCHER_AMBIGUOUS,
        best_score,
        GAME_MATCHER_MATCH_STAGE_TITLE,
        best_field);
    if (app_log_is_enabled(APP_LOG_LEVEL_DEBUG)) {
      app_log_write(
          APP_LOG_LEVEL_DEBUG,
          "matcher",
          "title-stage ambiguous game=%s best_rom_id=%d score=%d second_rom_id=%d second_score=%d margin=%d",
          local_item->game_id,
          best_rom_id,
          best_score,
          second_rom_id,
          second_score,
          GAME_MATCHER_TITLE_AMBIGUITY_MARGIN);
    }
    return GAME_MATCHER_AMBIGUOUS;
  }

  if (best_rom_id > 0) {
    game_matcher_resolution_set(
        out_resolution,
        best_rom_id,
        best_score,
        GAME_MATCHER_MATCH_STAGE_TITLE,
        best_field);
    if (app_log_is_enabled(APP_LOG_LEVEL_DEBUG)) {
      app_log_write(
          APP_LOG_LEVEL_DEBUG,
          "matcher",
          "title-stage winner game=%s rom_id=%d score=%d field=%s exact=%d contains=%d token=%d sort=%d lev=%d serial=%d sequel_penalty=%d",
          local_item->game_id,
          best_rom_id,
          best_score,
          game_matcher_match_field_str(best_field),
          best_breakdown.exact_bonus,
          best_breakdown.contains_bonus,
          best_breakdown.token_set_score,
          best_breakdown.token_sort_score,
          best_breakdown.levenshtein_score,
          best_breakdown.serial_bonus,
          best_breakdown.sequel_penalty);
    }
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
    const GameMatcherRomCandidate *candidate,
    GameMatcherMatchField *out_field) {
  if ((candidate == NULL) || (candidate->rom_id <= 0)) {
    return 0;
  }

  char normalized_catalog_name_no_tags[GAME_MATCHER_MAX_ROM_LABEL_LEN];
  char normalized_catalog_name[GAME_MATCHER_MAX_ROM_LABEL_LEN];
  char normalized_catalog_display_name[GAME_MATCHER_MAX_ROM_LABEL_LEN];
  game_matcher_normalize_title(candidate->fs_name_no_tags, normalized_catalog_name_no_tags, sizeof(normalized_catalog_name_no_tags));
  if (has_text(candidate->fs_name_no_ext)) {
    game_matcher_normalize_title(candidate->fs_name_no_ext, normalized_catalog_name, sizeof(normalized_catalog_name));
  } else {
    normalize_filename_without_extension(candidate->fs_name, normalized_catalog_name, sizeof(normalized_catalog_name));
  }
  game_matcher_normalize_title(candidate->name, normalized_catalog_display_name, sizeof(normalized_catalog_display_name));

  if (!has_text(normalized_catalog_name_no_tags) &&
      !has_text(normalized_catalog_name) &&
      !has_text(normalized_catalog_display_name)) {
    return 0;
  }

  if (out_field != NULL) {
    *out_field = GAME_MATCHER_MATCH_FIELD_NONE;
  }

  const char *labels[3] = {
      normalized_catalog_name_no_tags,
      normalized_catalog_name,
      normalized_catalog_display_name};
  const GameMatcherMatchField fields[3] = {
      GAME_MATCHER_MATCH_FIELD_FS_NAME_NO_TAGS,
      GAME_MATCHER_MATCH_FIELD_FS_NAME_NO_EXT,
      GAME_MATCHER_MATCH_FIELD_NAME};

  int score = 0;
  for (size_t i = 0U; i < (sizeof(labels) / sizeof(labels[0])); ++i) {
    const char *label = labels[i];
    const int is_no_tags_label = (i == 0U);
    if (!has_text(label)) {
      continue;
    }

    if (has_text(normalized_local_filename) && (strlen(normalized_local_filename) >= 4U)) {
      if (sync_string_ieq(normalized_local_filename, label)) {
        int exact_score = is_no_tags_label ? 105 : 100;
        if (score < exact_score) {
          score = exact_score;
          if (out_field != NULL) {
            *out_field = fields[i];
          }
        }
      } else if ((strstr(label, normalized_local_filename) != NULL) ||
                 (strstr(normalized_local_filename, label) != NULL)) {
        int partial_score = is_no_tags_label ? 80 : 70;
        if (score < partial_score) {
          score = partial_score;
          if (out_field != NULL) {
            *out_field = fields[i];
          }
        }
      }
    }

    if (has_text(normalized_local_game_id) && (strlen(normalized_local_game_id) >= 4U)) {
      if (sync_string_ieq(normalized_local_game_id, label)) {
        if (score < 95) {
          score = 95;
          if (out_field != NULL) {
            *out_field = fields[i];
          }
        }
      } else if (strstr(label, normalized_local_game_id) != NULL) {
        if (score < 90) {
          score = 90;
          if (out_field != NULL) {
            *out_field = fields[i];
          }
        }
      }
    }

    if (has_text(normalized_local_title) && (strlen(normalized_local_title) >= 8U)) {
      if (sync_string_ieq(normalized_local_title, label)) {
        int exact_score = is_no_tags_label ? 90 : 85;
        if (score < exact_score) {
          score = exact_score;
          if (out_field != NULL) {
            *out_field = fields[i];
          }
        }
      } else if ((strstr(label, normalized_local_title) != NULL) ||
                 (strstr(normalized_local_title, label) != NULL)) {
        int partial_score = is_no_tags_label ? 80 : 75;
        if (score < partial_score) {
          score = partial_score;
          if (out_field != NULL) {
            *out_field = fields[i];
          }
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
    int catalog_count,
    GameMatcherResolution *out_resolution) {
  if ((local_item == NULL) || (catalog == NULL) || (catalog_count <= 0)) {
    return GAME_MATCHER_NO_MATCH;
  }

  char normalized_local_filename[ROMM_SYNC_MAX_FILENAME_LEN];
  extract_local_filename_pattern(local_item, normalized_local_filename, sizeof(normalized_local_filename));

  char normalized_local_game_id[ROMM_GAME_ID_LEN];
  normalize_identifier(local_item->game_id, normalized_local_game_id, sizeof(normalized_local_game_id));
  char normalized_local_title[ROMM_GAME_TITLE_LEN];
  game_matcher_normalize_title(local_item->title, normalized_local_title, sizeof(normalized_local_title));

  if (!has_text(normalized_local_filename) &&
      !has_text(normalized_local_game_id) &&
      !has_text(normalized_local_title)) {
    return GAME_MATCHER_NO_MATCH;
  }

  int best_score = 0;
  int best_rom_id = -1;
  GameMatcherMatchField best_field = GAME_MATCHER_MATCH_FIELD_NONE;
  int ambiguous = 0;
  for (int i = 0; i < catalog_count; ++i) {
    const GameMatcherRomCandidate *candidate = &catalog[i];
    if (!candidate_is_ps1_eligible(candidate)) {
      continue;
    }

    GameMatcherMatchField field = GAME_MATCHER_MATCH_FIELD_NONE;
    int score = score_filename_pattern_match(
        normalized_local_filename,
        normalized_local_game_id,
        normalized_local_title,
        candidate,
        &field);
    if (score <= 0) {
      continue;
    }

    if (score > best_score) {
      best_score = score;
      best_rom_id = candidate->rom_id;
      best_field = field;
      ambiguous = 0;
      continue;
    }

    if ((score == best_score) && (best_rom_id != candidate->rom_id)) {
      ambiguous = 1;
    }
  }

  if ((best_score > 0) && !ambiguous && (best_rom_id > 0)) {
    game_matcher_resolution_set(
        out_resolution,
        best_rom_id,
        best_score,
        GAME_MATCHER_MATCH_STAGE_FILENAME_PATTERNS,
        best_field);
    return best_rom_id;
  }
  if (ambiguous) {
    game_matcher_resolution_set(
        out_resolution,
        GAME_MATCHER_AMBIGUOUS,
        best_score,
        GAME_MATCHER_MATCH_STAGE_FILENAME_PATTERNS,
        best_field);
    return GAME_MATCHER_AMBIGUOUS;
  }
  return GAME_MATCHER_NO_MATCH;
}

/*
 * Finds the best retained remote candidate using stable identity rules:
 * exact rom+slot+filename, then rom+slot compatibility, then rom only.
 * The final rom-only fallback exists because remote save listing already keeps
 * only the newest server save per rom_id regardless of slot.
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

  for (int i = 0; i < remote_count; ++i) {
    const SyncSaveDescriptor *candidate = &remote_items[i];
    if (rom_id_matches(local_item, candidate)) {
      return i;
    }
  }

  return -1;
}

/*
 * Resolves a local PS1 save to one RomM rom_id through deterministic fallbacks.
 */
int game_matcher_resolve_rom_id_with_details(
    const SyncSaveDescriptor *local_item,
    const GameMatcherRomCandidate *catalog,
    int catalog_count,
    GameMatcherResolution *out_resolution) {
  game_matcher_resolution_init(out_resolution);

  if ((local_item == NULL) || (catalog == NULL) || (catalog_count <= 0)) {
    return -1;
  }

  if (local_item->rom_id > 0) {
    game_matcher_resolution_set(
        out_resolution,
        local_item->rom_id,
        0,
        GAME_MATCHER_MATCH_STAGE_EXISTING_ROM_ID,
        GAME_MATCHER_MATCH_FIELD_ROM_ID);
    return local_item->rom_id;
  }

  int rom_id = resolve_rom_id_by_serial(local_item, catalog, catalog_count, out_resolution);
  if (rom_id != GAME_MATCHER_NO_MATCH) {
    return rom_id;
  }

  rom_id = resolve_rom_id_by_title(local_item, catalog, catalog_count, out_resolution);
  if (rom_id != GAME_MATCHER_NO_MATCH) {
    return rom_id;
  }

  rom_id = resolve_rom_id_by_filename_patterns(local_item, catalog, catalog_count, out_resolution);
  if (rom_id != GAME_MATCHER_NO_MATCH) {
    return rom_id;
  }

  return -1;
}

int game_matcher_resolve_rom_id(
    const SyncSaveDescriptor *local_item,
    const GameMatcherRomCandidate *catalog,
    int catalog_count) {
  return game_matcher_resolve_rom_id_with_details(local_item, catalog, catalog_count, NULL);
}
