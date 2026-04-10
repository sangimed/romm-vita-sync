#include "sync_types.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/*
 * Converts an ASCII letter to lowercase.
 */
static char ascii_lower(char value) {
  if ((value >= 'A') && (value <= 'Z')) {
    return (char)(value - 'A' + 'a');
  }
  return value;
}

/*
 * Returns non-zero when the given text is non-null and non-empty.
 */
static int has_text(const char *text) {
  return (text != NULL) && (text[0] != '\0');
}

/*
 * Parses exactly N decimal digits into an integer.
 */
static int parse_n_digits(const char *text, size_t count, int *out_value) {
  if ((text == NULL) || (out_value == NULL)) {
    return -1;
  }

  int value = 0;
  for (size_t i = 0; i < count; ++i) {
    unsigned char c = (unsigned char)text[i];
    if (!isdigit(c)) {
      return -1;
    }
    value = (value * 10) + (int)(c - '0');
  }

  *out_value = value;
  return 0;
}

/*
 * Returns non-zero when the given year is leap in Gregorian calendar.
 */
static int is_leap_year(int year) {
  if ((year % 4) != 0) {
    return 0;
  }
  if ((year % 100) != 0) {
    return 1;
  }
  return (year % 400) == 0;
}

/*
 * Returns the number of days in a specific month/year pair.
 */
static int days_in_month(int year, int month) {
  static const int kDaysPerMonth[] = {
      31, 28, 31, 30, 31, 30,
      31, 31, 30, 31, 30, 31};

  if ((month < 1) || (month > 12)) {
    return 0;
  }

  if ((month == 2) && is_leap_year(year)) {
    return 29;
  }

  return kDaysPerMonth[month - 1];
}

/*
 * Converts a Gregorian date to number of days since Unix epoch (1970-01-01).
 * This algorithm is deterministic and does not depend on libc timezone state.
 */
static int64_t days_from_civil(int year, int month, int day) {
  year -= (month <= 2);
  int era = (year >= 0) ? (year / 400) : ((year - 399) / 400);
  unsigned year_of_era = (unsigned)(year - (era * 400));
  unsigned day_of_year = (unsigned)((153 * (month + ((month > 2) ? -3 : 9)) + 2) / 5 + day - 1);
  unsigned day_of_era = (year_of_era * 365U) + (year_of_era / 4U) - (year_of_era / 100U) + day_of_year;
  return ((int64_t)era * 146097LL) + (int64_t)day_of_era - 719468LL;
}

/*
 * Returns non-zero when both descriptors belong to the same PS1 game bucket.
 * Empty game identifiers are intentionally not grouped so unrelated saves do
 * not collapse into one selection when scanner metadata is incomplete.
 */
static int local_items_share_game_group(
    const SyncSaveDescriptor *lhs,
    const SyncSaveDescriptor *rhs) {
  if ((lhs == NULL) || (rhs == NULL)) {
    return 0;
  }

  if (!has_text(lhs->game_id) || !has_text(rhs->game_id)) {
    return 0;
  }

  return sync_string_ieq(lhs->game_id, rhs->game_id);
}

/*
 * Scores slot priority used only for deterministic equal-timestamp fallback.
 * Slot 0 must win ties so callers can warn when M0 is chosen over M1.
 */
static int slot_selection_priority(SyncSlot slot) {
  switch (slot) {
    case SYNC_SLOT_0:
      return 2;
    case SYNC_SLOT_1:
      return 1;
    case SYNC_SLOT_UNKNOWN:
    default:
      return 0;
  }
}

/*
 * Returns non-zero when candidate should replace current_best for one game.
 * The newest timestamp wins first; exact ties fall back to slot priority.
 */
static int local_item_is_better_sync_candidate(
    const SyncSaveDescriptor *candidate,
    const SyncSaveDescriptor *current_best) {
  if (candidate == NULL) {
    return 0;
  }
  if (current_best == NULL) {
    return 1;
  }

  if (candidate->timestamp_unix > current_best->timestamp_unix) {
    return 1;
  }
  if (candidate->timestamp_unix < current_best->timestamp_unix) {
    return 0;
  }

  return slot_selection_priority(candidate->slot) > slot_selection_priority(current_best->slot);
}

/*
 * Resets a save descriptor to a known empty state.
 */
void sync_save_descriptor_init(SyncSaveDescriptor *item) {
  if (item == NULL) {
    return;
  }

  memset(item, 0, sizeof(*item));
  item->rom_id = -1;
  item->remote_id = -1;
  item->slot = SYNC_SLOT_UNKNOWN;
}

/*
 * Resets a sync-state entry to a known empty state.
 */
void sync_state_entry_init(SyncStateEntry *entry) {
  if (entry == NULL) {
    return;
  }

  memset(entry, 0, sizeof(*entry));
  entry->slot = SYNC_SLOT_UNKNOWN;
}

