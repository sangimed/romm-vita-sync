#include <psp2/ctrl.h>
#include <psp2/kernel/threadmgr.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "debugScreen.h"

#include "app_config.h"
#include "app_log.h"
#include "backup_manager.h"
#include "ps1_paths.h"
#include "romm_client.h"
#include "romm_http_client.h"
#include "scan_to_sync_adapter.h"
#include "save_scanner.h"
#include "sync_engine.h"

#define printf psvDebugScreenPrintf

#define UI_SELECT_SERVER_URL 0
#define UI_SELECT_USERNAME 1
#define UI_SELECT_PASSWORD 2
#define UI_SELECT_SAVE_SETTINGS 3
#define UI_SELECT_RESCAN 4
#define UI_SELECT_GAME_BASE 5

#define UI_GAME_LIST_VISIBLE 14
#define UI_LOG_VISIBLE_LINES 18
#define UI_STATUS_LINE_LEN 192
#define UI_EDITOR_BUFFER_LEN 512
#define APP_RUNTIME_DATA_DIRECTORY "ux0:data/romm-vita-sync"

typedef struct UiGameEntry {
  char key[ROMM_GAME_ID_LEN];
  char game_id[ROMM_GAME_ID_LEN];
  char title[ROMM_GAME_TITLE_LEN];
  int save_count;
} UiGameEntry;

typedef struct UiAppState {
  AppConfig config;
  int config_status;
  RommClient romm_client;

  ScanResult scan_result;
  SyncSaveDescriptor local_items[ROMM_SYNC_MAX_ITEMS];
  SyncSaveDescriptor sync_work_items[ROMM_SYNC_MAX_ITEMS];
  SyncRunReport sync_report;
  int local_count;

  UiGameEntry games[ROMM_SYNC_MAX_ITEMS];
  int game_count;

  int selected_index;
  int game_scroll;
  char status_line[UI_STATUS_LINE_LEN];
} UiAppState;

static UiAppState g_app_state;

/*
 * Returns non-zero when a string is non-null and non-empty.
 */
static int has_text(const char *value) {
  return (value != NULL) && (value[0] != '\0');
}

/*
 * Maps AppConfig numeric log level to AppLogLevel enum safely.
 */
static AppLogLevel app_log_level_from_config(int config_level) {
  if (config_level <= APP_CONFIG_LOG_LEVEL_ERROR) {
    return APP_LOG_LEVEL_ERROR;
  }
  if (config_level == APP_CONFIG_LOG_LEVEL_WARN) {
    return APP_LOG_LEVEL_WARN;
  }
  if (config_level >= APP_CONFIG_LOG_LEVEL_DEBUG) {
    return APP_LOG_LEVEL_DEBUG;
  }
  return APP_LOG_LEVEL_INFO;
}

/*
 * Writes one short status message shown near the top of the main screen.
 */
static void ui_set_status(UiAppState *state, const char *format, ...) {
  if ((state == NULL) || (format == NULL)) {
    return;
  }

  va_list args;
  va_start(args, format);
  vsnprintf(state->status_line, sizeof(state->status_line), format, args);
  va_end(args);
}

/*
 * Polls controller and returns buttons that transitioned to pressed state.
 */
static unsigned int ui_poll_pressed(unsigned int *io_previous_buttons) {
  if (io_previous_buttons == NULL) {
    return 0U;
  }

  SceCtrlData pad;
  memset(&pad, 0, sizeof(pad));
  sceCtrlPeekBufferPositive(0, &pad, 1);

  unsigned int pressed = pad.buttons & (~(*io_previous_buttons));
  *io_previous_buttons = pad.buttons;
  return pressed;
}

/*
 * Produces a single-line display string and truncates with "..." when needed.
 */
static void ui_truncate_text(const char *source, char *out_text, size_t out_size) {
  if ((out_text == NULL) || (out_size == 0U)) {
    return;
  }

  if (!has_text(source)) {
    snprintf(out_text, out_size, "(empty)");
    return;
  }

  size_t source_len = strlen(source);
  if (source_len < out_size) {
    snprintf(out_text, out_size, "%s", source);
    return;
  }

  if (out_size <= 4U) {
    out_text[0] = '\0';
    return;
  }

  size_t keep = out_size - 4U;
  memcpy(out_text, source, keep);
  out_text[keep] = '.';
  out_text[keep + 1U] = '.';
  out_text[keep + 2U] = '.';
  out_text[keep + 3U] = '\0';
}

/*
 * Renders password value as asterisks for screen-safe display.
 */
