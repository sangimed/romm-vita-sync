#include "ui_sync_orchestrator.h"

#include <psp2/ctrl.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "app_log.h"
#include "backup_manager.h"
#include "conflict_resolver.h"
#include "ps1_paths.h"
#include "romm_client.h"
#include "romm_http_client.h"
#include "scan_to_sync_adapter.h"
#include "save_scanner.h"
#include "sync_engine.h"
#include "vita_native_save_policy.h"
#include "vita_native_save_scanner.h"
#include "ui_common.h"
#include "ui_config_editor.h"
#include "ui_dialogs.h"
#include "ui_navigation.h"
#include "ui_render.h"
#include "ui_screens.h"
#include "ui_sync_modal.h"

static int ui_item_matches_game_key(const SyncSaveDescriptor *item, const char *key) {
  if ((item == NULL) || !has_text(key)) {
    return 0;
  }

  char item_key[ROMM_GAME_ID_LEN];
  ui_build_game_key(item, item_key, sizeof(item_key));
  return sync_string_ieq(item_key, key);
}

static int ui_collect_selected_game_items(
    const UiAppState *state,
    SyncSaveDescriptor *out_items,
    int max_items,
    int *out_selected_games,
    int *out_selected_targets) {
  if ((state == NULL) || (out_items == NULL) || (max_items <= 0)) {
    return 0;
  }

  int selected_games = 0;
  int selected_targets = 0;
  int count = 0;

  for (int game_index = 0; game_index < state->game_count; ++game_index) {
    const UiGameEntry *game = &state->games[game_index];
    if (!game->selected_for_sync) {
      continue;
    }

    selected_games += 1;
    for (int i = 0; i < state->local_count; ++i) {
      if (!ui_item_matches_game_key(&state->local_items[i], game->key)) {
        continue;
      }

      selected_targets += 1;
      if (count >= max_items) {
        continue;
      }

      memcpy(&out_items[count], &state->local_items[i], sizeof(out_items[count]));
      count += 1;
    }
  }

  if (out_selected_games != NULL) {
    *out_selected_games = selected_games;
  }
  if (out_selected_targets != NULL) {
    *out_selected_targets = selected_targets;
  }
  return count;
}

static int ui_estimate_ps1_sync_candidate_count(
    const SyncSaveDescriptor *items,
    int item_count) {
  if ((items == NULL) || (item_count <= 0) || (item_count > ROMM_SYNC_MAX_ITEMS)) {
    return 0;
  }

  int selected_mask[ROMM_SYNC_MAX_ITEMS];
  int selected_count = sync_select_latest_local_per_game(
      items,
      item_count,
      selected_mask,
      NULL);
  if (selected_count < 0) {
    return item_count;
  }

  return selected_count;
}