/*
 * Case-insensitive string equality for ASCII identifiers.
 */
int sync_string_ieq(const char *lhs, const char *rhs) {
  if (lhs == rhs) {
    return 1;
  }
  if ((lhs == NULL) || (rhs == NULL)) {
    return 0;
  }

  while ((*lhs != '\0') && (*rhs != '\0')) {
    if (ascii_lower(*lhs) != ascii_lower(*rhs)) {
      return 0;
    }
    lhs++;
    rhs++;
  }

  return (*lhs == '\0') && (*rhs == '\0');
}

/*
 * Extracts a basename from a path into a provided output buffer.
 */
int sync_extract_filename(const char *path, char *out_filename, size_t out_size) {
  if ((path == NULL) || (out_filename == NULL) || (out_size == 0U)) {
    return -1;
  }

  out_filename[0] = '\0';
  if (path[0] == '\0') {
    return -1;
  }

  const char *last_forward = strrchr(path, '/');
  const char *last_backslash = strrchr(path, '\\');
  const char *last_separator = last_forward;
  if ((last_backslash != NULL) && ((last_separator == NULL) || (last_backslash > last_separator))) {
    last_separator = last_backslash;
  }

  const char *filename = (last_separator == NULL) ? path : (last_separator + 1);
  if (filename[0] == '\0') {
    return -1;
  }

  size_t length = strlen(filename);
  if (length >= out_size) {
    length = out_size - 1U;
  }

  memcpy(out_filename, filename, length);
  out_filename[length] = '\0';
  return 0;
}

/*
 * Maps known PS1 memory-card filenames to slot identifiers.
 */
int sync_slot_from_filename(const char *filename, SyncSlot *out_slot) {
  if ((filename == NULL) || (out_slot == NULL)) {
    return -1;
  }

  char basename[ROMM_SYNC_MAX_FILENAME_LEN];
  if (sync_extract_filename(filename, basename, sizeof(basename)) < 0) {
    *out_slot = SYNC_SLOT_UNKNOWN;
    return -1;
  }

  if (sync_string_ieq(basename, "SCEVMC0.VMP")) {
    *out_slot = SYNC_SLOT_0;
    return 0;
  }

  if (sync_string_ieq(basename, "SCEVMC1.VMP")) {
    *out_slot = SYNC_SLOT_1;
    return 0;
  }

  *out_slot = SYNC_SLOT_UNKNOWN;
  return -2;
}

/*
 * Parses local scanner timestamps formatted as "YYYY-MM-DD HH:MM:SS".
 * The conversion is timezone-agnostic and deterministic.
 */
int sync_parse_local_timestamp(const char *timestamp, int64_t *out_timestamp_unix) {
  if ((timestamp == NULL) || (out_timestamp_unix == NULL)) {
    return -1;
  }

  if (strlen(timestamp) < 19U) {
    return -1;
  }

  if ((timestamp[4] != '-') || (timestamp[7] != '-') ||
      (timestamp[10] != ' ') || (timestamp[13] != ':') || (timestamp[16] != ':')) {
    return -1;
  }

  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;

  if ((parse_n_digits(timestamp + 0, 4, &year) < 0) ||
      (parse_n_digits(timestamp + 5, 2, &month) < 0) ||
      (parse_n_digits(timestamp + 8, 2, &day) < 0) ||
      (parse_n_digits(timestamp + 11, 2, &hour) < 0) ||
      (parse_n_digits(timestamp + 14, 2, &minute) < 0) ||
      (parse_n_digits(timestamp + 17, 2, &second) < 0)) {
    return -1;
  }

  if ((month < 1) || (month > 12)) {
    return -1;
  }
  if ((hour < 0) || (hour > 23) || (minute < 0) || (minute > 59) || (second < 0) || (second > 59)) {
    return -1;
  }

  int max_day = days_in_month(year, month);
  if ((day < 1) || (day > max_day)) {
    return -1;
  }

  int64_t days_since_epoch = days_from_civil(year, month, day);
  *out_timestamp_unix = (days_since_epoch * 86400LL) +
                        ((int64_t)hour * 3600LL) +
                        ((int64_t)minute * 60LL) +
                        (int64_t)second;
  return 0;
}

/*
 * Formats one deterministic sync timestamp for logs and diagnostics.
 * A zero/negative timestamp is rendered as "unknown".
 */