static void ui_mask_secret(const char *secret, char *out_masked, size_t out_size) {
  if ((out_masked == NULL) || (out_size == 0U)) {
    return;
  }

  if (!has_text(secret)) {
    snprintf(out_masked, out_size, "(empty)");
    return;
  }

  size_t length = strlen(secret);
  if (length >= out_size) {
    length = out_size - 1U;
  }

  memset(out_masked, '*', length);
  out_masked[length] = '\0';
}

/*
 * Builds one deterministic grouping key for a local save descriptor.
 */
static void ui_build_game_key(const SyncSaveDescriptor *item, char *out_key, size_t out_key_size) {
  if ((out_key == NULL) || (out_key_size == 0U)) {
    return;
  }

  out_key[0] = '\0';
  if (item == NULL) {
    return;
  }

  if (has_text(item->game_id)) {
    snprintf(out_key, out_key_size, "%s", item->game_id);
    return;
  }

  if (has_text(item->title)) {
    snprintf(out_key, out_key_size, "%s", item->title);
    return;
  }

  if (has_text(item->filename)) {
    snprintf(out_key, out_key_size, "%s", item->filename);
    return;
  }

  if (has_text(item->path)) {
    snprintf(out_key, out_key_size, "%s", item->path);
    return;
  }

  snprintf(out_key, out_key_size, "unknown");
}

/*
 * Finds the existing game entry index for a key, or -1 when missing.
 */
static int ui_find_game_entry(const UiGameEntry *games, int game_count, const char *key) {
  if ((games == NULL) || (game_count <= 0) || !has_text(key)) {
    return -1;
  }

  for (int i = 0; i < game_count; ++i) {
    if (sync_string_ieq(games[i].key, key)) {
      return i;
    }
  }

  return -1;
}

/*
 * Compares two strings in case-insensitive ASCII order.
 */
static int ui_ascii_casecmp(const char *lhs, const char *rhs) {
  if (lhs == rhs) {
    return 0;
  }
  if (lhs == NULL) {
    return -1;
  }
  if (rhs == NULL) {
    return 1;
  }

  while ((*lhs != '\0') && (*rhs != '\0')) {
    char l = (char)tolower((unsigned char)*lhs);
    char r = (char)tolower((unsigned char)*rhs);
    if (l != r) {
      return (l < r) ? -1 : 1;
    }
    lhs++;
    rhs++;
  }

  if (*lhs == '\0' && *rhs == '\0') {
    return 0;
  }

  return (*lhs == '\0') ? -1 : 1;
}

/*
 * Sorts game entries so the list stays predictable between scans.
 */
static void ui_sort_game_entries(UiGameEntry *games, int game_count) {
  if ((games == NULL) || (game_count <= 1)) {
    return;
  }

  for (int i = 1; i < game_count; ++i) {
    UiGameEntry key = games[i];
    int j = i - 1;

    while (j >= 0) {
      const char *left_title = has_text(games[j].title) ? games[j].title : games[j].game_id;
      const char *right_title = has_text(key.title) ? key.title : key.game_id;
      int cmp = ui_ascii_casecmp(left_title, right_title);
      if (cmp <= 0) {
        break;
      }

      games[j + 1] = games[j];
      j--;
    }

    games[j + 1] = key;
  }
}

/*
 * Builds unique PS1 game entries from the current local save descriptor list.
 */
static int ui_build_game_entries(
    const SyncSaveDescriptor *items,
    int item_count,
    UiGameEntry *out_games,
    int max_games) {
  if ((items == NULL) || (out_games == NULL) || (item_count < 0) || (max_games <= 0)) {
    return 0;
  }

  int game_count = 0;
  for (int i = 0; i < item_count; ++i) {
    const SyncSaveDescriptor *item = &items[i];
    char key[ROMM_GAME_ID_LEN];
    ui_build_game_key(item, key, sizeof(key));
    if (!has_text(key)) {
      continue;
    }

    int existing = ui_find_game_entry(out_games, game_count, key);
    if (existing >= 0) {
      out_games[existing].save_count += 1;
      if (!has_text(out_games[existing].title) && has_text(item->title)) {
        snprintf(out_games[existing].title, sizeof(out_games[existing].title), "%s", item->title);
      }
      continue;
    }

    if (game_count >= max_games) {
      break;
    }

    UiGameEntry *entry = &out_games[game_count];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->key, sizeof(entry->key), "%s", key);
    snprintf(entry->game_id, sizeof(entry->game_id), "%s", has_text(item->game_id) ? item->game_id : "(unknown)");
    snprintf(entry->title, sizeof(entry->title), "%s", item->title);
    entry->save_count = 1;
    game_count += 1;
  }

  ui_sort_game_entries(out_games, game_count);
  return game_count;
}

/*
 * Returns total number of selectable entries currently rendered in main UI.
 */