static const SyncSaveDescriptor *ui_find_slot1_tie_peer(
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

static const char *ui_sync_local_selection_reason_str(SyncLocalSelectionReason reason) {
  switch (reason) {
    case SYNC_LOCAL_SELECTION_ONLY_ITEM:
      return "only_item";
    case SYNC_LOCAL_SELECTION_LATEST_TIMESTAMP:
      return "latest_timestamp";
    case SYNC_LOCAL_SELECTION_EQUAL_TIMESTAMP_PREFER_SLOT0:
      return "equal_timestamp_prefer_slot0";
    case SYNC_LOCAL_SELECTION_DETERMINISTIC_FALLBACK:
      return "deterministic_fallback";
    case SYNC_LOCAL_SELECTION_NOT_SELECTED:
    default:
      return "not_selected";
  }
}

static int ui_prepare_ps1_sync_candidates(
    SyncSaveDescriptor *items,
    int item_count,
    SyncSavePlatform platform,
    int *out_warning_count) {
  if ((items == NULL) || (item_count < 0) || (item_count > ROMM_SYNC_MAX_ITEMS)) {
    return 0;
  }

  int selected_mask[ROMM_SYNC_MAX_ITEMS];
  SyncLocalSelectionReason selection_reasons[ROMM_SYNC_MAX_ITEMS];
  int selected_count = sync_select_latest_local_per_game(
      items,
      item_count,
      selected_mask,
      selection_reasons);
  if (selected_count < 0) {
    return item_count;
  }

  if (out_warning_count != NULL) {
    *out_warning_count = 0;
  }

  if ((item_count > 0) && (selected_count != item_count)) {
    ui_sync_log_write(
        APP_LOG_LEVEL_INFO,
        "%s latest-local rule active: %d sync candidate(s) selected from %d local target(s)",
        sync_save_platform_short_label(platform),
        selected_count,
        item_count);
  }

  int write_index = 0;
  for (int i = 0; i < item_count; ++i) {
    if (!selected_mask[i]) {
      continue;
    }

    char selected_timestamp[32];
    sync_format_timestamp(items[i].timestamp_unix, selected_timestamp, sizeof(selected_timestamp));
    ui_sync_log_write(
        APP_LOG_LEVEL_INFO,
        "%s candidate selected: game=%s file=%s slot=%s timestamp=%s unix=%lld reason=%s",
        sync_save_platform_short_label(platform),
        has_text(items[i].game_id) ? items[i].game_id : "(unknown)",
        has_text(items[i].filename) ? items[i].filename : items[i].path,
        sync_slot_str(items[i].slot),
        selected_timestamp,
        (long long)items[i].timestamp_unix,
        ui_sync_local_selection_reason_str(selection_reasons[i]));

    if (selection_reasons[i] == SYNC_LOCAL_SELECTION_EQUAL_TIMESTAMP_PREFER_SLOT0) {
      const SyncSaveDescriptor *slot1_peer = ui_find_slot1_tie_peer(items, item_count, i);
      if (slot1_peer != NULL) {
        char skipped_timestamp[32];
        sync_format_timestamp(slot1_peer->timestamp_unix, skipped_timestamp, sizeof(skipped_timestamp));
        ui_sync_log_write(
            APP_LOG_LEVEL_WARN,
            "Equal local timestamps for %s; defaulting to %s and skipping %s (selected_ts=%s skipped_ts=%s)",
            has_text(items[i].title) ? items[i].title : items[i].game_id,
            has_text(items[i].filename) ? items[i].filename : items[i].path,
            has_text(slot1_peer->filename) ? slot1_peer->filename : slot1_peer->path,
            selected_timestamp,
            skipped_timestamp);
        if (out_warning_count != NULL) {
          *out_warning_count += 1;
        }
      }
    }

    if (write_index != i) {
      memmove(&items[write_index], &items[i], sizeof(items[write_index]));
    }
    write_index += 1;
  }

  return write_index;
}

static void ui_sync_append_report_logs(const SyncRunReport *report) {
  if (report == NULL) {
    return;
  }

  ui_sync_log_write(
      APP_LOG_LEVEL_INFO,
      "Sync summary: uploads=%d/%d downloads=%d/%d skipped=%d conflicts=%d errors=%d",
      report->uploads_executed,
      report->uploads_planned,
      report->downloads_executed,
      report->downloads_planned,
      report->skipped,
      report->conflicts_detected,
      report->transfer_errors);

  int render_count = report->action_count;
  if (render_count > 20) {
    render_count = 20;
  }

  for (int i = 0; i < render_count; ++i) {
    const SyncActionRecord *action = &report->actions[i];
    AppLogLevel level = (action->status_code < 0) ? APP_LOG_LEVEL_ERROR : APP_LOG_LEVEL_INFO;
    const char *action_name = has_text(action->filename)
                                  ? action->filename
                                  : (has_text(action->game_id) ? action->game_id : "(unnamed)");
    ui_sync_log_write(
        level,
        "Action %02d: %s [%s] %s %s (%s)",
        i + 1,
        action_name,
        sync_slot_str(action->slot),
        sync_action_type_str(action->action),
        action->executed ? "executed" : "planned",
        action->reason);
  }

  if (report->action_count > render_count) {
    ui_sync_log_write(APP_LOG_LEVEL_INFO, "... %d additional action(s) omitted", report->action_count - render_count);
  }
}

static int ui_sync_report_has_restore_blocked_skip(const SyncRunReport *report) {
  if (report == NULL) {
    return 0;
  }

  for (int i = 0; i < report->action_count; ++i) {
    const SyncActionRecord *action = &report->actions[i];
    if ((action->action == SYNC_ACTION_SKIP) &&
        has_text(action->reason) &&
        (strstr(action->reason, "restore not supported") != NULL)) {
      return 1;
    }
  }
  return 0;
}

static void ui_sync_engine_progress_callback(
    int completed_units,
    int total_units,
    int local_index,
    int local_total,
    const char *message,
    void *user_data) {
  (void)local_index;
  (void)local_total;
  (void)total_units;

  UiSyncProgressBridge *bridge = (UiSyncProgressBridge *)user_data;
  if ((bridge == NULL) || (bridge->state == NULL)) {
    return;
  }

  UiAppState *state = bridge->state;
  UiSyncFeedback *feedback = &state->sync_feedback;
  ui_sync_feedback_set_progress(
      feedback,
      bridge->base_completed_units + completed_units,
      bridge->overall_total_units);
  if (has_text(message)) {
    ui_sync_feedback_set_message(feedback, message);
  }
  ui_sync_render_live(state);
}

static void ui_format_sync_timestamp(int64_t timestamp_unix, char *out_text, size_t out_size) {
  if ((out_text == NULL) || (out_size == 0U)) {
    return;
  }

  if (timestamp_unix <= 0) {
    snprintf(out_text, out_size, "unknown");
    return;
  }

  time_t raw = (time_t)timestamp_unix;
  struct tm *local = localtime(&raw);
  if (local == NULL) {
    snprintf(out_text, out_size, "%lld", (long long)timestamp_unix);
    return;
  }

  snprintf(
      out_text,
      out_size,
      "%04d-%02d-%02d %02d:%02d:%02d",
      local->tm_year + 1900,
      local->tm_mon + 1,
      local->tm_mday,
      local->tm_hour,
      local->tm_min,
      local->tm_sec);
}

static const char *ui_sync_conflict_summary(SyncConflictType conflict) {
  switch (conflict) {
    case SYNC_CONFLICT_LOCAL_NEWER:
      return "Local save is newer than the remote save.";
    case SYNC_CONFLICT_REMOTE_NEWER:
      return "Remote save is newer than the local save.";
    case SYNC_CONFLICT_SAME_TIMESTAMP_DIFFERENT_CONTENT:
      return "Local and remote saves share the same timestamp but differ in content.";
    case SYNC_CONFLICT_SAME_ORIGIN_DEVICE:
      return "Remote save already belongs to this device.";
    case SYNC_CONFLICT_NONE:
    default:
      return "No conflict detected.";
  }
}

static const char *ui_sync_action_phrase(SyncActionType action) {
  switch (action) {
    case SYNC_ACTION_UPLOAD:
      return "upload the local save to RomM";
    case SYNC_ACTION_DOWNLOAD:
      return "download the remote save to this Vita";
    case SYNC_ACTION_SKIP:
    case SYNC_ACTION_NONE:
    default:
      return "skip this save for now";
  }
}

static void ui_build_conflict_prompt_message(
    char *out_message,
    size_t out_size,
    const SyncSaveDescriptor *local_item,
    const SyncSaveDescriptor *remote_item,
    SyncConflictType conflict,
    SyncActionType action,
    int dry_run,
    const char *question,
    const char *decline_outcome) {
  if ((out_message == NULL) || (out_size == 0U)) {
    return;
  }

  char local_timestamp[64];
  char remote_timestamp[64];
  ui_format_sync_timestamp(local_item != NULL ? local_item->timestamp_unix : 0, local_timestamp, sizeof(local_timestamp));
  ui_format_sync_timestamp(remote_item != NULL ? remote_item->timestamp_unix : 0, remote_timestamp, sizeof(remote_timestamp));

  const char *title = (local_item != NULL) && has_text(local_item->title)
                          ? local_item->title
                          : (((local_item != NULL) && has_text(local_item->game_id)) ? local_item->game_id : "(unknown)");
  const char *filename = (local_item != NULL) && has_text(local_item->filename)
                             ? local_item->filename
                             : (((remote_item != NULL) && has_text(remote_item->filename)) ? remote_item->filename : "(unknown)");
  const char *question_text = has_text(question) ? question : "Apply the recommended action?";
  const char *decline_text = has_text(decline_outcome) ? decline_outcome : "skip this save";

  char title_display[96];
  char filename_display[80];
  char summary_display[96];
  char action_display[96];
  char question_display[128];
  char decline_display[96];
  ui_truncate_text(title, title_display, sizeof(title_display));
  ui_truncate_text(filename, filename_display, sizeof(filename_display));
  ui_truncate_text(ui_sync_conflict_summary(conflict), summary_display, sizeof(summary_display));
  ui_truncate_text(ui_sync_action_phrase(action), action_display, sizeof(action_display));
  ui_truncate_text(question_text, question_display, sizeof(question_display));
  ui_truncate_text(decline_text, decline_display, sizeof(decline_display));

  snprintf(
      out_message,
      out_size,
      "Conflict for %s\n"
      "File: %s (%s)\n\n"
      "Local : %s | %llu B\n"
      "Remote: %s | %llu B\n\n"
      "%s\n"
      "Recommended action: %s.\n\n"
      "%s%s\n\n"
      "Press %s to %s.\n"
      "Press %s to %s.",
      title_display,
      filename_display,
      (local_item != NULL) ? sync_slot_str(local_item->slot) : "unknown",
      local_timestamp,
      (unsigned long long)((local_item != NULL) ? local_item->size_bytes : 0U),
      remote_timestamp,
      (unsigned long long)((remote_item != NULL) ? remote_item->size_bytes : 0U),
      summary_display,
      action_display,
      dry_run ? "Dry-run: approving this will only plan the action.\n\n" : "",
      question_display,
      ui_dialog_confirm_button_label(),
      action_display,
      ui_dialog_decline_button_label(),
      decline_display);
}

static SyncActionType ui_sync_resolve_conflict_callback(
    SyncActionRecord *candidate_action,
    const SyncSaveDescriptor *local_item,
    const SyncSaveDescriptor *remote_item,
    void *user_data) {
  UiSyncConflictResolutionContext *context = (UiSyncConflictResolutionContext *)user_data;
  SyncConflictType conflict =
      (candidate_action != NULL) ? candidate_action->conflict : conflict_resolver_detect(local_item, remote_item, NULL);
  SyncActionType recommended_action = conflict_resolver_default_action(conflict);
  const char *filename = (candidate_action != NULL) && has_text(candidate_action->filename)
                             ? candidate_action->filename
                             : (((local_item != NULL) && has_text(local_item->filename)) ? local_item->filename : "(unknown)");

  if (recommended_action == SYNC_ACTION_NONE) {
    recommended_action = SYNC_ACTION_SKIP;
  }

  if ((context == NULL) || (context->state == NULL)) {
    return recommended_action;
  }

  UiAppState *state = context->state;
  if (context->auto_apply_conflicts) {
    if (candidate_action != NULL) {
      snprintf(
          candidate_action->reason,
          sizeof(candidate_action->reason),
          "auto-applied recommended action=%s (conflict=%s)",
          sync_action_type_str(recommended_action),
          sync_conflict_type_str(conflict));
    }
    ui_sync_log_write(
        APP_LOG_LEVEL_INFO,
        "Conflict auto-applied: file=%s conflict=%s action=%s trigger=%s",
        filename,
        sync_conflict_type_str(conflict),
        sync_action_type_str(recommended_action),
        (context->trigger == UI_SYNC_TRIGGER_AUTOMATIC) ? "automatic" : "manual");
    return recommended_action;
  }

  if (context->trigger == UI_SYNC_TRIGGER_AUTOMATIC) {
    if (candidate_action != NULL) {
      snprintf(
          candidate_action->reason,
          sizeof(candidate_action->reason),
          "auto sync deferred: review conflict manually (recommended=%s, conflict=%s)",
          sync_action_type_str(recommended_action),
          sync_conflict_type_str(conflict));
    }
    ui_sync_log_write(
        APP_LOG_LEVEL_WARN,
        "Auto sync deferred conflict review: file=%s conflict=%s recommended=%s",
        filename,
        sync_conflict_type_str(conflict),
        sync_action_type_str(recommended_action));
    return SYNC_ACTION_SKIP;
  }

  ui_sync_feedback_set_message(&state->sync_feedback, "Waiting for conflict confirmation...");
  ui_sync_render_live(state);

  ui_sync_log_write(
      APP_LOG_LEVEL_WARN,
      "Conflict review required: file=%s conflict=%s recommended=%s confirm=%s decline=%s",
      filename,
      sync_conflict_type_str(conflict),
      sync_action_type_str(recommended_action),
      ui_dialog_confirm_button_label(),
      ui_dialog_decline_button_label());

  char prompt[UI_DIALOG_MSG_MAX_LEN];
  int response = 0;

  if (conflict == SYNC_CONFLICT_SAME_TIMESTAMP_DIFFERENT_CONTENT) {
    ui_build_conflict_prompt_message(
        prompt, sizeof(prompt), local_item, remote_item, conflict, SYNC_ACTION_UPLOAD,
        context->dry_run, "Upload the local save to RomM?", "show the download option");
    response = ui_dialog_confirm(prompt);
    if (response < 0) {
      if (candidate_action != NULL) {
        snprintf(candidate_action->reason, sizeof(candidate_action->reason), "conflict dialog failed during upload choice");
      }
      ui_sync_log_write(APP_LOG_LEVEL_ERROR, "Conflict dialog failed for file=%s", filename);
      return SYNC_ACTION_SKIP;
    }
    if (response == 1) {
      ui_sync_log_write(APP_LOG_LEVEL_INFO, "%s approved: file=%s action=upload",
          context->dry_run ? "Dry-run conflict plan" : "Conflict action", filename);
      return SYNC_ACTION_UPLOAD;
    }

    ui_build_conflict_prompt_message(
        prompt, sizeof(prompt), local_item, remote_item, conflict, SYNC_ACTION_DOWNLOAD,
        context->dry_run, "Download the remote save to this Vita?", "skip this save");
    response = ui_dialog_confirm(prompt);
    if (response < 0) {
      if (candidate_action != NULL) {
        snprintf(candidate_action->reason, sizeof(candidate_action->reason), "conflict dialog failed during download choice");
      }
      ui_sync_log_write(APP_LOG_LEVEL_ERROR, "Conflict dialog failed for file=%s", filename);
      return SYNC_ACTION_SKIP;
    }
    if (response == 1) {
      ui_sync_log_write(APP_LOG_LEVEL_INFO, "%s approved: file=%s action=download",
          context->dry_run ? "Dry-run conflict plan" : "Conflict action", filename);
      return SYNC_ACTION_DOWNLOAD;
    }

    if (candidate_action != NULL) {
      snprintf(candidate_action->reason, sizeof(candidate_action->reason),
          "user skipped after reviewing same-content conflict");
    }
    ui_sync_log_write(APP_LOG_LEVEL_INFO,
        "Conflict skipped by user: file=%s conflict=%s alternatives=upload,download",
        filename, sync_conflict_type_str(conflict));
    return SYNC_ACTION_SKIP;
  }

  ui_build_conflict_prompt_message(
      prompt, sizeof(prompt), local_item, remote_item, conflict, recommended_action,
      context->dry_run, "Apply the recommended action?", "skip this save");
  response = ui_dialog_confirm(prompt);
  if (response < 0) {
    if (candidate_action != NULL) {
      snprintf(candidate_action->reason, sizeof(candidate_action->reason), "conflict dialog failed");
    }
    ui_sync_log_write(APP_LOG_LEVEL_ERROR, "Conflict dialog failed for file=%s", filename);
    return SYNC_ACTION_SKIP;
  }

  if (response == 1) {
    ui_sync_log_write(APP_LOG_LEVEL_INFO, "%s approved: file=%s action=%s",
        context->dry_run ? "Dry-run conflict plan" : "Conflict action",
        filename, sync_action_type_str(recommended_action));
    return recommended_action;
  }

  if (candidate_action != NULL) {
    snprintf(candidate_action->reason, sizeof(candidate_action->reason),
        "user skipped after reviewing conflict=%s", sync_conflict_type_str(conflict));
  }
  ui_sync_log_write(APP_LOG_LEVEL_INFO,
      "Conflict skipped by user: file=%s conflict=%s recommended=%s",
      filename, sync_conflict_type_str(conflict), sync_action_type_str(recommended_action));
  return SYNC_ACTION_SKIP;
}

static void ui_present_completed_manual_sync(UiAppState *state) {
  if (state == NULL) {
    return;
  }

  unsigned int previous_buttons = ui_poll_buttons();
  state->sync_feedback.modal_scroll_hold_direction = 0;
  state->sync_feedback.modal_scroll_hold_frames = 0;
  ui_sync_modal_reset_touch(&state->sync_feedback);
  for (;;) {
    ui_pump_app_events();
    ui_sync_modal_handle_input(state);
    ui_render_sync_modal(state);

    unsigned int pressed = ui_poll_pressed(&previous_buttons);
    if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_CROSS | SCE_CTRL_START)) {
      ui_sync_modal_reset_touch(&state->sync_feedback);
      return;
    }

    sceKernelDelayThread(16 * 1000);
  }
}

