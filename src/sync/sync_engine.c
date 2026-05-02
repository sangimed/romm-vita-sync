#include "sync_engine.h"

#include <ctype.h>
#include <psp2/io/stat.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_log.h"
#include "backup_manager.h"
#include "conflict_resolver.h"
#include "game_matcher.h"
#include "sync_state_store.h"
#include "vita_native_save_policy.h"
#include "vmp_signer.h"
#include "vmp_srm_converter.h"

#define SYNC_DEFAULT_BACKUP_DIRECTORY "ux0:data/romm-vita-sync/backups"
#define SYNC_DEFAULT_CONVERSION_DIRECTORY "ux0:data/romm-vita-sync/cache/conversion"
#define SYNC_DEFAULT_APP_TEMPLATE_SLOT0 "app0:templates/SCEVMC0.VMP"
#define SYNC_DEFAULT_APP_TEMPLATE_SLOT1 "app0:templates/SCEVMC1.VMP"
#define SYNC_REMOTE_UPLOAD_TIMESTAMP_TOLERANCE_SECONDS 300

static int extract_parent_directory(const char *path, char *out_directory, size_t out_directory_size);

/*
 * Default clock provider used when caller does not inject one.
 */
static int64_t default_now_callback(void) {
  return (int64_t)time(NULL);
}

/*
 * Returns non-zero when a string is non-null and non-empty.
 */
static int has_text(const char *value) {
  return (value != NULL) && (value[0] != '\0');
}

/*
 * Lightweight file existence probe used before backup operations.
 */
static int file_exists(const char *path) {
  if (!has_text(path)) {
    return 0;
  }

  FILE *probe = fopen(path, "rb");
  if (probe == NULL) {
    return 0;
  }

  fclose(probe);
  return 1;
}

/*
 * Converts one Unix timestamp back into the same broken-down UTC fields used by
 * the scanner so downloaded files can keep the remote modification time.
 */
static int unix_timestamp_to_datetime(int64_t timestamp_unix, SceDateTime *out_datetime) {
  if ((timestamp_unix <= 0) || (out_datetime == NULL)) {
    return -1;
  }

  time_t raw = (time_t)timestamp_unix;
  struct tm *utc = gmtime(&raw);
  if (utc == NULL) {
    return -1;
  }

  memset(out_datetime, 0, sizeof(*out_datetime));
  out_datetime->year = (unsigned short)(utc->tm_year + 1900);
  out_datetime->month = (unsigned short)(utc->tm_mon + 1);
  out_datetime->day = (unsigned short)utc->tm_mday;
  out_datetime->hour = (unsigned short)utc->tm_hour;
  out_datetime->minute = (unsigned short)utc->tm_min;
  out_datetime->second = (unsigned short)utc->tm_sec;
  out_datetime->microsecond = 0U;
  return 0;
}

/*
 * Reapplies the remote save timestamp to the restored local file so the next
 * scan does not interpret the write time as a fresh local modification.
 */
static int preserve_remote_timestamp_on_local_copy(
    const char *local_path,
    int64_t remote_timestamp_unix) {
  if (!has_text(local_path) || (remote_timestamp_unix <= 0)) {
    return -1;
  }

  SceIoStat stat;
  memset(&stat, 0, sizeof(stat));
  if (sceIoGetstat(local_path, &stat) < 0) {
    return -1;
  }

  if (unix_timestamp_to_datetime(remote_timestamp_unix, &stat.st_mtime) < 0) {
    return -1;
  }

  return sceIoChstat(local_path, &stat, SCE_CST_MT);
}

/*
 * Returns the current file size in bytes for one local path.
 */
static int get_local_file_size_bytes(const char *path, uint64_t *out_size_bytes) {
  if (!has_text(path) || (out_size_bytes == NULL)) {
    return -1;
  }

  SceIoStat stat;
  memset(&stat, 0, sizeof(stat));
  if (sceIoGetstat(path, &stat) < 0) {
    return -1;
  }

  *out_size_bytes = (uint64_t)stat.st_size;
  return 0;
}

/*
 * Creates a backup only when the local file currently exists.
 */
static int backup_existing_local_file(
    const char *path,
    const char *backup_directory,
    int64_t now) {
  if (!has_text(path) || !has_text(backup_directory)) {
    return BACKUP_MANAGER_ERR_INVALID_ARGUMENT;
  }

  if (!file_exists(path)) {
    return BACKUP_MANAGER_OK;
  }

  return backup_manager_backup_file(path, backup_directory, now, NULL, 0U);
}

/*
 * Builds a canonical PS1 VMP target for one slot in the same directory.
 */
static int build_ps1_slot_vmp_target(
    const SyncSaveDescriptor *anchor_item,
    SyncSlot slot,
    SyncSaveDescriptor *out_target) {
  if ((anchor_item == NULL) || !has_text(anchor_item->path) || (out_target == NULL)) {
    return -1;
  }

  if ((slot != SYNC_SLOT_0) && (slot != SYNC_SLOT_1)) {
    return -1;
  }

  char directory[ROMM_MAX_PATH_LEN];
  if (extract_parent_directory(anchor_item->path, directory, sizeof(directory)) < 0) {
    return -1;
  }

  const char *filename = (slot == SYNC_SLOT_1) ? "SCEVMC1.VMP" : "SCEVMC0.VMP";

  sync_save_descriptor_init(out_target);
  snprintf(out_target->game_id, sizeof(out_target->game_id), "%s", anchor_item->game_id);
  snprintf(out_target->title, sizeof(out_target->title), "%s", anchor_item->title);
  snprintf(out_target->filename, sizeof(out_target->filename), "%s", filename);
  out_target->slot = slot;
  out_target->size_bytes = ROMM_PS1_VMP_SIZE;

  int written = snprintf(out_target->path, sizeof(out_target->path), "%s/%s", directory, filename);
  return ((written > 0) && ((size_t)written < sizeof(out_target->path))) ? 0 : -1;
}

/*
 * Builds one sync-state entry for a local file restored from a remote save.
 */
static int build_download_state_entry_for_target(
    SyncStateEntry *entry,
    const SyncSaveDescriptor *local_target,
    const SyncSaveDescriptor *remote_item,
    int64_t last_upload_unix) {
  if ((entry == NULL) || (local_target == NULL) || (remote_item == NULL)) {
    return -1;
  }

  sync_state_entry_init(entry);

  char filename[ROMM_SYNC_MAX_FILENAME_LEN];
  if (has_text(local_target->filename)) {
    snprintf(filename, sizeof(filename), "%s", local_target->filename);
  } else if (sync_extract_filename(local_target->path, filename, sizeof(filename)) < 0) {
    return -1;
  }

  snprintf(entry->game_id, sizeof(entry->game_id), "%s", local_target->game_id);
  snprintf(entry->filename, sizeof(entry->filename), "%s", filename);
  entry->slot = local_target->slot;
  entry->size_bytes = local_target->size_bytes;
  if (get_local_file_size_bytes(local_target->path, &entry->size_bytes) < 0) {
    entry->size_bytes = local_target->size_bytes;
  }
  entry->timestamp_unix = remote_item->timestamp_unix;
  snprintf(entry->origin_device, sizeof(entry->origin_device), "%s", remote_item->origin_device);
  entry->last_upload_unix = last_upload_unix;
  return 0;
}

/*
 * Appends one positive rom_id to a small unique set used for remote filtering.
 */
static int append_unique_rom_id(
    int rom_id,
    int *rom_ids,
    int *in_out_count,
    int max_count) {
  if ((rom_ids == NULL) || (in_out_count == NULL) || (*in_out_count < 0) || (max_count < 0) || (rom_id <= 0)) {
    return -1;
  }

  for (int i = 0; i < *in_out_count; ++i) {
    if (rom_ids[i] == rom_id) {
      return 0;
    }
  }

  if (*in_out_count >= max_count) {
    return -1;
  }

  rom_ids[*in_out_count] = rom_id;
  *in_out_count += 1;
  return 0;
}

/*
 * Case-insensitive ASCII extension check helper.
 */
static int path_has_extension(const char *path, const char *extension) {
  if (!has_text(path) || !has_text(extension)) {
    return 0;
  }

  size_t path_len = strlen(path);
  size_t extension_len = strlen(extension);
  if (path_len < extension_len) {
    return 0;
  }

  const char *path_extension = path + path_len - extension_len;
  return sync_string_ieq(path_extension, extension);
}

/*
 * Sanitizes a string so it can safely be used in generated file names.
 */