static int ui_total_selectable_entries(const UiAppState *state) {
  if (state == NULL) {
    return UI_SELECT_GAME_BASE;
  }
  return UI_SELECT_GAME_BASE + state->game_count;
}

/*
 * Keeps selected index inside the current selectable range.
 */
static void ui_clamp_selection(UiAppState *state) {
  if (state == NULL) {
    return;
  }

  int total = ui_total_selectable_entries(state);
  if (total <= 0) {
    state->selected_index = 0;
    return;
  }

  if (state->selected_index < 0) {
    state->selected_index = 0;
  }
  if (state->selected_index >= total) {
    state->selected_index = total - 1;
  }
}

/*
 * Updates game-list scroll offset so the selected game stays in view.
 */
static void ui_update_game_scroll(UiAppState *state) {
  if (state == NULL) {
    return;
  }

  int selected_game = state->selected_index - UI_SELECT_GAME_BASE;
  if (selected_game < 0) {
    return;
  }

  if (selected_game < state->game_scroll) {
    state->game_scroll = selected_game;
  } else if (selected_game >= (state->game_scroll + UI_GAME_LIST_VISIBLE)) {
    state->game_scroll = selected_game - UI_GAME_LIST_VISIBLE + 1;
  }

  if (state->game_scroll < 0) {
    state->game_scroll = 0;
  }
}

/*
 * Saves current configuration to settings.ini and updates UI status text.
 */
static int ui_save_config(UiAppState *state, const char *success_message) {
  if (state == NULL) {
    return APP_CONFIG_ERR_INVALID_ARGUMENT;
  }

  int save_status = app_config_save(APP_CONFIG_DEFAULT_PATH, &state->config);
  if (save_status != APP_CONFIG_OK) {
    ui_set_status(
        state,
        "Settings save failed: %s (%d)",
        app_config_status_str(save_status),
        save_status);
    app_log_write(
        APP_LOG_LEVEL_ERROR,
        "ui",
        "settings save failed: %s (%d)",
        app_config_status_str(save_status),
        save_status);
    return save_status;
  }

  if (has_text(success_message)) {
    ui_set_status(state, "%s", success_message);
  } else {
    ui_set_status(state, "Settings saved to %s", APP_CONFIG_DEFAULT_PATH);
  }
  app_log_set_level(app_log_level_from_config(state->config.log_level));
  app_log_write(APP_LOG_LEVEL_INFO, "ui", "settings saved to %s", APP_CONFIG_DEFAULT_PATH);
  return APP_CONFIG_OK;
}

/*
 * Registers device_id when missing, then persists it back to settings.ini.
 * Returns non-zero when settings.ini has been newly written.
 */
static int ensure_device_registration(AppConfig *config, const RommClient *romm_client) {
  if ((config == NULL) || (romm_client == NULL)) {
    return 0;
  }

  if (has_text(config->device_id)) {
    return 0;
  }

  if (!app_config_has_server_url(config) || !app_config_has_auth(config)) {
    app_log_write(APP_LOG_LEVEL_WARN, "main", "device registration skipped (RomM url/auth missing)");
    return 0;
  }
  app_log_write(APP_LOG_LEVEL_INFO, "main", "registering device on RomM via /api/devices");

  char registered_device_id[ROMM_SYNC_MAX_DEVICE_ID_LEN];
  int status = romm_client_register_device(
      romm_client,
      config->device_name,
      config->device_platform,
      config->device_client,
      config->device_client_version,
      registered_device_id,
      sizeof(registered_device_id));

  if (status != ROMM_CLIENT_OK) {
    app_log_write(
        APP_LOG_LEVEL_ERROR,
        "main",
        "device registration failed: %s (%d)",
        romm_client_status_str(status),
        status);
    return 0;
  }

  if (app_config_set_device_id(config, registered_device_id) != APP_CONFIG_OK) {
    app_log_write(APP_LOG_LEVEL_ERROR, "main", "device registration failed: invalid device id returned");
    return 0;
  }

  int save_status = app_config_save(APP_CONFIG_DEFAULT_PATH, config);
  if (save_status != APP_CONFIG_OK) {
    app_log_write(
        APP_LOG_LEVEL_ERROR,
        "main",
        "device id acquired but settings save failed: %s (%d)",
        app_config_status_str(save_status),
        save_status);
    return 0;
  }

  app_log_write(APP_LOG_LEVEL_INFO, "main", "device registered and persisted: %s", config->device_id);
  return 1;
}

/*
 * Ensures the app runtime directory exists on first launch.
 * This allows config/state persistence without requiring manual file setup.
 */