int ui_refresh_local_inventory(UiAppState *state) {
  if (state == NULL) {
    return -1;
  }

  char previous_active_key[ROMM_GAME_ID_LEN];
  previous_active_key[0] = '\0';
  const UiGameEntry *previous_active = ui_active_game(state);
  if (previous_active != NULL) {
    snprintf(previous_active_key, sizeof(previous_active_key), "%s", previous_active->key);
  }

  char previous_selected_keys[ROMM_SYNC_MAX_ITEMS][ROMM_GAME_ID_LEN];
  int previous_selected_count = 0;
  for (int i = 0; i < state->game_count; ++i) {
    if (!state->games[i].selected_for_sync || !has_text(state->games[i].key)) {
      continue;
    }
    if (previous_selected_count >= ROMM_SYNC_MAX_ITEMS) {
      break;
    }
    snprintf(
        previous_selected_keys[previous_selected_count],
        sizeof(previous_selected_keys[previous_selected_count]),
        "%s",
        state->games[i].key);
    previous_selected_count += 1;
  }

  if (state->selected_save_platform == SYNC_SAVE_PLATFORM_VITA_NATIVE_EXPERIMENTAL) {
    ui_set_status(state, "Scanning local Vita native saves...");
    ui_render_busy_screen("Scanning local Vita native saves", "Path: ux0:user/00/savedata");
    app_log_write(APP_LOG_LEVEL_INFO, "ui", "selected sync platform=%s", sync_save_platform_id(state->selected_save_platform));

    state->local_count = vita_native_scan_save_containers(
        VITA_NATIVE_SAVEDATA_ROOT,
        state->config.log_scan_verbose,
        state->local_items,
        (int)(sizeof(state->local_items) / sizeof(state->local_items[0])));
    if (state->local_count < 0) {
      state->local_count = 0;
      state->game_count = 0;
      state->active_game_index = -1;
      state->game_scroll = 0;
      ui_refresh_game_filter(state);
      ui_set_status(state, "Local Vita native scan failed");
      app_log_write(APP_LOG_LEVEL_ERROR, "ui", "vita native scan failed");
      return -1;
    }
  } else {
    ui_set_status(state, "Scanning local PS1 saves...");
    ui_render_busy_screen("Scanning local PS1 saves", "Path: ux0:pspemu/PSP/SAVEDATA");

    memset(&state->scan_result, 0, sizeof(state->scan_result));
    int scan_status = scan_vmp_files(
        kPs1VmpCandidateRoots,
        (int)PS1_VMP_CANDIDATE_ROOT_COUNT,
        2,
        state->config.log_scan_verbose,
        &state->scan_result);
    if (scan_status < 0) {
      state->local_count = 0;
      state->game_count = 0;
      state->active_game_index = -1;
      state->game_scroll = 0;
      ui_refresh_game_filter(state);
      ui_set_status(state, "Local scan failed: %d", scan_status);
      app_log_write(APP_LOG_LEVEL_ERROR, "ui", "local scan failed status=%d", scan_status);
      return scan_status;
    }

    state->local_count = scan_result_to_sync_saves(
        &state->scan_result,
        state->local_items,
        (int)(sizeof(state->local_items) / sizeof(state->local_items[0])));
    if (state->local_count < 0) {
      state->local_count = 0;
      state->game_count = 0;
      state->active_game_index = -1;
      state->game_scroll = 0;
      ui_refresh_game_filter(state);
      ui_set_status(state, "Failed to build sync inventory from scan result");
      app_log_write(APP_LOG_LEVEL_ERROR, "ui", "scan_result_to_sync_saves failed");
      return -1;
    }
  }

  state->game_count = ui_build_game_entries(
      state->local_items,
      state->local_count,
      state->games,
      (int)(sizeof(state->games) / sizeof(state->games[0])));

  int restored_selected_count = 0;
  for (int i = 0; i < state->game_count; ++i) {
    state->games[i].selected_for_sync = 0;
    for (int j = 0; j < previous_selected_count; ++j) {
      if (sync_string_ieq(state->games[i].key, previous_selected_keys[j])) {
        state->games[i].selected_for_sync = 1;
        restored_selected_count += 1;
        break;
      }
    }
  }

  if ((state->game_count > 0) && has_text(previous_active_key)) {
    int restored_index = ui_find_game_entry(state->games, state->game_count, previous_active_key);
    state->active_game_index = (restored_index >= 0) ? restored_index : 0;
  } else {
    state->active_game_index = (state->game_count > 0) ? 0 : -1;
  }

  if ((restored_selected_count <= 0) && (state->game_count > 0)) {
    int fallback_index = state->active_game_index;
    if ((fallback_index < 0) || (fallback_index >= state->game_count)) {
      fallback_index = 0;
    }
    state->games[fallback_index].selected_for_sync = 1;
  }

  state->game_scroll = 0;
  ui_refresh_game_filter(state);
  ui_set_status(
      state,
      "Scan complete: %d %s games (%d local target%s)",
      state->game_count,
      sync_save_platform_short_label(state->selected_save_platform),
      state->local_count,
      (state->local_count == 1) ? "" : "s");
  app_log_write(
      APP_LOG_LEVEL_INFO,
      "ui",
      "scan complete games=%d saves=%d access_errors=%d",
      state->game_count,
      state->local_count,
      state->scan_result.stats.access_errors);
  return 0;
}

