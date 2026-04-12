#include "sync_state_store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backup_manager.h"

#define SYNC_STATE_FILE_VERSION "RVS_STATE_V1"

/*
 * Copies text safely into a fixed-size destination buffer.
 */
static void safe_copy(char *destination, size_t destination_size, const char *source) {
  if ((destination == NULL) || (destination_size == 0U)) {
    return;
  }

  if (source == NULL) {
    destination[0] = '\0';
    return;
  }

  snprintf(destination, destination_size, "%s", source);
}

/*
 * Removes trailing CR/LF characters from a line read from file.
 */
static void trim_line_endings(char *line) {
  if (line == NULL) {
    return;
  }

  size_t length = strlen(line);
  while (length > 0U) {
    char c = line[length - 1U];
    if ((c != '\n') && (c != '\r')) {
      break;
    }
    line[length - 1U] = '\0';
    length--;
  }
}

/*
 * Splits an in-place tab-delimited line into field pointers.
 */
static int split_tab_fields(char *line, char **out_fields, int max_fields) {
  if ((line == NULL) || (out_fields == NULL) || (max_fields <= 0)) {
    return 0;
  }

  int count = 0;
  char *cursor = line;
  while ((count < max_fields) && (cursor != NULL)) {
    out_fields[count++] = cursor;
    char *separator = strchr(cursor, '\t');
    if (separator == NULL) {
      break;
    }

    *separator = '\0';
    cursor = separator + 1;
  }

  return count;
}

/*
 * Parses a signed 64-bit integer from text.
 */
static int64_t parse_int64_str(const char *text, int *ok) {
  if (ok != NULL) {
    *ok = 0;
  }

  if (text == NULL) {
    return 0;
  }

  char *end = NULL;
  long long value = strtoll(text, &end, 10);
  if ((end == text) || ((end != NULL) && (*end != '\0'))) {
    return 0;
  }

  if (ok != NULL) {
    *ok = 1;
  }
  return (int64_t)value;
}

/*
 * Parses an unsigned 64-bit integer from text.
 */
static uint64_t parse_uint64_str(const char *text, int *ok) {
  if (ok != NULL) {
    *ok = 0;
  }

  if (text == NULL) {
    return 0;
  }

  char *end = NULL;
  unsigned long long value = strtoull(text, &end, 10);
  if ((end == text) || ((end != NULL) && (*end != '\0'))) {
    return 0;
  }

  if (ok != NULL) {
    *ok = 1;
  }
  return (uint64_t)value;
}

/*
 * Returns non-zero when slot matching should accept this pair.
 */
static int slot_key_matches(SyncSlot entry_slot, SyncSlot expected_slot) {
  if (expected_slot == SYNC_SLOT_UNKNOWN) {
    return 1;
  }
  return entry_slot == expected_slot;
}

/*
 * Compares a state entry key with requested lookup fields.
 */
static int entry_key_matches(
    const SyncStateEntry *entry,
    const char *game_id,
    const char *filename,
    SyncSlot slot) {
  if ((entry == NULL) || (filename == NULL) || (filename[0] == '\0')) {
    return 0;
  }

  if (!sync_string_ieq(entry->filename, filename)) {
    return 0;
  }

  if (!slot_key_matches(entry->slot, slot)) {
    return 0;
  }

  if ((game_id != NULL) && (game_id[0] != '\0') &&
      (entry->game_id[0] != '\0') &&
      !sync_string_ieq(entry->game_id, game_id)) {
    return 0;
  }

  return 1;
}

/*
 * Ensures the parent directory for a state file path exists.
 */
static int ensure_parent_directory(const char *path) {
  if ((path == NULL) || (path[0] == '\0')) {
    return SYNC_STATE_STORE_ERR_INVALID_ARGUMENT;
  }

  const char *last_forward = strrchr(path, '/');
  const char *last_backward = strrchr(path, '\\');
  const char *last_separator = last_forward;
  if ((last_backward != NULL) && ((last_separator == NULL) || (last_backward > last_separator))) {
    last_separator = last_backward;
  }

  if (last_separator == NULL) {
    return SYNC_STATE_STORE_OK;
  }

  size_t parent_length = (size_t)(last_separator - path);
  if (parent_length == 0U) {
    return SYNC_STATE_STORE_OK;
  }

  char parent_path[ROMM_MAX_PATH_LEN];
  if (parent_length >= sizeof(parent_path)) {
    return SYNC_STATE_STORE_ERR_INVALID_ARGUMENT;
  }

  memcpy(parent_path, path, parent_length);
  parent_path[parent_length] = '\0';

  int status = backup_manager_ensure_directory(parent_path);
  if (status != BACKUP_MANAGER_OK) {
    return SYNC_STATE_STORE_ERR_CREATE_DIRECTORY;
  }

  return SYNC_STATE_STORE_OK;
}