static int ensure_runtime_data_directory(void) {
  int status = backup_manager_ensure_directory(APP_RUNTIME_DATA_DIRECTORY);
  if (status == BACKUP_MANAGER_OK) {
    return BACKUP_MANAGER_OK;
  }

  app_log_write(
      APP_LOG_LEVEL_WARN,
      "main",
      "runtime directory creation failed: %s (%d)",
      backup_manager_status_str(status),
      status);
  return status;
}

/*
 * Scans local Vita storage for PS1 saves and rebuilds the UI game list.
 */
static int ui_refresh_local_inventory(UiAppState *state) {
  if (state == NULL) {
    return -1;
  }

  psvDebugScreenClear(0x10141F);
  printf("RomM Vita Sync\n\n");
  printf("Scanning local PS1 saves...\n");
  printf("Path: ux0:pspemu/PSP/SAVEDATA\n");
  printf("Please wait.\n");

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
    ui_set_status(state, "Failed to build sync inventory from scan result");
    app_log_write(APP_LOG_LEVEL_ERROR, "ui", "scan_result_to_sync_saves failed");
    return -1;
  }

  state->game_count = ui_build_game_entries(
      state->local_items,
      state->local_count,
      state->games,
      (int)(sizeof(state->games) / sizeof(state->games[0])));

  state->game_scroll = 0;
  ui_set_status(
      state,
      "Scan complete: %d PS1 games (%d memory card files)",
      state->game_count,
      state->local_count);
  app_log_write(
      APP_LOG_LEVEL_INFO,
      "ui",
      "scan complete games=%d saves=%d access_errors=%d",
      state->game_count,
      state->local_count,
      state->scan_result.stats.access_errors);
  return 0;
}

/*
 * Returns non-zero when one local save descriptor belongs to a game key.
 */
static int ui_item_matches_game_key(const SyncSaveDescriptor *item, const char *key) {
  if ((item == NULL) || !has_text(key)) {
    return 0;
  }

  char item_key[ROMM_GAME_ID_LEN];
  ui_build_game_key(item, item_key, sizeof(item_key));
  return sync_string_ieq(item_key, key);
}

/*
 * Collects local descriptors for a single selected game.
 */
static int ui_collect_game_items(
    const UiAppState *state,
    const UiGameEntry *game,
    SyncSaveDescriptor *out_items,
    int max_items) {
  if ((state == NULL) || (game == NULL) || (out_items == NULL) || (max_items <= 0)) {
    return 0;
  }

  int count = 0;
  for (int i = 0; i < state->local_count; ++i) {
    if (!ui_item_matches_game_key(&state->local_items[i], game->key)) {
      continue;
    }

    if (count >= max_items) {
      break;
    }

    memcpy(&out_items[count], &state->local_items[i], sizeof(out_items[count]));
    count++;
  }

  return count;
}

/*
 * Waits until user presses Circle, Cross, or Start.
 */
static void ui_wait_for_return_button(void) {
  unsigned int previous_buttons = 0U;
  for (;;) {
    unsigned int pressed = ui_poll_pressed(&previous_buttons);
    if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_CROSS | SCE_CTRL_START)) {
      return;
    }
    sceKernelDelayThread(16 * 1000);
  }
}

/*
 * Renders synchronization report summary and action outcomes.
 */
static void ui_render_sync_report(const SyncRunReport *report) {
  if (report == NULL) {
    printf("No sync report available.\n");
    return;
  }

  printf("\nSync summary\n");
  printf("  local saves        : %d\n", report->local_count);
  printf("  remote saves       : %d\n", report->remote_count);
  printf("  uploads planned    : %d\n", report->uploads_planned);
  printf("  downloads planned  : %d\n", report->downloads_planned);
  printf("  uploads executed   : %d\n", report->uploads_executed);
  printf("  downloads executed : %d\n", report->downloads_executed);
  printf("  conflicts          : %d\n", report->conflicts_detected);
  printf("  skipped            : %d\n", report->skipped);
  printf("  errors             : %d\n", report->transfer_errors);

  if (report->action_count <= 0) {
    return;
  }

  printf("\nAction log\n");
  int render_count = report->action_count;
  if (render_count > 24) {
    render_count = 24;
  }

  for (int i = 0; i < render_count; ++i) {
    const SyncActionRecord *action = &report->actions[i];
    printf(
        "[%02d] %s %s %s status=%d reason=%s\n",
        i + 1,
        sync_slot_str(action->slot),
        sync_action_type_str(action->action),
        action->executed ? "executed" : "planned",
        action->status_code,
        action->reason);
  }

  if (report->action_count > render_count) {
    printf("... %d more action(s) not shown\n", report->action_count - render_count);
  }
}

/*
 * Runs synchronization only for one selected game and prints a live step log.
 */