int ui_run_sync_pipeline(
    UiAppState *state,
    SyncSaveDescriptor *work_items,
    int work_item_count,
    UiSyncTrigger trigger,
    const char *title,
    const char *context) {
  if ((state == NULL) || (work_items == NULL) || (work_item_count < 0)) {
    return SYNC_ENGINE_ERR_INVALID_ARGUMENT;
  }

  memset(&state->sync_report, 0, sizeof(state->sync_report));
  app_log_clear_history();
  ui_sync_feedback_reset(&state->sync_feedback, trigger, title, context);

  int detected_item_count = work_item_count;
  int selection_warning_count = 0;

  ui_sync_log_write(APP_LOG_LEVEL_INFO, "Scanning local saves...");
  int preview_count = detected_item_count;
  if (preview_count > 24) {
    preview_count = 24;
  }
  for (int i = 0; i < preview_count; ++i) {
    const SyncSaveDescriptor *item = &work_items[i];
    const char *name = has_text(item->filename) ? item->filename : item->path;
    char timestamp[32];
    sync_format_timestamp(item->timestamp_unix, timestamp, sizeof(timestamp));
    ui_sync_log_write(
        APP_LOG_LEVEL_INFO,
        "Save detected: %s slot=%s timestamp=%s unix=%lld",
        has_text(name) ? name : "(unknown)",
        sync_slot_str(item->slot),
        timestamp,
        (long long)item->timestamp_unix);
  }
  if (detected_item_count > preview_count) {
    ui_sync_log_write(APP_LOG_LEVEL_INFO, "... %d more local save(s) omitted", detected_item_count - preview_count);
  }

  work_item_count = ui_prepare_ps1_sync_candidates(
      work_items,
      detected_item_count,
      state->selected_save_platform,
      &selection_warning_count);
  if (work_item_count <= 0) {
    ui_sync_log_write(APP_LOG_LEVEL_ERROR, "Sync failed: no sync candidate was selected");
    ui_sync_feedback_set_message(&state->sync_feedback, "Sync failed: no sync candidate selected");
    state->sync_feedback.running = 0;
    state->sync_feedback.completed = 1;
    state->sync_feedback.success = 0;
    state->sync_feedback.sync_status = SYNC_ENGINE_ERR_INVALID_ARGUMENT;
    ui_sync_feedback_set_progress(&state->sync_feedback, 1, 1);
    ui_sync_render_live(state);
    return SYNC_ENGINE_ERR_INVALID_ARGUMENT;
  }

  int engine_units = work_item_count + 1;
  if (engine_units < 1) {
    engine_units = 1;
  }
  int total_units = 3 + engine_units;
  ui_sync_feedback_set_progress(&state->sync_feedback, 0, total_units);
  ui_sync_render_live(state);

  ui_sync_feedback_set_message(&state->sync_feedback, "Validating RomM configuration...");
  ui_sync_render_live(state);
  if (!app_config_has_server_url(&state->config)) {
    ui_sync_log_write(APP_LOG_LEVEL_ERROR, "Sync failed: RomM server URL is missing");
    ui_sync_feedback_set_message(&state->sync_feedback, "Sync failed: RomM URL is missing");
    state->sync_feedback.running = 0;
    state->sync_feedback.completed = 1;
    state->sync_feedback.success = 0;
    state->sync_feedback.sync_status = SYNC_ENGINE_ERR_INVALID_ARGUMENT;
    ui_sync_feedback_set_progress(&state->sync_feedback, total_units, total_units);
    ui_sync_render_live(state);
    return SYNC_ENGINE_ERR_INVALID_ARGUMENT;
  }
  if (!app_config_has_auth(&state->config)) {
    ui_sync_log_write(APP_LOG_LEVEL_ERROR, "Sync failed: RomM auth is missing (API token or username/password)");
    ui_sync_feedback_set_message(&state->sync_feedback, "Sync failed: auth is missing");
    state->sync_feedback.running = 0;
    state->sync_feedback.completed = 1;
    state->sync_feedback.success = 0;
    state->sync_feedback.sync_status = SYNC_ENGINE_ERR_INVALID_ARGUMENT;
    ui_sync_feedback_set_progress(&state->sync_feedback, total_units, total_units);
    ui_sync_render_live(state);
    return SYNC_ENGINE_ERR_INVALID_ARGUMENT;
  }
  ui_sync_feedback_set_progress(&state->sync_feedback, 1, total_units);
  ui_sync_log_write(APP_LOG_LEVEL_INFO, "RomM auth present in configuration");
  ui_sync_render_live(state);

  ui_sync_feedback_set_message(&state->sync_feedback, "Ensuring device registration...");
  ui_sync_render_live(state);
  int wrote_config = ensure_device_registration(&state->config, &state->romm_client);
  if (wrote_config) {
    ui_sync_log_write(APP_LOG_LEVEL_INFO, "Device registered: %s", state->config.device_id);
  } else if (has_text(state->config.device_id)) {
    ui_sync_log_write(APP_LOG_LEVEL_INFO, "Using existing device ID: %s", state->config.device_id);
  } else {
    ui_sync_log_write(APP_LOG_LEVEL_WARN, "No device_id available; continuing anyway");
  }
  ui_sync_feedback_set_progress(&state->sync_feedback, 2, total_units);
  ui_sync_render_live(state);

  ui_sync_feedback_set_message(&state->sync_feedback, "Resolving RomM game mapping...");
  ui_sync_render_live(state);
  int mapped_count = romm_http_resolve_rom_ids(&state->config, work_items, work_item_count);
  if (mapped_count < 0) {
    ui_sync_log_write(APP_LOG_LEVEL_ERROR, "Sync failed: rom mapping error (%s)", romm_client_status_str(mapped_count));
    ui_sync_feedback_set_message(&state->sync_feedback, "Sync failed: Rom mapping error");
    state->sync_feedback.running = 0;
    state->sync_feedback.completed = 1;
    state->sync_feedback.success = 0;
    state->sync_feedback.sync_status = mapped_count;
    ui_sync_feedback_set_progress(&state->sync_feedback, total_units, total_units);
    ui_sync_render_live(state);
    return mapped_count;
  }
  ui_sync_log_write(APP_LOG_LEVEL_INFO, "Mapped %d/%d save(s)", mapped_count, work_item_count);
  int unresolved_count = 0;
  for (int i = 0; i < work_item_count; ++i) {
    const SyncSaveDescriptor *item = &work_items[i];
    if (item->rom_id > 0) {
      continue;
    }

    unresolved_count++;
    ui_sync_log_write(
        APP_LOG_LEVEL_WARN,
        "rom_id unresolved: game=%s title=%s file=%s",
        has_text(item->game_id) ? item->game_id : "(unknown)",
        has_text(item->title) ? item->title : "(unknown)",
        has_text(item->filename) ? item->filename : "(unknown)");
  }
  if (unresolved_count > 0) {
    if (state->selected_save_platform == SYNC_SAVE_PLATFORM_VITA_NATIVE_EXPERIMENTAL) {
      int write_index = 0;
      for (int i = 0; i < work_item_count; ++i) {
        if (work_items[i].rom_id > 0) {
          if (write_index != i) {
            memmove(&work_items[write_index], &work_items[i], sizeof(work_items[write_index]));
          }
          write_index += 1;
          continue;
        }
        ui_sync_log_write(
            APP_LOG_LEVEL_WARN,
            "mapping failed; skipping Vita container: game=%s title=%s file=%s",
            has_text(work_items[i].game_id) ? work_items[i].game_id : "(unknown)",
            has_text(work_items[i].title) ? work_items[i].title : "(unknown)",
            has_text(work_items[i].filename) ? work_items[i].filename : "(unknown)");
      }
      work_item_count = write_index;
      if (work_item_count <= 0) {
        ui_sync_log_write(APP_LOG_LEVEL_WARN, "Sync skipped: no Vita native save container mapped to RomM");
        ui_sync_feedback_set_message(&state->sync_feedback, "Sync skipped: no mapped Vita saves");
        state->sync_feedback.running = 0;
        state->sync_feedback.completed = 1;
        state->sync_feedback.success = 1;
        state->sync_feedback.sync_status = SYNC_ENGINE_OK;
        ui_sync_feedback_set_progress(&state->sync_feedback, total_units, total_units);
        ui_sync_render_live(state);
        return SYNC_ENGINE_OK;
      }
      ui_sync_log_write(APP_LOG_LEVEL_INFO, "Continuing with %d mapped Vita save container(s)", work_item_count);
    } else {
      ui_sync_log_write(APP_LOG_LEVEL_ERROR, "Sync aborted: %d save(s) have no rom_id after mapping", unresolved_count);
      ui_sync_feedback_set_message(&state->sync_feedback, "Sync failed: unresolved RomM mapping");
      state->sync_feedback.running = 0;
      state->sync_feedback.completed = 1;
      state->sync_feedback.success = 0;
      state->sync_feedback.sync_status = SYNC_ENGINE_ERR_UNRESOLVED_ROM_ID;
      ui_sync_feedback_set_progress(&state->sync_feedback, total_units, total_units);
      ui_sync_render_live(state);
      return SYNC_ENGINE_ERR_UNRESOLVED_ROM_ID;
    }
  }
  ui_sync_feedback_set_progress(&state->sync_feedback, 3, total_units);
  ui_sync_render_live(state);

  if (state->selected_save_platform == SYNC_SAVE_PLATFORM_VITA_NATIVE_EXPERIMENTAL) {
    ui_sync_feedback_set_message(&state->sync_feedback, "Preparing Vita archive export...");
    ui_sync_log_write(APP_LOG_LEVEL_INFO, "Vita native archive sync enabled: upload and restore use raw PFS tar backups");
    ui_sync_log_write(APP_LOG_LEVEL_INFO, "%s", vita_native_save_vita3k_import_notice());
    ui_sync_log_write(APP_LOG_LEVEL_INFO, "Preparing archive-only export for %d Vita native save container(s)", work_item_count);
    int archive_status = vita_native_prepare_export_archives(
        work_items,
        work_item_count,
        VITA_NATIVE_EXPORT_CACHE_DIR,
        state->config.log_scan_verbose);
    if (archive_status < 0) {
      ui_sync_log_write(APP_LOG_LEVEL_ERROR, "Sync failed: Vita archive export preparation failed (%d)", archive_status);
      ui_sync_feedback_set_message(&state->sync_feedback, "Sync failed: Vita archive export failed");
      state->sync_feedback.running = 0;
      state->sync_feedback.completed = 1;
      state->sync_feedback.success = 0;
      state->sync_feedback.sync_status = archive_status;
      ui_sync_feedback_set_progress(&state->sync_feedback, total_units, total_units);
      ui_sync_render_live(state);
      return archive_status;
    }
    for (int i = 0; i < work_item_count; ++i) {
      ui_sync_log_write(
          APP_LOG_LEVEL_INFO,
          "Vita archive export ready: title_id=%s archive=%s size=%llu platform=%s",
          has_text(work_items[i].game_id) ? work_items[i].game_id : "(unknown)",
          has_text(work_items[i].path) ? work_items[i].path : "(unknown)",
          (unsigned long long)work_items[i].size_bytes,
          sync_save_platform_id(work_items[i].platform));
    }
    ui_sync_render_live(state);
  }

  state->config.sync_auto_apply_conflicts = 1;
  ui_sync_log_write(
      APP_LOG_LEVEL_INFO,
      "Sync options: dry_run=%d auto_apply_conflicts=%d trigger=%s",
      state->config.sync_dry_run,
      state->config.sync_auto_apply_conflicts,
      (trigger == UI_SYNC_TRIGGER_AUTOMATIC) ? "automatic" : "manual");

  if (state->config.sync_dry_run) {
    ui_sync_log_write(APP_LOG_LEVEL_INFO, "Dry-run enabled: transfers will not execute");
  }
  ui_sync_log_write(APP_LOG_LEVEL_INFO, "Auto-apply conflicts enabled: recommended actions will execute without confirmation");

  AppConfig sync_http_config;
  RommClient sync_client;
  const RommClient *active_client = &state->romm_client;
  if (state->selected_save_platform == SYNC_SAVE_PLATFORM_VITA_NATIVE_EXPERIMENTAL) {
    memcpy(&sync_http_config, &state->config, sizeof(sync_http_config));
    snprintf(
        sync_http_config.romm_save_emulator,
        sizeof(sync_http_config.romm_save_emulator),
        "%s",
        romm_http_default_save_emulator_for_platform(SYNC_SAVE_PLATFORM_VITA_NATIVE_EXPERIMENTAL));
    memcpy(&sync_client, &state->romm_client, sizeof(sync_client));
    sync_client.context = &sync_http_config;
    active_client = &sync_client;
    ui_sync_log_write(APP_LOG_LEVEL_INFO, "Using RomM save backend for Vita native archives: %s", sync_http_config.romm_save_emulator);
  }

  SyncEngineConfig config;
  sync_engine_config_init(&config);
  config.device_id = has_text(state->config.device_id) ? state->config.device_id : NULL;
  config.state_store_path = has_text(state->config.sync_state_store_path) ? state->config.sync_state_store_path : NULL;
  config.backup_directory = has_text(state->config.sync_backup_directory) ? state->config.sync_backup_directory : NULL;
  config.dry_run = state->config.sync_dry_run;

  UiSyncProgressBridge progress_bridge;
  memset(&progress_bridge, 0, sizeof(progress_bridge));
  progress_bridge.state = state;
  progress_bridge.base_completed_units = 3;
  progress_bridge.overall_total_units = total_units;

  UiSyncConflictResolutionContext conflict_context;
  memset(&conflict_context, 0, sizeof(conflict_context));
  conflict_context.state = state;
  conflict_context.trigger = trigger;
  conflict_context.dry_run = state->config.sync_dry_run;
  conflict_context.auto_apply_conflicts = 1;

  config.resolve_conflict = ui_sync_resolve_conflict_callback;
  config.resolve_conflict_user_data = &conflict_context;
  config.progress_callback = ui_sync_engine_progress_callback;
  config.progress_user_data = &progress_bridge;

  int sync_status = sync_engine_run(
      &config, work_items, work_item_count, active_client, &state->sync_report);

  int restore_blocked_skip = ui_sync_report_has_restore_blocked_skip(&state->sync_report);
  int has_transfer_errors = state->sync_report.transfer_errors > 0;
  state->sync_feedback.running = 0;
  state->sync_feedback.completed = 1;
  state->sync_feedback.sync_status = sync_status;
  state->sync_feedback.warning = restore_blocked_skip || has_transfer_errors;
  state->sync_feedback.success = (sync_status == SYNC_ENGINE_OK) && !restore_blocked_skip && !has_transfer_errors;
  ui_sync_feedback_set_progress(&state->sync_feedback, total_units, total_units);

  if (sync_status == SYNC_ENGINE_OK) {
    ui_sync_log_write(APP_LOG_LEVEL_INFO, "Sync completed");
    ui_sync_append_report_logs(&state->sync_report);
    if (has_transfer_errors) {
      ui_sync_feedback_set_message(&state->sync_feedback, "Sync completed with errors.");
    } else if (restore_blocked_skip) {
      ui_sync_feedback_set_message(&state->sync_feedback, "Restore skipped for unsupported platform.");
    } else if ((selection_warning_count > 0) ||
               (state->sync_report.skipped > 0) ||
               (state->sync_report.conflicts_detected > 0)) {
      ui_sync_feedback_set_message(&state->sync_feedback, "Sync completed with warnings.");
    } else {
      ui_sync_feedback_set_message(&state->sync_feedback, "Sync completed successfully.");
    }
  } else {
    char failure_message[96];
    snprintf(failure_message, sizeof(failure_message), "Sync failed: %s.", sync_engine_status_str(sync_status));
    ui_sync_log_write(APP_LOG_LEVEL_ERROR, "Sync failed: %s (%d)", sync_engine_status_str(sync_status), sync_status);
    ui_sync_feedback_set_message(&state->sync_feedback, failure_message);
  }

  ui_sync_render_live(state);
  return sync_status;
}