/*
 * Initializes the in-memory state store to an empty state.
 */
void sync_state_store_init(SyncStateStore *store) {
  if (store == NULL) {
    return;
  }

  memset(store, 0, sizeof(*store));
  for (int i = 0; i < ROMM_SYNC_MAX_STATE_ENTRIES; ++i) {
    store->entries[i].slot = SYNC_SLOT_UNKNOWN;
  }
}

/*
 * Sets the current device identifier stored with state metadata.
 */
void sync_state_store_set_device_id(SyncStateStore *store, const char *device_id) {
  if (store == NULL) {
    return;
  }

  safe_copy(store->device_id, sizeof(store->device_id), device_id);
}

/*
 * Loads sync state from disk.
 * Missing file is treated as an empty state (not an error).
 */
int sync_state_store_load(const char *path, SyncStateStore *out_store) {
  if ((path == NULL) || (out_store == NULL) || (path[0] == '\0')) {
    return SYNC_STATE_STORE_ERR_INVALID_ARGUMENT;
  }

  sync_state_store_init(out_store);

  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    if (errno == ENOENT) {
      return SYNC_STATE_STORE_OK;
    }
    return SYNC_STATE_STORE_ERR_OPEN;
  }

  char line[1024];
  if (fgets(line, sizeof(line), file) == NULL) {
    fclose(file);
    return SYNC_STATE_STORE_OK;
  }
  trim_line_endings(line);

  if (strcmp(line, SYNC_STATE_FILE_VERSION) != 0) {
    fclose(file);
    return SYNC_STATE_STORE_ERR_FORMAT;
  }

  while (fgets(line, sizeof(line), file) != NULL) {
    trim_line_endings(line);
    if (line[0] == '\0') {
      continue;
    }

    char *fields[8];
    int field_count = split_tab_fields(line, fields, (int)(sizeof(fields) / sizeof(fields[0])));
    if (field_count < 1) {
      continue;
    }

    if (strcmp(fields[0], "device_id") == 0) {
      if (field_count >= 2) {
        safe_copy(out_store->device_id, sizeof(out_store->device_id), fields[1]);
      }
      continue;
    }

    if (strcmp(fields[0], "entry") != 0) {
      continue;
    }

    if (field_count < 8) {
      fclose(file);
      return SYNC_STATE_STORE_ERR_FORMAT;
    }

    if (out_store->count >= ROMM_SYNC_MAX_STATE_ENTRIES) {
      fclose(file);
      return SYNC_STATE_STORE_ERR_FULL;
    }

    SyncStateEntry *entry = &out_store->entries[out_store->count];
    sync_state_entry_init(entry);

    safe_copy(entry->game_id, sizeof(entry->game_id), fields[1]);
    safe_copy(entry->filename, sizeof(entry->filename), fields[2]);

    int slot_ok = 0;
    int64_t slot_value = parse_int64_str(fields[3], &slot_ok);
    if (!slot_ok || (slot_value < (int64_t)SYNC_SLOT_UNKNOWN) || (slot_value > (int64_t)SYNC_SLOT_1)) {
      fclose(file);
      return SYNC_STATE_STORE_ERR_FORMAT;
    }
    entry->slot = (SyncSlot)slot_value;

    int size_ok = 0;
    entry->size_bytes = parse_uint64_str(fields[4], &size_ok);
    if (!size_ok) {
      fclose(file);
      return SYNC_STATE_STORE_ERR_FORMAT;
    }

    int ts_ok = 0;
    entry->timestamp_unix = parse_int64_str(fields[5], &ts_ok);
    if (!ts_ok) {
      fclose(file);
      return SYNC_STATE_STORE_ERR_FORMAT;
    }

    safe_copy(entry->origin_device, sizeof(entry->origin_device), fields[6]);

    int upload_ok = 0;
    entry->last_upload_unix = parse_int64_str(fields[7], &upload_ok);
    if (!upload_ok) {
      fclose(file);
      return SYNC_STATE_STORE_ERR_FORMAT;
    }

    out_store->count += 1;
  }

  if (ferror(file)) {
    fclose(file);
    return SYNC_STATE_STORE_ERR_READ;
  }

  fclose(file);
  return SYNC_STATE_STORE_OK;
}

/*
 * Persists current sync state to disk in a deterministic text format.
 */