static void ui_run_sync_for_game(UiAppState *state, int game_index) {
  if ((state == NULL) || (game_index < 0) || (game_index >= state->game_count)) {
    return;
  }

  const UiGameEntry *game = &state->games[game_index];
  int game_item_count = ui_collect_game_items(
      state,
      game,
      state->sync_work_items,
      (int)(sizeof(state->sync_work_items) / sizeof(state->sync_work_items[0])));

  psvDebugScreenClear(0x10141F);
  app_log_clear_history();

  printf("RomM Vita Sync - Game Sync\n\n");
  printf("Game ID : %s\n", game->game_id);
  printf("Title   : %s\n", has_text(game->title) ? game->title : "(no title)");
  printf("Saves   : %d\n\n", game_item_count);

  if (game_item_count <= 0) {
    printf("No local save found for this game.\n");
    printf("\nPress O or START to return.\n");
    ui_wait_for_return_button();
    return;
  }

  printf("[1/4] Validating RomM configuration...\n");
  if (!app_config_has_server_url(&state->config)) {
    printf("  ERROR: RomM server URL is empty.\n");
    ui_set_status(state, "Sync canceled: RomM URL is missing");
    printf("\nPress O or START to return.\n");
    ui_wait_for_return_button();
    return;
  }
  if (!app_config_has_auth(&state->config)) {
    printf("  ERROR: username/password or token is required.\n");
    ui_set_status(state, "Sync canceled: credentials are missing");
    printf("\nPress O or START to return.\n");
    ui_wait_for_return_button();
    return;
  }
  printf("  OK: credentials are configured.\n");

  printf("[2/4] Ensuring device registration...\n");
  int wrote_config = ensure_device_registration(&state->config, &state->romm_client);
  if (wrote_config) {
    printf("  OK: device_id persisted to settings.ini (%s)\n", state->config.device_id);
  } else if (has_text(state->config.device_id)) {
    printf("  OK: device_id available (%s)\n", state->config.device_id);
  } else {
    printf("  WARN: no device_id available, sync will continue if endpoints allow it.\n");
  }

  printf("[3/4] Resolving RomM rom_id mapping...\n");
  int mapped_count = romm_http_resolve_rom_ids(&state->config, state->sync_work_items, game_item_count);
  if (mapped_count < 0) {
    printf(
        "  WARN: Rom mapping failed: %s (%d)\n",
        romm_client_status_str(mapped_count),
        mapped_count);
  } else {
    printf("  OK: mapped %d/%d save(s).\n", mapped_count, game_item_count);
  }

  printf("[4/4] Running synchronization...\n");
  if (state->config.sync_dry_run) {
    printf("  INFO: dry-run is enabled (no upload/download execution).\n");
  }

  SyncEngineConfig config;
  sync_engine_config_init(&config);
  config.device_id = has_text(state->config.device_id) ? state->config.device_id : NULL;
  config.state_store_path = has_text(state->config.sync_state_store_path) ? state->config.sync_state_store_path : NULL;
  config.backup_directory = has_text(state->config.sync_backup_directory) ? state->config.sync_backup_directory : NULL;
  config.dry_run = state->config.sync_dry_run;

  int sync_status = sync_engine_run(
      &config,
      state->sync_work_items,
      game_item_count,
      &state->romm_client,
      &state->sync_report);
  if (sync_status < 0) {
    printf(
        "\nSynchronization failed: %s (%d)\n",
        sync_engine_status_str(sync_status),
        sync_status);
    ui_set_status(
        state,
        "Sync failed for %s: %s",
        game->game_id,
        sync_engine_status_str(sync_status));
  } else {
    printf("\nSynchronization finished.\n");
    ui_render_sync_report(&state->sync_report);
    ui_set_status(
        state,
        "Sync finished for %s (uploads=%d, downloads=%d, errors=%d)",
        game->game_id,
        state->sync_report.uploads_executed,
        state->sync_report.downloads_executed,
        state->sync_report.transfer_errors);
  }

  printf("\nPress O or START to return to main UI.\n");
  ui_wait_for_return_button();
}

/*
 * Prints the current in-memory log history as the bottom UI log panel.
 */
static void ui_render_log_panel(void) {
  printf("\nRecent Logs\n");

  int total = app_log_history_count();
  if (total <= 0) {
    printf("  (no logs yet)\n");
    return;
  }

  int start = total - UI_LOG_VISIBLE_LINES;
  if (start < 0) {
    start = 0;
  }

  for (int i = start; i < total; ++i) {
    const char *line = app_log_history_line(i);
    if (!has_text(line)) {
      continue;
    }

    char clipped[112];
    ui_truncate_text(line, clipped, sizeof(clipped));
    printf("  %s\n", clipped);
  }
}