/*
 * Writes a compact controller-screen status after a sync run. Dry-run reports
 * use planned transfers, while live runs use executed transfers so the footer
 * tells users what happened without opening the log modal again.
 */
static void ui_set_sync_result_status(UiAppState *state, const char *scope, int sync_status) {
  if (state == NULL) {
    return;
  }

  const char *scope_label = has_text(scope) ? scope : "Sync";
  if (sync_status != SYNC_ENGINE_OK) {
    ui_set_status(state, "%s failed: %s", scope_label, sync_engine_status_str(sync_status));
    return;
  }

  if (state->config.sync_dry_run) {
    ui_set_status(
        state,
        "%s preview: uploads=%d downloads=%d skipped=%d conflicts=%d errors=%d",
        scope_label,
        state->sync_report.uploads_planned,
        state->sync_report.downloads_planned,
        state->sync_report.skipped,
        state->sync_report.conflicts_detected,
        state->sync_report.transfer_errors);
    return;
  }

  if (ui_sync_report_has_restore_blocked_skip(&state->sync_report)) {
    ui_set_status(state, "%s skipped: restore unsupported", scope_label);
    return;
  }

  ui_set_status(
      state,
      "%s complete: uploads=%d downloads=%d skipped=%d conflicts=%d errors=%d",
      scope_label,
      state->sync_report.uploads_executed,
      state->sync_report.downloads_executed,
      state->sync_report.skipped,
      state->sync_report.conflicts_detected,
      state->sync_report.transfer_errors);
}