int sync_state_store_save(const char *path, const SyncStateStore *store) {
  if ((path == NULL) || (store == NULL) || (path[0] == '\0')) {
    return SYNC_STATE_STORE_ERR_INVALID_ARGUMENT;
  }

  int ensure_status = ensure_parent_directory(path);
  if (ensure_status != SYNC_STATE_STORE_OK) {
    return ensure_status;
  }

  FILE *file = fopen(path, "wb");
  if (file == NULL) {
    return SYNC_STATE_STORE_ERR_OPEN;
  }

  int status = SYNC_STATE_STORE_OK;

  if (fprintf(file, "%s\n", SYNC_STATE_FILE_VERSION) < 0) {
    status = SYNC_STATE_STORE_ERR_WRITE;
  }

  if ((status == SYNC_STATE_STORE_OK) && (store->device_id[0] != '\0')) {
    if (fprintf(file, "device_id\t%s\n", store->device_id) < 0) {
      status = SYNC_STATE_STORE_ERR_WRITE;
    }
  }

  for (int i = 0; (status == SYNC_STATE_STORE_OK) && (i < store->count); ++i) {
    const SyncStateEntry *entry = &store->entries[i];
    if (fprintf(file,
                "entry\t%s\t%s\t%d\t%llu\t%lld\t%s\t%lld\n",
                entry->game_id,
                entry->filename,
                (int)entry->slot,
                (unsigned long long)entry->size_bytes,
                (long long)entry->timestamp_unix,
                entry->origin_device,
                (long long)entry->last_upload_unix) < 0) {
      status = SYNC_STATE_STORE_ERR_WRITE;
      break;
    }
  }

  if ((status == SYNC_STATE_STORE_OK) && (fflush(file) != 0)) {
    status = SYNC_STATE_STORE_ERR_WRITE;
  }

  if (fclose(file) != 0) {
    if (status == SYNC_STATE_STORE_OK) {
      status = SYNC_STATE_STORE_ERR_WRITE;
    }
  }

  return status;
}

/*
 * Finds a mutable state entry by key (game_id + filename + slot).
 */
SyncStateEntry *sync_state_store_find(
    SyncStateStore *store,
    const char *game_id,
    const char *filename,
    SyncSlot slot) {
  if (store == NULL) {
    return NULL;
  }

  for (int i = 0; i < store->count; ++i) {
    SyncStateEntry *entry = &store->entries[i];
    if (entry_key_matches(entry, game_id, filename, slot)) {
      return entry;
    }
  }

  return NULL;
}

/*
 * Finds a read-only state entry by key (game_id + filename + slot).
 */
const SyncStateEntry *sync_state_store_find_const(
    const SyncStateStore *store,
    const char *game_id,
    const char *filename,
    SyncSlot slot) {
  if (store == NULL) {
    return NULL;
  }

  for (int i = 0; i < store->count; ++i) {
    const SyncStateEntry *entry = &store->entries[i];
    if (entry_key_matches(entry, game_id, filename, slot)) {
      return entry;
    }
  }

  return NULL;
}

/*
 * Updates an existing entry or appends a new one when absent.
 */
int sync_state_store_upsert(SyncStateStore *store, const SyncStateEntry *entry) {
  if ((store == NULL) || (entry == NULL) || (entry->filename[0] == '\0')) {
    return SYNC_STATE_STORE_ERR_INVALID_ARGUMENT;
  }

  SyncStateEntry *existing =
      sync_state_store_find(store, entry->game_id, entry->filename, entry->slot);
  if (existing != NULL) {
    *existing = *entry;
    return SYNC_STATE_STORE_OK;
  }

  if (store->count >= ROMM_SYNC_MAX_STATE_ENTRIES) {
    return SYNC_STATE_STORE_ERR_FULL;
  }

  store->entries[store->count] = *entry;
  store->count += 1;
  return SYNC_STATE_STORE_OK;
}

/*
 * Returns a human-readable message for state-store status codes.
 */
const char *sync_state_store_status_str(int status) {
  switch (status) {
    case SYNC_STATE_STORE_OK:
      return "ok";
    case SYNC_STATE_STORE_ERR_INVALID_ARGUMENT:
      return "invalid argument";
    case SYNC_STATE_STORE_ERR_OPEN:
      return "cannot open file";
    case SYNC_STATE_STORE_ERR_READ:
      return "read failure";
    case SYNC_STATE_STORE_ERR_WRITE:
      return "write failure";
    case SYNC_STATE_STORE_ERR_FORMAT:
      return "invalid format";
    case SYNC_STATE_STORE_ERR_FULL:
      return "store capacity reached";
    case SYNC_STATE_STORE_ERR_CREATE_DIRECTORY:
      return "cannot create directory";
    default:
      return "unknown error";
  }
}