/*
 * Draws the home screen with settings, game list, and log area.
 */
static void ui_render_main_screen(UiAppState *state) {
  if (state == NULL) {
    return;
  }

  psvDebugScreenClear(0x10141F);

  printf("RomM Vita Sync - PS1 Save Synchronization\n");
  printf("X Select/Edit | Up/Down Navigate | START Exit | SELECT Clear Logs\n");
  printf("Status: %s\n", has_text(state->status_line) ? state->status_line : "Ready");

  printf("\nSettings (stored in settings.ini)\n");
  char url_display[68];
  char user_display[44];
  char pass_display[44];
  ui_truncate_text(state->config.romm_url, url_display, sizeof(url_display));
  ui_truncate_text(state->config.romm_username, user_display, sizeof(user_display));
  ui_mask_secret(state->config.romm_password, pass_display, sizeof(pass_display));

  printf("%c Server URL : %s\n", (state->selected_index == UI_SELECT_SERVER_URL) ? '>' : ' ', url_display);
  printf("%c Username   : %s\n", (state->selected_index == UI_SELECT_USERNAME) ? '>' : ' ', user_display);
  printf("%c Password   : %s\n", (state->selected_index == UI_SELECT_PASSWORD) ? '>' : ' ', pass_display);
  printf(
      "%c Save settings to %s\n",
      (state->selected_index == UI_SELECT_SAVE_SETTINGS) ? '>' : ' ',
      APP_CONFIG_DEFAULT_PATH);
  printf("%c Rescan local PS1 games\n", (state->selected_index == UI_SELECT_RESCAN) ? '>' : ' ');
  printf("  Credentials are currently stored WITHOUT encryption.\n");

  printf("\nDetected PS1 Games (%d)\n", state->game_count);
  if (state->game_count <= 0) {
    printf("  No PS1 memory card files detected.\n");
  } else {
    int start = state->game_scroll;
    int end = start + UI_GAME_LIST_VISIBLE;
    if (end > state->game_count) {
      end = state->game_count;
    }

    for (int i = start; i < end; ++i) {
      const UiGameEntry *game = &state->games[i];
      int selected = (state->selected_index == (UI_SELECT_GAME_BASE + i));

      char title_display[44];
      ui_truncate_text(has_text(game->title) ? game->title : "(no title)", title_display, sizeof(title_display));
      printf(
          "%c [Sync] %s | %s | saves=%d\n",
          selected ? '>' : ' ',
          game->game_id,
          title_display,
          game->save_count);
    }

    if ((start > 0) || (end < state->game_count)) {
      printf("  showing %d-%d / %d\n", start + 1, end, state->game_count);
    }
  }

  ui_render_log_panel();
}

/*
 * Returns one printable representation for editor candidate character display.
 */
static char ui_display_char(char value) {
  if (value == ' ') {
    return '_';
  }
  if (value == '\0') {
    return '?';
  }
  return value;
}

/*
 * Renders a small moving window around the currently selected editor character.
 */
static void ui_render_charset_window(const char *charset, int selected_index) {
  if (!has_text(charset)) {
    return;
  }

  int length = (int)strlen(charset);
  if (length <= 0) {
    return;
  }

  if (selected_index < 0) {
    selected_index = 0;
  }
  if (selected_index >= length) {
    selected_index = length - 1;
  }

  int from = selected_index - 12;
  if (from < 0) {
    from = 0;
  }
  int to = from + 24;
  if (to > length) {
    to = length;
    from = to - 24;
    if (from < 0) {
      from = 0;
    }
  }

  for (int i = from; i < to; ++i) {
    char display = ui_display_char(charset[i]);
    if (i == selected_index) {
      printf("[%c]", display);
    } else {
      printf(" %c ", display);
    }
  }
  printf("\n");
}

/*
 * Opens a small controller-driven text editor and writes result when confirmed.
 */