void ui_run_sync_for_selected_games(UiAppState *state) {
  if (state == NULL) {
    return;
  }

  int selected_game_count = 0;
  int selected_target_count = 0;
  int work_item_count = ui_collect_selected_game_items(
      state, state->sync_work_items,
      (int)(sizeof(state->sync_work_items) / sizeof(state->sync_work_items[0])),
      &selected_game_count, &selected_target_count);
  int sync_candidate_count = ui_estimate_ps1_sync_candidate_count(state->sync_work_items, work_item_count);

  const char *mode_line = state->config.sync_dry_run
                              ? "Dry-run preview: no files will be written."
                              : "Live sync may upload/download saves. Backups and conflict rules apply.";
  char confirm_msg[320];
  snprintf(confirm_msg, sizeof(confirm_msg),
      "%s\n\nSynchronize %d selected game(s)?\n%d sync candidate(s) selected from %d local target(s).",
      mode_line, selected_game_count, sync_candidate_count, selected_target_count);
  if (ui_dialog_confirm(confirm_msg) != 1) {
    ui_set_status(state, "Sync canceled for selected games");
    return;
  }

  if (work_item_count <= 0) {
    ui_set_status(state, "No local save found for the checked games");
    return;
  }

  char ctx[UI_STATUS_LINE_LEN];
  snprintf(ctx, sizeof(ctx), "Selected games: %d (%d sync candidate%s)",
      selected_game_count, sync_candidate_count, (sync_candidate_count == 1) ? "" : "s");

  int sync_status = ui_run_sync_pipeline(state, state->sync_work_items, work_item_count,
      UI_SYNC_TRIGGER_MANUAL, "Manual Synchronization", ctx);

  ui_set_sync_result_status(state, "Selected sync", sync_status);

  ui_present_completed_manual_sync(state);
}