static void sanitize_path_component(const char *input, char *output, size_t output_size) {
  if ((output == NULL) || (output_size == 0U)) {
    return;
  }

  output[0] = '\0';
  if (!has_text(input)) {
    snprintf(output, output_size, "item");
    return;
  }

  size_t out = 0U;
  for (const unsigned char *cursor = (const unsigned char *)input;
       (*cursor != '\0') && ((out + 1U) < output_size);
       ++cursor) {
    unsigned char c = *cursor;
    if (isalnum(c) || (c == '_') || (c == '-')) {
      output[out++] = (char)c;
    } else {
      output[out++] = '_';
    }
  }

  if (out == 0U) {
    output[out++] = 'i';
    if (out < output_size) {
      output[out++] = 't';
    }
    if (out < output_size) {
      output[out++] = 'e';
    }
    if (out < output_size) {
      output[out++] = 'm';
    }
  }

  output[out] = '\0';
}

/*
 * Generates a deterministic temporary conversion path for one sync action.
 */
static int build_conversion_temp_path(
    const SyncSaveDescriptor *item,
    const char *operation,
    const char *extension,
    int64_t seed,
    char *out_path,
    size_t out_path_size) {
  if ((item == NULL) || !has_text(operation) || !has_text(extension) ||
      (out_path == NULL) || (out_path_size == 0U)) {
    return -1;
  }

  char safe_game_id[ROMM_GAME_ID_LEN + 8];
  char safe_operation[32];
  sanitize_path_component(item->game_id, safe_game_id, sizeof(safe_game_id));
  sanitize_path_component(operation, safe_operation, sizeof(safe_operation));

  int slot_number = (item->slot == SYNC_SLOT_1) ? 1 : 0;
  int written = snprintf(
      out_path,
      out_path_size,
      "%s/%s_slot%d_%s_%lld%s",
      SYNC_DEFAULT_CONVERSION_DIRECTORY,
      safe_game_id,
      slot_number,
      safe_operation,
      (long long)seed,
      extension);

  return ((written > 0) && ((size_t)written < out_path_size)) ? 0 : -1;
}

/*
 * Extracts the parent directory portion of a full path.
 */
static int extract_parent_directory(const char *path, char *out_directory, size_t out_directory_size) {
  if (!has_text(path) || (out_directory == NULL) || (out_directory_size == 0U)) {
    return -1;
  }

  const char *last_forward = strrchr(path, '/');
  const char *last_backward = strrchr(path, '\\');
  const char *separator = last_forward;
  if ((last_backward != NULL) && ((separator == NULL) || (last_backward > separator))) {
    separator = last_backward;
  }
  if ((separator == NULL) || (separator == path)) {
    return -1;
  }

  size_t directory_length = (size_t)(separator - path);
  if (directory_length >= out_directory_size) {
    return -1;
  }

  memcpy(out_directory, path, directory_length);
  out_directory[directory_length] = '\0';
  return 0;
}

/*
 * Chooses a trusted VMP template path for SRM->VMP reconstruction.
 */
static int resolve_template_vmp_path(
    const SyncSaveDescriptor *local_item,
    char *out_template_path,
    size_t out_template_path_size) {
  if ((local_item == NULL) || !has_text(local_item->path) ||
      (out_template_path == NULL) || (out_template_path_size == 0U)) {
    return -1;
  }

  if (file_exists(local_item->path)) {
    snprintf(out_template_path, out_template_path_size, "%s", local_item->path);
    return 0;
  }

  char directory[ROMM_MAX_PATH_LEN];
  if (extract_parent_directory(local_item->path, directory, sizeof(directory)) < 0) {
    return -1;
  }

  const char *preferred = (local_item->slot == SYNC_SLOT_1) ? "SCEVMC1.VMP" : "SCEVMC0.VMP";
  const char *fallback = (local_item->slot == SYNC_SLOT_1) ? "SCEVMC0.VMP" : "SCEVMC1.VMP";

  int written = snprintf(out_template_path, out_template_path_size, "%s/%s", directory, preferred);
  if ((written > 0) && ((size_t)written < out_template_path_size) && file_exists(out_template_path)) {
    return 0;
  }

  written = snprintf(out_template_path, out_template_path_size, "%s/%s", directory, fallback);
  if ((written > 0) && ((size_t)written < out_template_path_size) && file_exists(out_template_path)) {
    return 0;
  }

  {
    const char *app_template = (local_item->slot == SYNC_SLOT_1)
                                   ? SYNC_DEFAULT_APP_TEMPLATE_SLOT1
                                   : SYNC_DEFAULT_APP_TEMPLATE_SLOT0;
    if (file_exists(app_template)) {
      snprintf(out_template_path, out_template_path_size, "%s", app_template);
      return 0;
    }
  }

  out_template_path[0] = '\0';
  return -1;
}

/*
 * Writes a formatted reason message into an action record.
 */
static void set_reason(SyncActionRecord *action, const char *format, ...) {
  if ((action == NULL) || (format == NULL)) {
    return;
  }

  va_list args;
  va_start(args, format);
  vsnprintf(action->reason, sizeof(action->reason), format, args);
  va_end(args);
}

/*
 * Emits one progress checkpoint to the optional sync progress callback.
 * The callback receives completed/total units and a concise stage message.
 */
static void emit_progress(
    const SyncEngineConfig *config,
    int completed_units,
    int total_units,
    int local_index,
    int local_total,
    const char *format,
    ...) {
  if ((config == NULL) || (config->progress_callback == NULL) || (format == NULL)) {
    return;
  }

  char message[ROMM_SYNC_MAX_REASON_LEN];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);

  config->progress_callback(
      completed_units,
      total_units,
      local_index,
      local_total,
      message,
      config->progress_user_data);
}

/*
 * Compares local and remote content signatures using size and optional hash.
 */
static int same_content_signature(const SyncSaveDescriptor *local_item, const SyncSaveDescriptor *remote_item) {
  if ((local_item == NULL) || (remote_item == NULL)) {
    return 0;
  }

  if (local_item->size_bytes != remote_item->size_bytes) {
    return 0;
  }

  if (has_text(local_item->hash) && has_text(remote_item->hash)) {
    return sync_string_ieq(local_item->hash, remote_item->hash);
  }

  return 1;
}

/*
 * Logs one concise local/remote metadata pair on the exact timestamp basis used
 * by conflict detection so false "newer" decisions are easier to inspect.
 */
static void log_sync_match_metadata(
    const SyncSaveDescriptor *local_item,
    const SyncSaveDescriptor *remote_item) {
  if ((local_item == NULL) || (remote_item == NULL)) {
    return;
  }

  char local_timestamp[32];
  char remote_timestamp[32];
  sync_format_timestamp(local_item->timestamp_unix, local_timestamp, sizeof(local_timestamp));
  sync_format_timestamp(remote_item->timestamp_unix, remote_timestamp, sizeof(remote_timestamp));

  app_log_write(
      APP_LOG_LEVEL_INFO,
      "sync",
      "match metadata game=%s file=%s local_ts=%s local_unix=%lld remote_id=%d remote_file=%s remote_ts=%s remote_unix=%lld local_size=%llu remote_size=%llu remote_slot=%s remote_origin=%s device_current=%d",
      has_text(local_item->game_id) ? local_item->game_id : "(unknown)",
      has_text(local_item->filename) ? local_item->filename : local_item->path,
      local_timestamp,
      (long long)local_item->timestamp_unix,
      remote_item->remote_id,
      has_text(remote_item->filename) ? remote_item->filename : "(unknown)",
      remote_timestamp,
      (long long)remote_item->timestamp_unix,
      (unsigned long long)local_item->size_bytes,
      (unsigned long long)remote_item->size_bytes,
      sync_slot_str(remote_item->slot),
      has_text(remote_item->origin_device) ? remote_item->origin_device : "(none)",
      remote_item->device_is_current);
}

/*
 * Finds the slot 1 peer that lost an equal-timestamp tie against a selected
 * slot 0 item so callers can log the deterministic fallback explicitly.
 */
static const SyncSaveDescriptor *find_slot1_tie_peer(
    const SyncSaveDescriptor *items,
    int item_count,
    int selected_index) {
  if ((items == NULL) || (selected_index < 0) || (selected_index >= item_count)) {
    return NULL;
  }

  const SyncSaveDescriptor *selected = &items[selected_index];
  if ((selected->slot != SYNC_SLOT_0) || !has_text(selected->game_id)) {
    return NULL;
  }

  for (int i = 0; i < item_count; ++i) {
    if (i == selected_index) {
      continue;
    }

    const SyncSaveDescriptor *candidate = &items[i];
    if (!has_text(candidate->game_id) ||
        !sync_string_ieq(candidate->game_id, selected->game_id)) {
      continue;
    }
    if ((candidate->slot != SYNC_SLOT_1) ||
        (candidate->timestamp_unix != selected->timestamp_unix)) {
      continue;
    }

    return candidate;
  }

  return NULL;
}