static int ui_edit_text_field(
    const char *field_name,
    char *value,
    size_t value_size,
    int mask_value_in_preview) {
  if (!has_text(field_name) || (value == NULL) || (value_size == 0U) || (value_size > UI_EDITOR_BUFFER_LEN)) {
    return 0;
  }

  static const char *kCharsets[] = {
      "abcdefghijklmnopqrstuvwxyz0123456789-._:/@",
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._:/@",
      "0123456789-._:/@!#$%&*+?=,;()[]{} "};
  static const char *kCharsetNames[] = {
      "lowercase",
      "uppercase",
      "symbols"};

  char draft[UI_EDITOR_BUFFER_LEN];
  snprintf(draft, sizeof(draft), "%s", value);

  int charset_index = 0;
  int selected_char_index = 0;
  char message[96];
  message[0] = '\0';

  unsigned int previous_buttons = 0U;
  for (;;) {
    const char *charset = kCharsets[charset_index];
    int charset_length = (int)strlen(charset);
    if (charset_length <= 0) {
      return 0;
    }
    if (selected_char_index >= charset_length) {
      selected_char_index = charset_length - 1;
    }
    if (selected_char_index < 0) {
      selected_char_index = 0;
    }

    psvDebugScreenClear(0x10141F);
    printf("Edit %s\n\n", field_name);

    char preview[104];
    if (mask_value_in_preview) {
      ui_mask_secret(draft, preview, sizeof(preview));
    } else {
      ui_truncate_text(draft, preview, sizeof(preview));
    }
    printf("Current value: %s\n", preview);
    printf("Charset: %s\n", kCharsetNames[charset_index]);
    printf("Selected character: '%c'\n\n", ui_display_char(charset[selected_char_index]));
    ui_render_charset_window(charset, selected_char_index);

    printf("\nLeft/Right: choose character | Up/Down: change charset\n");
    printf("X: add character | Triangle: backspace | Select: clear\n");
    printf("START: save | O: cancel\n");
    if (has_text(message)) {
      printf("\n%s\n", message);
    }

    unsigned int pressed = ui_poll_pressed(&previous_buttons);
    if (pressed & SCE_CTRL_LEFT) {
      selected_char_index--;
      if (selected_char_index < 0) {
        selected_char_index = charset_length - 1;
      }
      message[0] = '\0';
    }
    if (pressed & SCE_CTRL_RIGHT) {
      selected_char_index++;
      if (selected_char_index >= charset_length) {
        selected_char_index = 0;
      }
      message[0] = '\0';
    }
    if (pressed & SCE_CTRL_UP) {
      charset_index--;
      if (charset_index < 0) {
        charset_index = (int)(sizeof(kCharsets) / sizeof(kCharsets[0])) - 1;
      }
      selected_char_index = 0;
      message[0] = '\0';
    }
    if (pressed & SCE_CTRL_DOWN) {
      charset_index++;
      if (charset_index >= (int)(sizeof(kCharsets) / sizeof(kCharsets[0]))) {
        charset_index = 0;
      }
      selected_char_index = 0;
      message[0] = '\0';
    }
    if (pressed & SCE_CTRL_CROSS) {
      size_t current_length = strlen(draft);
      if ((current_length + 1U) < value_size) {
        draft[current_length] = charset[selected_char_index];
        draft[current_length + 1U] = '\0';
        message[0] = '\0';
      } else {
        snprintf(message, sizeof(message), "Field is full (max %u chars)", (unsigned int)(value_size - 1U));
      }
    }
    if (pressed & SCE_CTRL_TRIANGLE) {
      size_t current_length = strlen(draft);
      if (current_length > 0U) {
        draft[current_length - 1U] = '\0';
      }
      message[0] = '\0';
    }
    if (pressed & SCE_CTRL_SELECT) {
      draft[0] = '\0';
      snprintf(message, sizeof(message), "Value cleared");
    }
    if (pressed & SCE_CTRL_START) {
      snprintf(value, value_size, "%s", draft);
      return 1;
    }
    if (pressed & SCE_CTRL_CIRCLE) {
      return 0;
    }

    sceKernelDelayThread(16 * 1000);
  }
}

/*
 * Handles editing of one text field and persists settings on successful edit.
 */
static void ui_edit_config_field(
    UiAppState *state,
    const char *label,
    char *field,
    size_t field_size,
    int mask_preview) {
  if ((state == NULL) || !has_text(label) || (field == NULL) || (field_size == 0U)) {
    return;
  }

  char previous_value[UI_EDITOR_BUFFER_LEN];
  snprintf(previous_value, sizeof(previous_value), "%s", field);

  int edited = ui_edit_text_field(label, field, field_size, mask_preview);
  if (!edited) {
    ui_set_status(state, "%s edit canceled", label);
    return;
  }

  if (strcmp(previous_value, field) == 0) {
    ui_set_status(state, "%s unchanged", label);
    return;
  }

  if (ui_save_config(state, "Settings updated") == APP_CONFIG_OK) {
    app_log_write(APP_LOG_LEVEL_INFO, "ui", "%s updated", label);
  }
}

/*
 * Executes action associated with the currently selected main UI row.
 */