void sync_format_timestamp(int64_t timestamp_unix, char *out_timestamp, size_t out_size) {
  if ((out_timestamp == NULL) || (out_size == 0U)) {
    return;
  }

  if (timestamp_unix <= 0) {
    snprintf(out_timestamp, out_size, "unknown");
    return;
  }

  time_t raw = (time_t)timestamp_unix;
  struct tm *utc = gmtime(&raw);
  if (utc == NULL) {
    snprintf(out_timestamp, out_size, "%lld", (long long)timestamp_unix);
    return;
  }

  snprintf(
      out_timestamp,
      out_size,
      "%04d-%02d-%02d %02d:%02d:%02d",
      utc->tm_year + 1900,
      utc->tm_mon + 1,
      utc->tm_mday,
      utc->tm_hour,
      utc->tm_min,
      utc->tm_sec);
}

/*
 * Selects one local PS1 sync candidate per game using deterministic rules.
 * This prevents both local memory cards from being synchronized independently:
 * the newest local timestamp wins, and an exact tie falls back to slot 0 so
 * callers can surface a warning before mapping and transfer decisions begin.
 */
int sync_select_latest_local_per_game(
    const SyncSaveDescriptor *items,
    int item_count,
    int *out_selected_mask,
    SyncLocalSelectionReason *out_selection_reasons) {
  if ((items == NULL) || (item_count < 0) || (out_selected_mask == NULL)) {
    return -1;
  }

  for (int i = 0; i < item_count; ++i) {
    out_selected_mask[i] = 0;
    if (out_selection_reasons != NULL) {
      out_selection_reasons[i] = SYNC_LOCAL_SELECTION_NOT_SELECTED;
    }
  }

  int selected_count = 0;
  for (int i = 0; i < item_count; ++i) {
    int already_grouped = 0;
    for (int j = 0; j < i; ++j) {
      if (local_items_share_game_group(&items[i], &items[j])) {
        already_grouped = 1;
        break;
      }
    }
    if (already_grouped) {
      continue;
    }

    int best_index = i;
    int group_size = 1;
    for (int j = i + 1; j < item_count; ++j) {
      if (!local_items_share_game_group(&items[i], &items[j])) {
        continue;
      }

      group_size += 1;
      if (local_item_is_better_sync_candidate(&items[j], &items[best_index])) {
        best_index = j;
      }
    }

    SyncLocalSelectionReason reason = SYNC_LOCAL_SELECTION_ONLY_ITEM;
    if (group_size > 1) {
      int has_different_timestamp = 0;
      int has_equal_timestamp_slot1_peer = 0;
      for (int j = i; j < item_count; ++j) {
        if (j == best_index) {
          continue;
        }
        if (!local_items_share_game_group(&items[best_index], &items[j])) {
          continue;
        }

        if (items[j].timestamp_unix != items[best_index].timestamp_unix) {
          has_different_timestamp = 1;
          continue;
        }

        if ((items[best_index].slot == SYNC_SLOT_0) &&
            (items[j].slot == SYNC_SLOT_1)) {
          has_equal_timestamp_slot1_peer = 1;
        }
      }

      if (has_equal_timestamp_slot1_peer) {
        reason = SYNC_LOCAL_SELECTION_EQUAL_TIMESTAMP_PREFER_SLOT0;
      } else if (has_different_timestamp) {
        reason = SYNC_LOCAL_SELECTION_LATEST_TIMESTAMP;
      } else {
        reason = SYNC_LOCAL_SELECTION_DETERMINISTIC_FALLBACK;
      }
    }

    out_selected_mask[best_index] = 1;
    if (out_selection_reasons != NULL) {
      out_selection_reasons[best_index] = reason;
    }
    selected_count += 1;
  }

  return selected_count;
}

/*
 * Returns a short textual representation of a slot enum.
 */
const char *sync_slot_str(SyncSlot slot) {
  switch (slot) {
    case SYNC_SLOT_0:
      return "slot0";
    case SYNC_SLOT_1:
      return "slot1";
    case SYNC_SLOT_UNKNOWN:
    default:
      return "unknown";
  }
}

/*
 * Returns a short textual representation of an action enum.
 */
const char *sync_action_type_str(SyncActionType action) {
  switch (action) {
    case SYNC_ACTION_UPLOAD:
      return "upload";
    case SYNC_ACTION_DOWNLOAD:
      return "download";
    case SYNC_ACTION_SKIP:
      return "skip";
    case SYNC_ACTION_NONE:
    default:
      return "none";
  }
}

/*
 * Returns a short textual representation of a conflict enum.
 */
const char *sync_conflict_type_str(SyncConflictType conflict) {
  switch (conflict) {
    case SYNC_CONFLICT_REMOTE_NEWER:
      return "remote_newer";
    case SYNC_CONFLICT_LOCAL_NEWER:
      return "local_newer";
    case SYNC_CONFLICT_SAME_TIMESTAMP_DIFFERENT_CONTENT:
      return "same_timestamp_different_content";
    case SYNC_CONFLICT_SAME_ORIGIN_DEVICE:
      return "same_origin_device";
    case SYNC_CONFLICT_NONE:
    default:
      return "none";
  }
}