/*
 * Adds a new action entry to the run report and pre-fills identity fields.
 */
static SyncActionRecord *append_action_record(SyncRunReport *report, const SyncSaveDescriptor *local_item) {
  if ((report == NULL) || (local_item == NULL)) {
    return NULL;
  }

  if (report->action_count >= ROMM_SYNC_MAX_ACTIONS) {
    return NULL;
  }

  SyncActionRecord *action = &report->actions[report->action_count];
  memset(action, 0, sizeof(*action));
  snprintf(action->game_id, sizeof(action->game_id), "%s", local_item->game_id);
  snprintf(action->filename, sizeof(action->filename), "%s", local_item->filename);
  action->slot = local_item->slot;
  action->action = SYNC_ACTION_NONE;
  action->conflict = SYNC_CONFLICT_NONE;
  action->status_code = SYNC_ENGINE_OK;
  report->action_count += 1;
  return action;
}

/*
 * Implements "skip upload when size and timestamp are unchanged" rule.
 */
static int should_skip_redundant_upload(
    const SyncSaveDescriptor *local_item,
    const SyncStateEntry *state_entry) {
  if ((local_item == NULL) || (state_entry == NULL)) {
    return 0;
  }

  return (local_item->size_bytes == state_entry->size_bytes) &&
         (local_item->timestamp_unix == state_entry->timestamp_unix);
}

/*
 * Returns non-zero when a matched remote save appears to be the same server
 * object produced by the last upload from this device. RomM exposes server
 * updated_at as the remote timestamp, so it can be newer than the savedata
 * mtime even when the local save has not changed.
 */
static int remote_matches_known_uploaded_local(
    const SyncSaveDescriptor *local_item,
    const SyncSaveDescriptor *remote_item,
    const SyncStateEntry *state_entry) {
  if (!should_skip_redundant_upload(local_item, state_entry) || (remote_item == NULL)) {
    return 0;
  }

  if ((state_entry != NULL) &&
      has_text(state_entry->origin_device) &&
      has_text(remote_item->origin_device) &&
      sync_string_ieq(state_entry->origin_device, remote_item->origin_device)) {
    return 1;
  }

  if ((state_entry == NULL) || (state_entry->last_upload_unix <= 0) ||
      (remote_item->timestamp_unix <= 0)) {
    return 0;
  }

  return remote_item->timestamp_unix <=
         (state_entry->last_upload_unix + SYNC_REMOTE_UPLOAD_TIMESTAMP_TOLERANCE_SECONDS);
}

/*
 * Returns non-zero when remote metadata says this device is already current.
 */
static int remote_reports_current_for_device(
    const SyncSaveDescriptor *local_item,
    const SyncSaveDescriptor *remote_item) {
  if ((local_item == NULL) || (remote_item == NULL)) {
    return 0;
  }

  if (!remote_item->device_is_current) {
    return 0;
  }

  if (!has_text(local_item->path)) {
    return 0;
  }

  return file_exists(local_item->path);
}

/*
 * Returns non-zero when the local descriptor is only a restore target.
 * The scanner synthesizes these entries from CONFIG.BIN when both VMP cards are
 * absent, so they should download from RomM instead of attempting an upload.
 */
static int local_item_is_missing_restore_target(const SyncSaveDescriptor *local_item) {
  if (local_item == NULL) {
    return 0;
  }

  if (!has_text(local_item->path) || !path_has_extension(local_item->path, ".vmp")) {
    return 0;
  }

  return !file_exists(local_item->path);
}

/*
 * Builds a sync-state entry from a save descriptor after a transfer decision.
 */
static void build_state_entry_from_item(
    SyncStateEntry *entry,
    const SyncSaveDescriptor *item,
    const char *device_id,
    int64_t last_upload_unix) {
  if ((entry == NULL) || (item == NULL)) {
    return;
  }

  sync_state_entry_init(entry);
  snprintf(entry->game_id, sizeof(entry->game_id), "%s", item->game_id);
  snprintf(entry->filename, sizeof(entry->filename), "%s", item->filename);
  entry->slot = item->slot;
  entry->size_bytes = item->size_bytes;
  entry->timestamp_unix = item->timestamp_unix;
  snprintf(entry->origin_device, sizeof(entry->origin_device), "%s", device_id != NULL ? device_id : "");
  entry->last_upload_unix = last_upload_unix;
}

/*
 * Executes (or plans in dry-run mode) an upload and updates sync state.
 * When remote_item points to an already matched RomM save, its remote_id is
 * propagated so the transport layer can overwrite that save instead of
 * creating a new versioned entry on the server.
 */
static int execute_upload(
    const SyncEngineConfig *config,
    const RommClient *romm_client,
    const char *device_id,
    const SyncSaveDescriptor *local_item,
    const SyncSaveDescriptor *remote_item,
    SyncStateStore *state_store,
    SyncActionRecord *action,
    SyncRunReport *report) {
  if ((config == NULL) || (romm_client == NULL) || (local_item == NULL) ||
      (state_store == NULL) || (action == NULL) || (report == NULL)) {
    return SYNC_ENGINE_ERR_INVALID_ARGUMENT;
  }

  if (config->dry_run) {
    app_log_write(
        APP_LOG_LEVEL_INFO,
        "sync",
        "dry-run plan: would upload save: game=%s file=%s",
        local_item->game_id,
        has_text(action->filename) ? action->filename : local_item->filename);
    action->executed = 0;
    action->status_code = SYNC_ENGINE_OK;
    if (action->conflict != SYNC_CONFLICT_NONE) {
      set_reason(
          action,
          "planned upload (dry-run; approved conflict=%s)",
          sync_conflict_type_str(action->conflict));
    } else {
      set_reason(action, "planned upload (dry-run)");
    }
    return SYNC_ENGINE_OK;
  }

  app_log_write(
      APP_LOG_LEVEL_INFO,
      "sync",
      "uploading save: game=%s file=%s",
      local_item->game_id,
      has_text(action->filename) ? action->filename : local_item->filename);

  SyncSaveDescriptor upload_item;
  memcpy(&upload_item, local_item, sizeof(upload_item));
  if ((remote_item != NULL) && (remote_item->remote_id > 0)) {
    upload_item.remote_id = remote_item->remote_id;
  }

  int has_conversion_temp = 0;
  char conversion_temp_path[ROMM_MAX_PATH_LEN];
  conversion_temp_path[0] = '\0';

  if (has_text(local_item->path) && path_has_extension(local_item->path, ".vmp")) {
    app_log_write(
        APP_LOG_LEVEL_INFO,
        "sync",
        "converting save format: .VMP -> .SRM before upload");
    int ensure_status = backup_manager_ensure_directory(SYNC_DEFAULT_CONVERSION_DIRECTORY);
    if (ensure_status != BACKUP_MANAGER_OK) {
      action->status_code = ensure_status;
      report->transfer_errors += 1;
      set_reason(action, "cannot create conversion directory (%s)", backup_manager_status_str(ensure_status));
      return ensure_status;
    }

    int64_t now = (config->now_callback != NULL) ? config->now_callback() : default_now_callback();
    if (build_conversion_temp_path(
            local_item,
            "upload",
            ".srm",
            now,
            conversion_temp_path,
            sizeof(conversion_temp_path)) < 0) {
      action->status_code = SYNC_ENGINE_ERR_INVALID_ARGUMENT;
      report->transfer_errors += 1;
      set_reason(action, "cannot build upload conversion path");
      return SYNC_ENGINE_ERR_INVALID_ARGUMENT;
    }

    int convert_status = vmp_to_srm_file(local_item->path, conversion_temp_path);
    if (convert_status != ROMM_VMP_SRM_OK) {
      action->status_code = convert_status;
      report->transfer_errors += 1;
      set_reason(action, "vmp->srm conversion failed (%s)", vmp_srm_status_str(convert_status));
      app_log_write(
          APP_LOG_LEVEL_WARN,
          "sync",
          "upload conversion failed game=%s file=%s status=%s",
          local_item->game_id,
          action->filename,
          vmp_srm_status_str(convert_status));
      return convert_status;
    }

    snprintf(upload_item.path, sizeof(upload_item.path), "%s", conversion_temp_path);
    if (sync_extract_filename(conversion_temp_path, upload_item.filename, sizeof(upload_item.filename)) < 0) {
      upload_item.filename[0] = '\0';
    }
    upload_item.size_bytes = ROMM_PS1_SRM_SIZE;
    has_conversion_temp = 1;
    app_log_write(
        APP_LOG_LEVEL_DEBUG,
        "sync",
        "upload conversion success game=%s src=%s tmp=%s",
        local_item->game_id,
        local_item->path,
        conversion_temp_path);
  }

  int status = romm_client_upload_save(romm_client, &upload_item);
  if (has_conversion_temp) {
    remove(conversion_temp_path);
  }

  action->status_code = status;
  if (status == ROMM_CLIENT_ERR_CONFLICT) {
    action->executed = 0;
    set_reason(action, "upload conflict (server newer)");
    return status;
  }

  if (status < 0) {
    action->executed = 0;
    report->transfer_errors += 1;
    set_reason(action, "upload failed (%s)", romm_client_status_str(status));
    return status;
  }

  action->executed = 1;
  report->uploads_executed += 1;
  if (action->conflict != SYNC_CONFLICT_NONE) {
    set_reason(action, "uploaded after conflict review (%s)", sync_conflict_type_str(action->conflict));
  } else {
    set_reason(action, "uploaded");
  }
  app_log_write(APP_LOG_LEVEL_INFO, "sync", "upload complete: game=%s file=%s", local_item->game_id, action->filename);

  int64_t now = (config->now_callback != NULL) ? config->now_callback() : default_now_callback();
  SyncStateEntry state_entry;
  build_state_entry_from_item(&state_entry, local_item, device_id, now);

  int upsert_status = sync_state_store_upsert(state_store, &state_entry);
  if (upsert_status != SYNC_STATE_STORE_OK) {
    report->transfer_errors += 1;
    action->status_code = upsert_status;
    set_reason(action, "uploaded but state update failed (%s)", sync_state_store_status_str(upsert_status));
    return upsert_status;
  }

  return SYNC_ENGINE_OK;
}

