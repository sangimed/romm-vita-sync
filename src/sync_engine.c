#include "sync_engine.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "backup_manager.h"
#include "conflict_resolver.h"
#include "game_matcher.h"
#include "sync_state_store.h"

#define SYNC_DEFAULT_BACKUP_DIRECTORY "ux0:data/romm-vita-sync/backups"

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
 */
static int execute_upload(
    const SyncEngineConfig *config,
    const RommClient *romm_client,
    const char *device_id,
    const SyncSaveDescriptor *local_item,
    SyncStateStore *state_store,
    SyncActionRecord *action,
    SyncRunReport *report) {
  if ((config == NULL) || (romm_client == NULL) || (local_item == NULL) ||
      (state_store == NULL) || (action == NULL) || (report == NULL)) {
    return SYNC_ENGINE_ERR_INVALID_ARGUMENT;
  }

  if (config->dry_run) {
    action->executed = 0;
    action->status_code = SYNC_ENGINE_OK;
    set_reason(action, "planned upload (dry-run)");
    return SYNC_ENGINE_OK;
  }

  int status = romm_client_upload_save(romm_client, local_item);
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
  set_reason(action, "uploaded");

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
    action->executed = 0;
    action->status_code = SYNC_ENGINE_OK;
    set_reason(action, "planned download (dry-run)");
    return SYNC_ENGINE_OK;
  }

  if (file_exists(local_item->path)) {
    const char *backup_directory = has_text(config->backup_directory)
                                       ? config->backup_directory
                                       : SYNC_DEFAULT_BACKUP_DIRECTORY;
    int64_t now = (config->now_callback != NULL) ? config->now_callback() : default_now_callback();
    int backup_status = backup_manager_backup_file(local_item->path, backup_directory, now, NULL, 0U);
    if (backup_status != BACKUP_MANAGER_OK) {
      action->status_code = backup_status;
      set_reason(action, "backup failed (%s)", backup_manager_status_str(backup_status));
      report->transfer_errors += 1;
      return backup_status;
    }
  }

  int status = romm_client_download_save(romm_client, remote_item, local_item->path);
  action->status_code = status;
  if (status < 0) {
    action->executed = 0;
    report->transfer_errors += 1;
    set_reason(action, "download failed (%s)", romm_client_status_str(status));
    return status;
  }

  action->executed = 1;
  report->downloads_executed += 1;
  set_reason(action, "downloaded");

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
 * local scan -> remote compare -> action decisions -> optional execution.
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

  SyncStateStore state_store;
  sync_state_store_init(&state_store);

  if (has_text(config->state_store_path)) {
    int load_status = sync_state_store_load(config->state_store_path, &state_store);
    if (load_status != SYNC_STATE_STORE_OK) {
      return SYNC_ENGINE_ERR_STATE_LOAD;
    }
  }

  if (has_text(config->device_id) && !has_text(state_store.device_id)) {
    sync_state_store_set_device_id(&state_store, config->device_id);
  }

  const char *active_device_id = has_text(config->device_id)
                                     ? config->device_id
                                     : state_store.device_id;

  SyncSaveDescriptor remote_items[ROMM_SYNC_MAX_ITEMS];
  for (int i = 0; i < ROMM_SYNC_MAX_ITEMS; ++i) {
    sync_save_descriptor_init(&remote_items[i]);
  }

  int remote_count = romm_client_list_remote_saves(
      romm_client, remote_items, (int)(sizeof(remote_items) / sizeof(remote_items[0])));
  if (remote_count < 0) {
    return SYNC_ENGINE_ERR_REMOTE_LIST;
  }
  out_report->remote_count = remote_count;

  for (int i = 0; i < local_count; ++i) {
    const SyncSaveDescriptor *local_item = &local_items[i];
    SyncActionRecord *action = append_action_record(out_report, local_item);
    if (action == NULL) {
      break;
    }

    char filename_fallback[ROMM_SYNC_MAX_FILENAME_LEN];
    const char *filename = local_item->filename;
    if (!has_text(filename) && has_text(local_item->path) &&
        (sync_extract_filename(local_item->path, filename_fallback, sizeof(filename_fallback)) == 0)) {
      filename = filename_fallback;
      snprintf(action->filename, sizeof(action->filename), "%s", filename_fallback);
    }

    const SyncStateEntry *state_entry =
        sync_state_store_find_const(&state_store, local_item->game_id, filename, local_item->slot);

    int remote_index = game_matcher_find_remote_index(local_item, remote_items, remote_count);
    if (remote_index < 0) {
      if (should_skip_redundant_upload(local_item, state_entry)) {
        action->action = SYNC_ACTION_SKIP;
        out_report->skipped += 1;
        set_reason(action, "skip unchanged upload candidate");
        continue;
      }

      action->action = SYNC_ACTION_UPLOAD;
      out_report->uploads_planned += 1;
      set_reason(action, "upload candidate (no remote match)");
      int upload_status = execute_upload(config, romm_client, active_device_id, local_item, &state_store, action, out_report);
      if (upload_status == ROMM_CLIENT_ERR_CONFLICT) {
        action->action = SYNC_ACTION_SKIP;
        action->conflict = SYNC_CONFLICT_REMOTE_NEWER;
        out_report->conflicts_detected += 1;
        out_report->skipped += 1;
        set_reason(action, "server conflict (409): remote newer, download before upload");
      }
      continue;
    }

    const SyncSaveDescriptor *remote_item = &remote_items[remote_index];
    if (remote_reports_current_for_device(local_item, remote_item)) {
      action->action = SYNC_ACTION_SKIP;
      out_report->skipped += 1;
      if (remote_item->remote_id >= 0) {
        set_reason(action, "skip remote %d (device already current)", remote_item->remote_id);
      } else {
        set_reason(action, "skip remote save (device already current)");
      }
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
      continue;
    }

    if (conflict == SYNC_CONFLICT_SAME_ORIGIN_DEVICE) {
      action->action = SYNC_ACTION_SKIP;
      out_report->skipped += 1;
      set_reason(action, "skip same-origin remote save");
      continue;
    }

    out_report->conflicts_detected += 1;
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

    action->action = decision;

    if (decision == SYNC_ACTION_UPLOAD) {
      out_report->uploads_planned += 1;
      set_reason(action, "upload selected for conflict=%s", sync_conflict_type_str(conflict));
      int upload_status = execute_upload(config, romm_client, active_device_id, local_item, &state_store, action, out_report);
      if (upload_status == ROMM_CLIENT_ERR_CONFLICT) {
        action->action = SYNC_ACTION_SKIP;
        action->conflict = SYNC_CONFLICT_REMOTE_NEWER;
        out_report->skipped += 1;
        set_reason(action, "server conflict (409): remote newer, download before upload");
      }
      continue;
    }

    if (decision == SYNC_ACTION_DOWNLOAD) {
      out_report->downloads_planned += 1;
      set_reason(action, "download selected for conflict=%s", sync_conflict_type_str(conflict));
      execute_download(
          config,
          romm_client,
          local_item,
          remote_item,
          state_entry,
          &state_store,
          action,
          out_report);
      continue;
    }

    out_report->skipped += 1;
    set_reason(action, "manual confirmation required (conflict=%s)", sync_conflict_type_str(conflict));
  }

  if (!config->dry_run && has_text(config->state_store_path)) {
    int save_status = sync_state_store_save(config->state_store_path, &state_store);
    if (save_status != SYNC_STATE_STORE_OK) {
      return SYNC_ENGINE_ERR_STATE_SAVE;
    }
  }

  return SYNC_ENGINE_OK;
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
    default:
      return "unknown error";
  }
}