void ui_run_sync_all_saves(UiAppState *state) {
  if (state == NULL) {
    return;
  }

  if (state->local_count <= 0) {
    ui_set_status(state, "No local %s saves were detected", sync_save_platform_short_label(state->selected_save_platform));
    return;
  }

  int work_item_count = state->local_count;
  if (work_item_count > (int)(sizeof(state->sync_work_items) / sizeof(state->sync_work_items[0]))) {
    work_item_count = (int)(sizeof(state->sync_work_items) / sizeof(state->sync_work_items[0]));
  }
  memcpy(state->sync_work_items, state->local_items, sizeof(state->sync_work_items[0]) * (size_t)work_item_count);
  int sync_candidate_count = ui_estimate_ps1_sync_candidate_count(state->sync_work_items, work_item_count);

  const char *mode_line = state->config.sync_dry_run
                              ? "Dry-run preview: no files will be written."
                              : "Live sync may upload/download saves. Backups and conflict rules apply.";
  char confirm_msg[320];
  snprintf(confirm_msg, sizeof(confirm_msg),
      "%s\n\nSynchronize all detected %s saves?\n%d sync candidate(s) selected from %d local target(s) across %d game(s).",
      mode_line,
      sync_save_platform_short_label(state->selected_save_platform),
      sync_candidate_count,
      work_item_count,
      state->game_count);
  if (ui_dialog_confirm(confirm_msg) != 1) {
    ui_set_status(state, "Sync canceled for all games");
    return;
  }

  char ctx[UI_STATUS_LINE_LEN];
  snprintf(ctx, sizeof(ctx), "All games: %d sync candidate%s across %d game(s)",
      sync_candidate_count, (sync_candidate_count == 1) ? "" : "s", state->game_count);

  int sync_status = ui_run_sync_pipeline(state, state->sync_work_items, work_item_count,
      UI_SYNC_TRIGGER_MANUAL, "Manual Synchronization", ctx);

  ui_set_sync_result_status(state, "All sync", sync_status);

  ui_present_completed_manual_sync(state);
}