/*
 * Executes (or plans in dry-run mode) a download, including mandatory backup.
 */
static int execute_download(
    const SyncEngineConfig *config,
    const RommClient *romm_client,
    const SyncSaveDescriptor *local_item,
    const SyncSaveDescriptor *remote_item,
    const SyncStateEntry *previous_state_entry,
    SyncStateStore *state_store,
    SyncActionRecord *action,
    SyncRunReport *report) {
  if ((config == NULL) || (romm_client == NULL) || (local_item == NULL) || (remote_item == NULL) ||
      (state_store == NULL) || (action == NULL) || (report == NULL)) {
    return SYNC_ENGINE_ERR_INVALID_ARGUMENT;
  }

  if (!has_text(local_item->path)) {
    action->status_code = SYNC_ENGINE_ERR_INVALID_ARGUMENT;
    set_reason(action, "download destination path missing");
    report->transfer_errors += 1;
    return SYNC_ENGINE_ERR_INVALID_ARGUMENT;
  }

  if (config->dry_run) {
    app_log_write(
        APP_LOG_LEVEL_INFO,
        "sync",
        "dry-run plan: would download save: game=%s file=%s",
        local_item->game_id,
        has_text(action->filename) ? action->filename : local_item->filename);
    action->executed = 0;
    action->status_code = SYNC_ENGINE_OK;
    if (action->conflict != SYNC_CONFLICT_NONE) {
      set_reason(
          action,
          "planned download (dry-run; approved conflict=%s)",
          sync_conflict_type_str(action->conflict));
    } else {
      set_reason(action, "planned download (dry-run)");
    }
    return SYNC_ENGINE_OK;
  }

  app_log_write(
      APP_LOG_LEVEL_INFO,
      "sync",
      "downloading save: game=%s file=%s",
      local_item->game_id,
      has_text(action->filename) ? action->filename : local_item->filename);
  {
    char remote_timestamp[32];
    sync_format_timestamp(remote_item->timestamp_unix, remote_timestamp, sizeof(remote_timestamp));
    app_log_write(
        APP_LOG_LEVEL_INFO,
        "sync",
        "download source remote_id=%d remote_file=%s remote_ts=%s remote_unix=%lld remote_origin=%s",
        remote_item->remote_id,
        has_text(remote_item->filename) ? remote_item->filename : "(unknown)",
        remote_timestamp,
        (long long)remote_item->timestamp_unix,
        has_text(remote_item->origin_device) ? remote_item->origin_device : "(none)");
  }

  const char *backup_directory = has_text(config->backup_directory)
                                     ? config->backup_directory
                                     : SYNC_DEFAULT_BACKUP_DIRECTORY;
  int64_t now = (config->now_callback != NULL) ? config->now_callback() : default_now_callback();

  if (file_exists(local_item->path)) {
    app_log_write(APP_LOG_LEVEL_INFO, "sync", "creating backup before overwrite");
    int backup_status = backup_manager_backup_file(local_item->path, backup_directory, now, NULL, 0U);
    if (backup_status != BACKUP_MANAGER_OK) {
      action->status_code = backup_status;
      set_reason(action, "backup failed (%s)", backup_manager_status_str(backup_status));
      report->transfer_errors += 1;
      return backup_status;
    }
  }

  int status = ROMM_CLIENT_OK;
  int has_conversion_temp = 0;
  char conversion_temp_path[ROMM_MAX_PATH_LEN];
  conversion_temp_path[0] = '\0';
  SyncSaveDescriptor restored_targets[2];
  int restored_target_count = 0;

  if (path_has_extension(local_item->path, ".vmp")) {
    app_log_write(
        APP_LOG_LEVEL_INFO,
        "sync",
        "converting save format: .SRM -> .VMP after download");
    int ensure_status = backup_manager_ensure_directory(SYNC_DEFAULT_CONVERSION_DIRECTORY);
    if (ensure_status != BACKUP_MANAGER_OK) {
      action->status_code = ensure_status;
      report->transfer_errors += 1;
      set_reason(action, "cannot create conversion directory (%s)", backup_manager_status_str(ensure_status));
      return ensure_status;
    }

    if (build_conversion_temp_path(
            local_item,
            "download",
            ".srm",
            now,
            conversion_temp_path,
            sizeof(conversion_temp_path)) < 0) {
      action->status_code = SYNC_ENGINE_ERR_INVALID_ARGUMENT;
      report->transfer_errors += 1;
      set_reason(action, "cannot build download conversion path");
      return SYNC_ENGINE_ERR_INVALID_ARGUMENT;
    }

    status = romm_client_download_save(romm_client, remote_item, conversion_temp_path);
    action->status_code = status;
    if (status < 0) {
      action->executed = 0;
      report->transfer_errors += 1;
      set_reason(action, "download failed (%s)", romm_client_status_str(status));
      remove(conversion_temp_path);
      return status;
    }

    has_conversion_temp = 1;

    int card_count = 0;
    int card_count_status = srm_get_card_count(conversion_temp_path, &card_count);
    if (card_count_status != ROMM_VMP_SRM_OK) {
      action->executed = 0;
      action->status_code = card_count_status;
      report->transfer_errors += 1;
      set_reason(action, "srm inspection failed (%s)", vmp_srm_status_str(card_count_status));
      app_log_write(
          APP_LOG_LEVEL_WARN,
          "sync",
          "download SRM inspection failed game=%s file=%s status=%s",
          local_item->game_id,
          action->filename,
          vmp_srm_status_str(card_count_status));
      remove(conversion_temp_path);
      return card_count_status;
    }

    if (card_count == 2) {
      SyncSaveDescriptor slot0_target;
      SyncSaveDescriptor slot1_target;
      if ((build_ps1_slot_vmp_target(local_item, SYNC_SLOT_0, &slot0_target) < 0) ||
          (build_ps1_slot_vmp_target(local_item, SYNC_SLOT_1, &slot1_target) < 0)) {
        action->executed = 0;
        action->status_code = SYNC_ENGINE_ERR_INVALID_ARGUMENT;
        report->transfer_errors += 1;
        set_reason(action, "cannot derive dual-card PS1 restore targets");
        remove(conversion_temp_path);
        return SYNC_ENGINE_ERR_INVALID_ARGUMENT;
      }

      app_log_write(
          APP_LOG_LEVEL_INFO,
          "sync",
          "dual-card SRM detected for game=%s; restoring both PS1 cards to %s and %s",
          local_item->game_id,
          slot0_target.filename,
          slot1_target.filename);

      if (strcmp(slot0_target.path, local_item->path) != 0) {
        int backup_status = backup_existing_local_file(slot0_target.path, backup_directory, now);
        if (backup_status != BACKUP_MANAGER_OK) {
          action->status_code = backup_status;
          set_reason(action, "backup failed for %s (%s)", slot0_target.filename, backup_manager_status_str(backup_status));
          report->transfer_errors += 1;
          remove(conversion_temp_path);
          return backup_status;
        }
      }

      if (strcmp(slot1_target.path, local_item->path) != 0) {
        int backup_status = backup_existing_local_file(slot1_target.path, backup_directory, now);
        if (backup_status != BACKUP_MANAGER_OK) {
          action->status_code = backup_status;
          set_reason(action, "backup failed for %s (%s)", slot1_target.filename, backup_manager_status_str(backup_status));
          report->transfer_errors += 1;
          remove(conversion_temp_path);
          return backup_status;
        }
      }

      char template_slot0[ROMM_MAX_PATH_LEN];
      char template_slot1[ROMM_MAX_PATH_LEN];
      if ((resolve_template_vmp_path(&slot0_target, template_slot0, sizeof(template_slot0)) < 0) ||
          (resolve_template_vmp_path(&slot1_target, template_slot1, sizeof(template_slot1)) < 0)) {
        action->executed = 0;
        action->status_code = SYNC_ENGINE_ERR_INVALID_ARGUMENT;
        report->transfer_errors += 1;
        set_reason(action, "no trusted template VMP available for dual-card SRM->VMP reconstruction");
        remove(conversion_temp_path);
        return SYNC_ENGINE_ERR_INVALID_ARGUMENT;
      }

      int convert_status = srm_card_to_vmp_file(conversion_temp_path, 0, template_slot0, slot0_target.path);
      if (convert_status == ROMM_VMP_SRM_OK) {
        convert_status = srm_card_to_vmp_file(conversion_temp_path, 1, template_slot1, slot1_target.path);
      }
      if (convert_status != ROMM_VMP_SRM_OK) {
        action->executed = 0;
        action->status_code = convert_status;
        report->transfer_errors += 1;
        set_reason(action, "dual-card srm->vmp conversion failed (%s)", vmp_srm_status_str(convert_status));
        app_log_write(
            APP_LOG_LEVEL_WARN,
            "sync",
            "dual-card download conversion failed game=%s file=%s status=%s",
            local_item->game_id,
            action->filename,
            vmp_srm_status_str(convert_status));
        remove(conversion_temp_path);
        return convert_status;
      }

      int sign_status = vmp_sign_file_in_place(slot0_target.path);
      if (sign_status == ROMM_VMP_SIGNER_OK) {
        sign_status = vmp_sign_file_in_place(slot1_target.path);
      }
      if (sign_status != ROMM_VMP_SIGNER_OK) {
        action->executed = 0;
        action->status_code = sign_status;
        report->transfer_errors += 1;
        set_reason(action, "dual-card vmp signing failed (%s)", vmp_signer_status_str(sign_status));
        app_log_write(
            APP_LOG_LEVEL_WARN,
            "sync",
            "dual-card download signing failed game=%s file=%s status=%s",
            local_item->game_id,
            action->filename,
            vmp_signer_status_str(sign_status));
        remove(conversion_temp_path);
        return sign_status;
      }

      restored_targets[0] = slot0_target;
      restored_targets[1] = slot1_target;
      restored_target_count = 2;

      app_log_write(
          APP_LOG_LEVEL_DEBUG,
          "sync",
          "dual-card download conversion success game=%s tmp=%s dst0=%s dst1=%s",
          local_item->game_id,
          conversion_temp_path,
          slot0_target.path,
          slot1_target.path);
    } else {
      char template_vmp_path[ROMM_MAX_PATH_LEN];
      if (resolve_template_vmp_path(local_item, template_vmp_path, sizeof(template_vmp_path)) < 0) {
        action->executed = 0;
        action->status_code = SYNC_ENGINE_ERR_INVALID_ARGUMENT;
        report->transfer_errors += 1;
        set_reason(action, "no trusted template VMP available for SRM->VMP reconstruction");
        remove(conversion_temp_path);
        return SYNC_ENGINE_ERR_INVALID_ARGUMENT;
      }

      int convert_status = srm_to_vmp_file(conversion_temp_path, template_vmp_path, local_item->path);
      if (convert_status != ROMM_VMP_SRM_OK) {
        action->executed = 0;
        action->status_code = convert_status;
        report->transfer_errors += 1;
        set_reason(action, "srm->vmp conversion failed (%s)", vmp_srm_status_str(convert_status));
        app_log_write(
            APP_LOG_LEVEL_WARN,
            "sync",
            "download conversion failed game=%s file=%s status=%s",
            local_item->game_id,
            action->filename,
            vmp_srm_status_str(convert_status));
        remove(conversion_temp_path);
        return convert_status;
      }

      int sign_status = vmp_sign_file_in_place(local_item->path);
      if (sign_status != ROMM_VMP_SIGNER_OK) {
        action->executed = 0;
        action->status_code = sign_status;
        report->transfer_errors += 1;
        set_reason(action, "vmp signing failed (%s)", vmp_signer_status_str(sign_status));
        app_log_write(
            APP_LOG_LEVEL_WARN,
            "sync",
            "download signing failed game=%s file=%s status=%s",
            local_item->game_id,
            action->filename,
            vmp_signer_status_str(sign_status));
        remove(conversion_temp_path);
        return sign_status;
      }

      restored_targets[0] = *local_item;
      restored_target_count = 1;

      app_log_write(
          APP_LOG_LEVEL_DEBUG,
          "sync",
          "download conversion success game=%s tmp=%s dst=%s",
          local_item->game_id,
          conversion_temp_path,
          local_item->path);
    }
  } else {
    status = romm_client_download_save(romm_client, remote_item, local_item->path);
    action->status_code = status;
    if (status < 0) {
      action->executed = 0;
      report->transfer_errors += 1;
      set_reason(action, "download failed (%s)", romm_client_status_str(status));
      return status;
    }
  }

  if (has_conversion_temp) {
    remove(conversion_temp_path);
  }

  if (remote_item->timestamp_unix > 0) {
    if (restored_target_count > 0) {
      for (int i = 0; i < restored_target_count; ++i) {
        int preserve_status = preserve_remote_timestamp_on_local_copy(
            restored_targets[i].path,
            remote_item->timestamp_unix);
        if (preserve_status < 0) {
          app_log_write(
              APP_LOG_LEVEL_WARN,
              "sync",
              "download timestamp preserve failed game=%s file=%s remote_ts=%lld status=%d",
              local_item->game_id,
              restored_targets[i].filename,
              (long long)remote_item->timestamp_unix,
              preserve_status);
        }
      }
    } else {
      int preserve_status = preserve_remote_timestamp_on_local_copy(local_item->path, remote_item->timestamp_unix);
      if (preserve_status < 0) {
        app_log_write(
            APP_LOG_LEVEL_WARN,
            "sync",
            "download timestamp preserve failed game=%s file=%s remote_ts=%lld status=%d",
            local_item->game_id,
            action->filename,
            (long long)remote_item->timestamp_unix,
            preserve_status);
      }
    }
  }

  action->status_code = status;

  action->executed = 1;
  report->downloads_executed += 1;
  if (action->conflict != SYNC_CONFLICT_NONE) {
    if (restored_target_count == 2) {
      set_reason(action, "downloaded dual-card restore after conflict review (%s)", sync_conflict_type_str(action->conflict));
    } else {
      set_reason(action, "downloaded after conflict review (%s)", sync_conflict_type_str(action->conflict));
    }
  } else {
    set_reason(action, (restored_target_count == 2) ? "downloaded dual-card restore" : "downloaded");
  }
  if (restored_target_count == 2) {
    app_log_write(
        APP_LOG_LEVEL_INFO,
        "sync",
        "download complete: game=%s restored=%s,%s",
        local_item->game_id,
        restored_targets[0].filename,
        restored_targets[1].filename);
  } else {
    app_log_write(APP_LOG_LEVEL_INFO, "sync", "download complete: game=%s file=%s", local_item->game_id, action->filename);
  }

  if (restored_target_count > 0) {
    for (int i = 0; i < restored_target_count; ++i) {
      const SyncStateEntry *target_previous_state = previous_state_entry;
      if ((strcmp(restored_targets[i].path, local_item->path) != 0) || (previous_state_entry == NULL)) {
        target_previous_state = sync_state_store_find_const(
            state_store,
            restored_targets[i].game_id,
            restored_targets[i].filename,
            restored_targets[i].slot);
      }

      int64_t last_upload = 0;
      if (target_previous_state != NULL) {
        last_upload = target_previous_state->last_upload_unix;
      }

      SyncStateEntry state_entry;
      if (build_download_state_entry_for_target(
              &state_entry,
              &restored_targets[i],
              remote_item,
              last_upload) < 0) {
        report->transfer_errors += 1;
        action->status_code = SYNC_STATE_STORE_ERR_INVALID_ARGUMENT;
        set_reason(action, "downloaded but state update failed (%s)", sync_state_store_status_str(SYNC_STATE_STORE_ERR_INVALID_ARGUMENT));
        return SYNC_STATE_STORE_ERR_INVALID_ARGUMENT;
      }

      int upsert_status = sync_state_store_upsert(state_store, &state_entry);
      if (upsert_status != SYNC_STATE_STORE_OK) {
        report->transfer_errors += 1;
        action->status_code = upsert_status;
        set_reason(action, "downloaded but state update failed (%s)", sync_state_store_status_str(upsert_status));
        return upsert_status;
      }
    }
  } else {
    int64_t last_upload = 0;
    if (previous_state_entry != NULL) {
      last_upload = previous_state_entry->last_upload_unix;
    }

    SyncStateEntry state_entry;
    build_state_entry_from_item(
        &state_entry,
        remote_item,
        remote_item->origin_device,
        last_upload);

    if (has_text(local_item->game_id)) {
      snprintf(state_entry.game_id, sizeof(state_entry.game_id), "%s", local_item->game_id);
    }
    if (has_text(local_item->filename)) {
      snprintf(state_entry.filename, sizeof(state_entry.filename), "%s", local_item->filename);
    }
    if (local_item->slot != SYNC_SLOT_UNKNOWN) {
      state_entry.slot = local_item->slot;
    }

    int upsert_status = sync_state_store_upsert(state_store, &state_entry);
    if (upsert_status != SYNC_STATE_STORE_OK) {
      report->transfer_errors += 1;
      action->status_code = upsert_status;
      set_reason(action, "downloaded but state update failed (%s)", sync_state_store_status_str(upsert_status));
      return upsert_status;
    }
  }

  return SYNC_ENGINE_OK;
}