static void ui_activate_selection(UiAppState *state) {
  if (state == NULL) {
    return;
  }

  if (state->selected_index == UI_SELECT_SERVER_URL) {
    ui_edit_config_field(
        state,
        "Server URL",
        state->config.romm_url,
        sizeof(state->config.romm_url),
        0);
    return;
  }

  if (state->selected_index == UI_SELECT_USERNAME) {
    ui_edit_config_field(
        state,
        "Username",
        state->config.romm_username,
        sizeof(state->config.romm_username),
        0);
    return;
  }

  if (state->selected_index == UI_SELECT_PASSWORD) {
    ui_edit_config_field(
        state,
        "Password",
        state->config.romm_password,
        sizeof(state->config.romm_password),
        1);
    return;
  }

  if (state->selected_index == UI_SELECT_SAVE_SETTINGS) {
    ui_save_config(state, "Settings saved");
    return;
  }

  if (state->selected_index == UI_SELECT_RESCAN) {
    ui_refresh_local_inventory(state);
    ui_clamp_selection(state);
    return;
  }

  if (state->selected_index >= UI_SELECT_GAME_BASE) {
    int game_index = state->selected_index - UI_SELECT_GAME_BASE;
    ui_run_sync_for_game(state, game_index);
    ui_refresh_local_inventory(state);
    ui_clamp_selection(state);
  }
}

/*
 * Initializes app state from disk configuration and builds first game list.
 */
static void ui_initialize_state(UiAppState *state) {
  if (state == NULL) {
    return;
  }

  memset(state, 0, sizeof(*state));
  int runtime_directory_status = ensure_runtime_data_directory();
  app_config_init_defaults(&state->config);
  state->config_status = app_config_load(APP_CONFIG_DEFAULT_PATH, &state->config);
  if ((state->config_status != APP_CONFIG_OK) && (state->config_status != APP_CONFIG_ERR_NOT_FOUND)) {
    app_log_write(
        APP_LOG_LEVEL_WARN,
        "main",
        "settings load error: %s (%d), using defaults",
        app_config_status_str(state->config_status),
        state->config_status);
    app_config_init_defaults(&state->config);
  }

  if (state->config_status == APP_CONFIG_ERR_NOT_FOUND) {
    app_log_write(APP_LOG_LEVEL_WARN, "main", "settings.ini not found, using defaults");
    ui_set_status(state, "settings.ini not found. Edit fields and save to create it.");
  } else if (state->config_status == APP_CONFIG_OK) {
    ui_set_status(state, "Configuration loaded from %s", APP_CONFIG_DEFAULT_PATH);
  } else {
    ui_set_status(
        state,
        "Configuration fallback to defaults: %s",
        app_config_status_str(state->config_status));
  }

  if (runtime_directory_status != BACKUP_MANAGER_OK) {
    ui_set_status(
        state,
        "Warning: cannot create %s (%s)",
        APP_RUNTIME_DATA_DIRECTORY,
        backup_manager_status_str(runtime_directory_status));
  }

  app_log_set_level(app_log_level_from_config(state->config.log_level));
  state->romm_client.context = &state->config;
  state->romm_client.list_remote_saves = romm_http_list_remote_saves_callback;
  state->romm_client.upload_save = romm_http_upload_save_callback;
  state->romm_client.download_save = romm_http_download_save_callback;
  state->romm_client.register_device = romm_http_register_device_callback;

  ui_refresh_local_inventory(state);
  state->selected_index = UI_SELECT_SERVER_URL;
}

/*
 * Entry point for the interactive PS Vita UI flow.
 */
int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  psvDebugScreenInit();
  sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
  app_log_clear_history();

  UiAppState *state = &g_app_state;
  ui_initialize_state(state);

  unsigned int previous_buttons = 0U;
  for (;;) {
    ui_clamp_selection(state);
    ui_update_game_scroll(state);
    ui_render_main_screen(state);

    unsigned int pressed = ui_poll_pressed(&previous_buttons);
    if (pressed & SCE_CTRL_START) {
      break;
    }

    int total_entries = ui_total_selectable_entries(state);
    if (pressed & SCE_CTRL_UP) {
      state->selected_index--;
      if (state->selected_index < 0) {
        state->selected_index = total_entries - 1;
      }
    }
    if (pressed & SCE_CTRL_DOWN) {
      state->selected_index++;
      if (state->selected_index >= total_entries) {
        state->selected_index = 0;
      }
    }
    if (pressed & SCE_CTRL_SELECT) {
      app_log_clear_history();
      ui_set_status(state, "Log history cleared");
    }
    if (pressed & SCE_CTRL_CROSS) {
      ui_activate_selection(state);
      previous_buttons = 0U;
    }

    sceKernelDelayThread(16 * 1000);
  }

  psvDebugScreenClear(0x10141F);
  printf("Exiting RomM Vita Sync...\n");
  sceKernelDelayThread(400 * 1000);
  return 0;
}