void ui_run_pending_auto_sync(UiAppState *state) {
  if ((state == NULL) || !state->pending_auto_sync) {
    return;
  }

  state->pending_auto_sync = 0;

  if (state->local_count <= 0) {
    ui_set_status(state, "Auto sync skipped: no local %s saves detected", sync_save_platform_short_label(state->selected_save_platform));
    return;
  }

  if (!app_config_has_server_url(&state->config) || !app_config_has_auth(&state->config)) {
    ui_set_status(state, "Auto sync skipped: configure the RomM URL and API token (or username/password) first");
    return;
  }

  int work_item_count = state->local_count;
  if (work_item_count > (int)(sizeof(state->sync_work_items) / sizeof(state->sync_work_items[0]))) {
    work_item_count = (int)(sizeof(state->sync_work_items) / sizeof(state->sync_work_items[0]));
  }
  memcpy(state->sync_work_items, state->local_items, sizeof(state->sync_work_items[0]) * (size_t)work_item_count);
  int sync_candidate_count = ui_estimate_ps1_sync_candidate_count(state->sync_work_items, work_item_count);

  char ctx[UI_STATUS_LINE_LEN];
  snprintf(ctx, sizeof(ctx), "Startup auto sync: %d sync candidate%s",
      sync_candidate_count, (sync_candidate_count == 1) ? "" : "s");

  int sync_status = ui_run_sync_pipeline(state, state->sync_work_items, work_item_count,
      UI_SYNC_TRIGGER_AUTOMATIC, "Automatic Synchronization", ctx);

  ui_set_sync_result_status(state, "Auto sync", sync_status);

  ui_refresh_local_inventory(state);
  ui_clamp_selection(state);
}