/*
 * Initializes sync engine configuration with safe defaults.
 */
void sync_engine_config_init(SyncEngineConfig *config) {
  if (config == NULL) {
    return;
  }

  memset(config, 0, sizeof(*config));
  config->dry_run = 0;
}

/*
 * Runs one deterministic synchronization pass:
 * local scan -> latest local card selection per game -> remote compare ->
 * action decisions -> optional execution.
 */
int sync_engine_run(
    const SyncEngineConfig *config,
    const SyncSaveDescriptor *local_items,
    int local_count,
    const RommClient *romm_client,
    SyncRunReport *out_report) {
  if ((config == NULL) || (local_items == NULL) || (local_count < 0) ||
      (romm_client == NULL) || (out_report == NULL)) {
    return SYNC_ENGINE_ERR_INVALID_ARGUMENT;
  }

  memset(out_report, 0, sizeof(*out_report));
  out_report->local_count = local_count;
  app_log_write(APP_LOG_LEVEL_DEBUG, "sync", "run begin local_count=%d dry_run=%d", local_count, config->dry_run);
  int total_engine_units = local_count + 1;
  if (total_engine_units < 1) {
    total_engine_units = 1;
  }
  int completed_engine_units = 0;
  int status = SYNC_ENGINE_OK;
  SyncStateStore *state_store = NULL;
  SyncSaveDescriptor *remote_items = NULL;
  int *selected_mask = NULL;
  SyncLocalSelectionReason *selection_reasons = NULL;
  int *selected_rom_ids = NULL;
  int selected_local_count = local_count;
  int selected_rom_id_count = 0;
  emit_progress(
      config,
      completed_engine_units,
      total_engine_units,
      -1,
      local_count,
      "Listing remote saves...");

  if (local_count > 0) {
    selected_mask = (int *)malloc(sizeof(*selected_mask) * (size_t)local_count);
    selection_reasons =
        (SyncLocalSelectionReason *)malloc(sizeof(*selection_reasons) * (size_t)local_count);
    if ((selected_mask == NULL) || (selection_reasons == NULL)) {
      app_log_write(APP_LOG_LEVEL_ERROR, "sync", "out of memory allocating local selection buffers");
      emit_progress(
          config,
          completed_engine_units,
          total_engine_units,
          -1,
          local_count,
          "Sync failed: insufficient memory");
      status = SYNC_ENGINE_ERR_OUT_OF_MEMORY;
      goto cleanup;
    }

    int selected_count = sync_select_latest_local_per_game(
        local_items,
        local_count,
        selected_mask,
        selection_reasons);
    if (selected_count < 0) {
      status = SYNC_ENGINE_ERR_INVALID_ARGUMENT;
      goto cleanup;
    }
    selected_local_count = selected_count;

    if (selected_count != local_count) {
      app_log_write(
          APP_LOG_LEVEL_INFO,
          "sync",
          "ps1 latest-card rule selected %d sync candidate(s) from %d local card(s)",
          selected_count,
          local_count);
    }

    for (int i = 0; i < local_count; ++i) {
      if (!selected_mask[i] ||
          (selection_reasons[i] != SYNC_LOCAL_SELECTION_EQUAL_TIMESTAMP_PREFER_SLOT0)) {
        continue;
      }

      const SyncSaveDescriptor *slot1_peer = find_slot1_tie_peer(local_items, local_count, i);
      if (slot1_peer == NULL) {
        continue;
      }

      app_log_write(
          APP_LOG_LEVEL_WARN,
          "sync",
          "equal local timestamps for game=%s; defaulting to %s and skipping %s",
          has_text(local_items[i].game_id) ? local_items[i].game_id : "(unknown)",
          has_text(local_items[i].filename) ? local_items[i].filename : local_items[i].path,
          has_text(slot1_peer->filename) ? slot1_peer->filename : slot1_peer->path);
    }
  }

  for (int i = 0; i < local_count; ++i) {
    if ((selected_mask != NULL) && !selected_mask[i]) {
      continue;
    }

    const SyncSaveDescriptor *local_item = &local_items[i];
    if (local_item->rom_id > 0) {
      continue;
    }

    app_log_write(
        APP_LOG_LEVEL_ERROR,
        "sync",
        "sync aborted: unresolved rom_id game=%s file=%s",
        local_item->game_id,
        local_item->filename);
    emit_progress(
        config,
        completed_engine_units,
        total_engine_units,
        i,
        local_count,
        "Sync failed: unresolved rom_id for %s",
        has_text(local_item->game_id) ? local_item->game_id : "(unknown)");
    status = SYNC_ENGINE_ERR_UNRESOLVED_ROM_ID;
    goto cleanup;
  }

  if (selected_local_count > 0) {
    selected_rom_ids = (int *)malloc(sizeof(*selected_rom_ids) * (size_t)selected_local_count);
    if (selected_rom_ids == NULL) {
      app_log_write(APP_LOG_LEVEL_ERROR, "sync", "out of memory allocating remote filter rom_id buffer");
      emit_progress(
          config,
          completed_engine_units,
          total_engine_units,
          -1,
          local_count,
          "Sync failed: insufficient memory");
      status = SYNC_ENGINE_ERR_OUT_OF_MEMORY;
      goto cleanup;
    }

    for (int i = 0; i < local_count; ++i) {
      if ((selected_mask != NULL) && !selected_mask[i]) {
        continue;
      }

      if (append_unique_rom_id(
              local_items[i].rom_id,
              selected_rom_ids,
              &selected_rom_id_count,
              selected_local_count) < 0) {
        app_log_write(APP_LOG_LEVEL_ERROR, "sync", "failed to build remote filter rom_id set");
        emit_progress(
            config,
            completed_engine_units,
            total_engine_units,
            -1,
            local_count,
            "Sync failed: remote filter preparation failed");
        status = SYNC_ENGINE_ERR_INVALID_ARGUMENT;
        goto cleanup;
      }
    }
  }

  state_store = (SyncStateStore *)malloc(sizeof(*state_store));
  if (state_store == NULL) {
    app_log_write(APP_LOG_LEVEL_ERROR, "sync", "out of memory allocating state store");
    emit_progress(
        config,
        completed_engine_units,
        total_engine_units,
        -1,
        local_count,
        "Sync failed: insufficient memory");
    status = SYNC_ENGINE_ERR_OUT_OF_MEMORY;
    goto cleanup;
  }
  sync_state_store_init(state_store);

  if (has_text(config->state_store_path)) {
    app_log_write(APP_LOG_LEVEL_DEBUG, "sync", "loading state store: %s", config->state_store_path);
    int load_status = sync_state_store_load(config->state_store_path, state_store);
    if (load_status != SYNC_STATE_STORE_OK) {
      app_log_write(APP_LOG_LEVEL_ERROR, "sync", "state load failed: %s", sync_state_store_status_str(load_status));
      emit_progress(
          config,
          completed_engine_units,
          total_engine_units,
          -1,
          local_count,
          "Sync failed: state loading failed");
      status = SYNC_ENGINE_ERR_STATE_LOAD;
      goto cleanup;
    }
  }

  if (has_text(config->device_id) && !has_text(state_store->device_id)) {
    sync_state_store_set_device_id(state_store, config->device_id);
  }

  const char *active_device_id = has_text(config->device_id)
                                     ? config->device_id
                                     : state_store->device_id;

  remote_items = (SyncSaveDescriptor *)malloc(sizeof(*remote_items) * (size_t)ROMM_SYNC_MAX_ITEMS);
  if (remote_items == NULL) {
    app_log_write(APP_LOG_LEVEL_ERROR, "sync", "out of memory allocating remote item buffer");
    emit_progress(
        config,
        completed_engine_units,
        total_engine_units,
        -1,
        local_count,
        "Sync failed: insufficient memory");
    status = SYNC_ENGINE_ERR_OUT_OF_MEMORY;
    goto cleanup;
  }
  for (int i = 0; i < ROMM_SYNC_MAX_ITEMS; ++i) {
    sync_save_descriptor_init(&remote_items[i]);
  }

  app_log_write(
      APP_LOG_LEVEL_INFO,
      "sync",
      "scanning remote saves for %d mapped rom_id filter(s)...",
      selected_rom_id_count);
  int remote_count = romm_client_list_remote_saves(
      romm_client,
      selected_rom_ids,
      selected_rom_id_count,
      remote_items,
      ROMM_SYNC_MAX_ITEMS);
  if (remote_count < 0) {
    app_log_write(APP_LOG_LEVEL_ERROR, "sync", "remote listing failed: %s (%d)", romm_client_status_str(remote_count), remote_count);
    emit_progress(
        config,
        completed_engine_units,
        total_engine_units,
        -1,
        local_count,
        "Sync failed: remote listing error (%s)",
        romm_client_status_str(remote_count));
    status = SYNC_ENGINE_ERR_REMOTE_LIST;
    goto cleanup;
  }
  out_report->remote_count = remote_count;
  app_log_write(APP_LOG_LEVEL_INFO, "sync", "remote listing returned %d unique remote save entries", remote_count);
  completed_engine_units = 1;
  emit_progress(
      config,
      completed_engine_units,
      total_engine_units,
      -1,
      local_count,
      "Remote saves loaded: %d",
      remote_count);

  for (int i = 0; i < local_count; ++i) {
    const SyncSaveDescriptor *local_item = &local_items[i];
    SyncActionRecord *action = append_action_record(out_report, local_item);
    if (action == NULL) {
      break;
    }

    if ((selected_mask != NULL) && !selected_mask[i]) {
      action->action = SYNC_ACTION_SKIP;
      out_report->skipped += 1;
      set_reason(action, "skipped by PS1 latest-card rule");
      completed_engine_units = 2 + i;
      emit_progress(
          config,
          completed_engine_units,
          total_engine_units,
          i,
          local_count,
          "%s: %s",
          has_text(action->filename) ? action->filename : "(unnamed)",
          action->reason);
      continue;
    }

    char filename_fallback[ROMM_SYNC_MAX_FILENAME_LEN];
    const char *filename = local_item->filename;
    if (!has_text(filename) && has_text(local_item->path) &&
        (sync_extract_filename(local_item->path, filename_fallback, sizeof(filename_fallback)) == 0)) {
      filename = filename_fallback;
      snprintf(action->filename, sizeof(action->filename), "%s", filename_fallback);
    }
    const char *display_filename = has_text(action->filename)
                                       ? action->filename
                                       : (has_text(filename) ? filename : "(unnamed)");

    app_log_write(
        APP_LOG_LEVEL_INFO,
        "sync",
        "matching save with remote entry: game=%s file=%s",
        local_item->game_id,
        display_filename);
    emit_progress(
        config,
        completed_engine_units,
        total_engine_units,
        i,
        local_count,
        "Matching save with remote entry: %s",
        display_filename);

    const SyncStateEntry *state_entry =
        sync_state_store_find_const(state_store, local_item->game_id, filename, local_item->slot);

    int remote_index = game_matcher_find_remote_index(local_item, remote_items, remote_count);
    if (remote_index < 0) {
      if (local_item_is_missing_restore_target(local_item)) {
        action->action = SYNC_ACTION_SKIP;
        out_report->skipped += 1;
        set_reason(action, "skip missing local slot: no remote save found to restore");
        app_log_write(
            APP_LOG_LEVEL_INFO,
            "sync",
            "missing local restore target skipped game=%s file=%s: no remote match",
            local_item->game_id,
            display_filename);
        completed_engine_units = 2 + i;
        emit_progress(
            config,
            completed_engine_units,
            total_engine_units,
            i,
            local_count,
            "%s: %s",
            display_filename,
            action->reason);
        continue;
      }

      if (should_skip_redundant_upload(local_item, state_entry)) {
        action->action = SYNC_ACTION_SKIP;
        out_report->skipped += 1;
        set_reason(action, "skip unchanged upload candidate");
        app_log_write(APP_LOG_LEVEL_INFO, "sync", "upload skipped: already up to date");
        completed_engine_units = 2 + i;
        emit_progress(
            config,
            completed_engine_units,
            total_engine_units,
            i,
            local_count,
            "Upload skipped: already up to date");
        continue;
      }

      action->action = SYNC_ACTION_UPLOAD;
      out_report->uploads_planned += 1;
      set_reason(action, "upload candidate (no remote match)");
      app_log_write(
          APP_LOG_LEVEL_DEBUG,
          "sync",
          "upload candidate game=%s file=%s slot=%s",
          local_item->game_id,
          action->filename,
          sync_slot_str(local_item->slot));
      int upload_status = execute_upload(
          config,
          romm_client,
          active_device_id,
          local_item,
          NULL,
          state_store,
          action,
          out_report);
      if (upload_status == ROMM_CLIENT_ERR_CONFLICT) {
        action->action = SYNC_ACTION_SKIP;
        action->conflict = SYNC_CONFLICT_REMOTE_NEWER;
        out_report->conflicts_detected += 1;
        out_report->skipped += 1;
        set_reason(action, "server conflict (409): remote newer, download before upload");
        app_log_write(APP_LOG_LEVEL_WARN, "sync", "upload conflict game=%s file=%s", local_item->game_id, action->filename);
      }
      completed_engine_units = 2 + i;
      emit_progress(
          config,
          completed_engine_units,
          total_engine_units,
          i,
          local_count,
          "%s: %s",
          display_filename,
          action->reason);
      continue;
    }

    const SyncSaveDescriptor *remote_item = &remote_items[remote_index];
    log_sync_match_metadata(local_item, remote_item);
    if (local_item_is_missing_restore_target(local_item)) {
      action->action = SYNC_ACTION_DOWNLOAD;
      out_report->downloads_planned += 1;
      set_reason(action, "restoring missing local slot from remote");
      app_log_write(
          APP_LOG_LEVEL_INFO,
          "sync",
          "missing local slot detected: restoring from remote game=%s file=%s remote_id=%d",
          local_item->game_id,
          action->filename,
          remote_item->remote_id);
      execute_download(
          config,
          romm_client,
          local_item,
          remote_item,
          state_entry,
          state_store,
          action,
          out_report);
      completed_engine_units = 2 + i;
      emit_progress(
          config,
          completed_engine_units,
          total_engine_units,
          i,
          local_count,
          "%s: %s",
          display_filename,
          action->reason);
      continue;
    }

    if (remote_reports_current_for_device(local_item, remote_item)) {
      action->action = SYNC_ACTION_SKIP;
      out_report->skipped += 1;
      if (remote_item->remote_id >= 0) {
        set_reason(action, "skip remote %d (device already current)", remote_item->remote_id);
      } else {
        set_reason(action, "skip remote save (device already current)");
      }
      app_log_write(APP_LOG_LEVEL_INFO, "sync", "download skipped: already up to date");
      completed_engine_units = 2 + i;
      emit_progress(
          config,
          completed_engine_units,
          total_engine_units,
          i,
          local_count,
          "Download skipped: already up to date");
      continue;
    }

    if (remote_matches_known_uploaded_local(local_item, remote_item, state_entry)) {
      action->action = SYNC_ACTION_SKIP;
      out_report->skipped += 1;
      set_reason(action, "skip unchanged local save (already uploaded)");
      app_log_write(
          APP_LOG_LEVEL_INFO,
          "sync",
          "upload skipped: matched remote save already covers unchanged local file game=%s file=%s",
          local_item->game_id,
          display_filename);
      completed_engine_units = 2 + i;
      emit_progress(
          config,
          completed_engine_units,
          total_engine_units,
          i,
          local_count,
          "%s: %s",
          display_filename,
          action->reason);
      continue;
    }

    SyncConflictType conflict = conflict_resolver_detect(local_item, remote_item, active_device_id);
    action->conflict = conflict;

    if (conflict == SYNC_CONFLICT_NONE) {
      action->action = SYNC_ACTION_SKIP;
      out_report->skipped += 1;
      if (same_content_signature(local_item, remote_item) &&
          (local_item->timestamp_unix == remote_item->timestamp_unix)) {
        set_reason(action, "already synchronized");
      } else {
        set_reason(action, "metadata differs but no deterministic conflict");
      }
      completed_engine_units = 2 + i;
      emit_progress(
          config,
          completed_engine_units,
          total_engine_units,
          i,
          local_count,
          "%s: %s",
          display_filename,
          action->reason);
      continue;
    }

    if (conflict == SYNC_CONFLICT_SAME_ORIGIN_DEVICE) {
      action->action = SYNC_ACTION_SKIP;
      out_report->skipped += 1;
      set_reason(action, "skip same-origin remote save");
      app_log_write(APP_LOG_LEVEL_DEBUG, "sync", "skip same-origin game=%s file=%s", local_item->game_id, action->filename);
      completed_engine_units = 2 + i;
      emit_progress(
          config,
          completed_engine_units,
          total_engine_units,
          i,
          local_count,
          "%s: %s",
          display_filename,
          action->reason);
      continue;
    }

    out_report->conflicts_detected += 1;
    app_log_write(
        APP_LOG_LEVEL_WARN,
        "sync",
        "conflict game=%s file=%s type=%s",
        local_item->game_id,
        action->filename,
        sync_conflict_type_str(conflict));
    SyncActionType decision = conflict_resolver_default_action(conflict);
    if (decision == SYNC_ACTION_NONE) {
      decision = SYNC_ACTION_SKIP;
    }

    if (conflict_resolver_requires_confirmation(conflict)) {
      if (config->resolve_conflict != NULL) {
        SyncActionType callback_decision = config->resolve_conflict(
            action, local_item, remote_item, config->resolve_conflict_user_data);
        if ((callback_decision == SYNC_ACTION_UPLOAD) ||
            (callback_decision == SYNC_ACTION_DOWNLOAD) ||
            (callback_decision == SYNC_ACTION_SKIP)) {
          decision = callback_decision;
        }
      } else {
        decision = SYNC_ACTION_SKIP;
      }
    }

    if ((decision == SYNC_ACTION_DOWNLOAD) &&
        !sync_save_platform_restore_supported(local_item->platform)) {
      decision = SYNC_ACTION_SKIP;
      app_log_write(
          APP_LOG_LEVEL_WARN,
          "sync",
          "%s",
          vita_native_save_restore_unsupported_reason());
      set_reason(action, "%s", vita_native_save_restore_unsupported_reason());
    }

    action->action = decision;

    if (decision == SYNC_ACTION_UPLOAD) {
      out_report->uploads_planned += 1;
      set_reason(action, "upload selected for conflict=%s", sync_conflict_type_str(conflict));
      app_log_write(APP_LOG_LEVEL_INFO, "sync", "decision=upload game=%s file=%s", local_item->game_id, action->filename);
      int upload_status = execute_upload(
          config,
          romm_client,
          active_device_id,
          local_item,
          remote_item,
          state_store,
          action,
          out_report);
      if (upload_status == ROMM_CLIENT_ERR_CONFLICT) {
        action->action = SYNC_ACTION_SKIP;
        action->conflict = SYNC_CONFLICT_REMOTE_NEWER;
        out_report->skipped += 1;
        set_reason(action, "server conflict (409): remote newer, download before upload");
        app_log_write(APP_LOG_LEVEL_WARN, "sync", "upload conflict game=%s file=%s", local_item->game_id, action->filename);
      }
      completed_engine_units = 2 + i;
      emit_progress(
          config,
          completed_engine_units,
          total_engine_units,
          i,
          local_count,
          "%s: %s",
          display_filename,
          action->reason);
      continue;
    }

    if (decision == SYNC_ACTION_DOWNLOAD) {
      out_report->downloads_planned += 1;
      set_reason(action, "download selected for conflict=%s", sync_conflict_type_str(conflict));
      app_log_write(APP_LOG_LEVEL_INFO, "sync", "decision=download game=%s file=%s", local_item->game_id, action->filename);
      execute_download(
          config,
          romm_client,
          local_item,
          remote_item,
          state_entry,
          state_store,
          action,
          out_report);
      completed_engine_units = 2 + i;
      emit_progress(
          config,
          completed_engine_units,
          total_engine_units,
          i,
          local_count,
          "%s: %s",
          display_filename,
          action->reason);
      continue;
    }

    out_report->skipped += 1;
    if (!has_text(action->reason)) {
      if (conflict_resolver_requires_confirmation(conflict) && (config->resolve_conflict != NULL)) {
        set_reason(action, "skip selected during conflict review (conflict=%s)", sync_conflict_type_str(conflict));
      } else if (conflict_resolver_requires_confirmation(conflict)) {
        set_reason(action, "manual confirmation required (conflict=%s)", sync_conflict_type_str(conflict));
      } else {
        set_reason(action, "skipped (conflict=%s)", sync_conflict_type_str(conflict));
      }
    }
    completed_engine_units = 2 + i;
    emit_progress(
        config,
        completed_engine_units,
        total_engine_units,
        i,
        local_count,
        "%s: %s",
        display_filename,
        action->reason);
  }

  if (!config->dry_run && has_text(config->state_store_path)) {
    int save_status = sync_state_store_save(config->state_store_path, state_store);
    if (save_status != SYNC_STATE_STORE_OK) {
      app_log_write(APP_LOG_LEVEL_ERROR, "sync", "state save failed: %s", sync_state_store_status_str(save_status));
      emit_progress(
          config,
          completed_engine_units,
          total_engine_units,
          -1,
          local_count,
          "Sync failed: state saving failed");
      status = SYNC_ENGINE_ERR_STATE_SAVE;
      goto cleanup;
    }
  }
  app_log_write(
      APP_LOG_LEVEL_INFO,
      "sync",
      "run end uploads_planned=%d downloads_planned=%d conflicts=%d skipped=%d errors=%d",
      out_report->uploads_planned,
      out_report->downloads_planned,
      out_report->conflicts_detected,
      out_report->skipped,
      out_report->transfer_errors);
  emit_progress(
      config,
      total_engine_units,
      total_engine_units,
      -1,
      local_count,
      "Sync engine completed");

cleanup:
  free(selected_rom_ids);
  free(selection_reasons);
  free(selected_mask);
  free(remote_items);
  free(state_store);
  return status;
}

/*
 * Returns a human-readable message for sync engine status codes.
 */
const char *sync_engine_status_str(int status) {
  switch (status) {
    case SYNC_ENGINE_OK:
      return "ok";
    case SYNC_ENGINE_ERR_INVALID_ARGUMENT:
      return "invalid argument";
    case SYNC_ENGINE_ERR_REMOTE_LIST:
      return "remote listing failed";
    case SYNC_ENGINE_ERR_STATE_LOAD:
      return "state loading failed";
    case SYNC_ENGINE_ERR_STATE_SAVE:
      return "state saving failed";
    case SYNC_ENGINE_ERR_UNRESOLVED_ROM_ID:
      return "unresolved rom_id";
    case SYNC_ENGINE_ERR_OUT_OF_MEMORY:
      return "out of memory";
    default:
      return "unknown error";
  }
}
