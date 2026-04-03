#include <psp2/ctrl.h>
#include <psp2/ime_dialog.h>
#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <vita2d.h>

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
#include "ui_dialogs.h"

#define UI_SELECT_SERVER_URL 0
#define UI_SELECT_USERNAME 1
#define UI_SELECT_PASSWORD 2
#define UI_SELECT_SYNC_PRIMARY 3
#define UI_SELECT_SYNC_ALL 4
#define UI_SELECT_RESCAN 5
#define UI_SELECT_GAME_BASE 6

#define UI_SCREEN_WIDTH 960.0f
#define UI_SCREEN_HEIGHT 544.0f

#define UI_GAME_LIST_VISIBLE 4
#define UI_LOG_VISIBLE_LINES 5
#define UI_REPORT_VISIBLE_LINES 16
#define UI_REPORT_MAX_LINES 192
#define UI_STATUS_LINE_LEN 192
#define UI_EDITOR_BUFFER_LEN 512
#define APP_RUNTIME_DATA_DIRECTORY "ux0:data/romm-vita-sync"

#define UI_COLOR_BACKGROUND RGBA8(11, 15, 22, 255)
#define UI_COLOR_BACKGROUND_ALT RGBA8(16, 22, 32, 255)
#define UI_COLOR_HEADER RGBA8(18, 24, 34, 255)
#define UI_COLOR_FOOTER RGBA8(14, 19, 27, 255)
#define UI_COLOR_PANEL RGBA8(24, 32, 45, 236)
#define UI_COLOR_PANEL_ALT RGBA8(20, 28, 39, 236)
#define UI_COLOR_PANEL_BORDER RGBA8(84, 100, 122, 148)
#define UI_COLOR_PANEL_BORDER_ACTIVE RGBA8(94, 155, 255, 220)
#define UI_COLOR_FIELD RGBA8(28, 37, 51, 255)
#define UI_COLOR_FIELD_ACTIVE RGBA8(37, 52, 74, 255)
#define UI_COLOR_BUTTON RGBA8(60, 122, 218, 255)
#define UI_COLOR_BUTTON_ACTIVE RGBA8(84, 146, 244, 255)
#define UI_COLOR_BUTTON_DISABLED RGBA8(52, 60, 72, 255)
#define UI_COLOR_BUTTON_BORDER RGBA8(182, 214, 255, 96)
#define UI_COLOR_TEXT RGBA8(248, 250, 252, 255)
#define UI_COLOR_TEXT_MUTED RGBA8(191, 200, 214, 255)
#define UI_COLOR_TEXT_DIM RGBA8(130, 142, 160, 255)
#define UI_COLOR_STATUS RGBA8(224, 231, 241, 255)
#define UI_COLOR_ACCENT RGBA8(94, 155, 255, 255)
#define UI_COLOR_ACCENT_SOFT RGBA8(94, 155, 255, 56)
#define UI_COLOR_SUCCESS RGBA8(138, 214, 167, 255)
#define UI_COLOR_WARNING RGBA8(255, 194, 119, 255)
#define UI_COLOR_DANGER RGBA8(255, 140, 140, 255)

typedef struct UiGameEntry {
  char key[ROMM_GAME_ID_LEN];
  char game_id[ROMM_GAME_ID_LEN];
  char title[ROMM_GAME_TITLE_LEN];
  int save_count;
} UiGameEntry;

typedef struct UiReportBuffer {
  char lines[UI_REPORT_MAX_LINES][UI_STATUS_LINE_LEN];
  int count;
} UiReportBuffer;

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
  int active_game_index;
  int game_scroll;
  char status_line[UI_STATUS_LINE_LEN];
} UiAppState;

static UiAppState g_app_state;
static vita2d_pgf *g_ui_font = NULL;
static int g_common_dialog_active = 0;
static int g_dialog_runtime_initialized = 0;
static int g_ime_module_loaded = 0;

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
 * Writes one short status message shown in the main screen status line.
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
 * Formats a config field for display in the main UI.
 * Empty fields are normalized to a cleaner "Not configured" label.
 */
static void ui_format_field_display(const char *value, int secret, char *out_text, size_t out_size) {
  if ((out_text == NULL) || (out_size == 0U)) {
    return;
  }

  if (!has_text(value)) {
    snprintf(out_text, out_size, "Not configured");
    return;
  }

  if (secret) {
    ui_mask_secret(value, out_text, out_size);
    return;
  }

  ui_truncate_text(value, out_text, out_size);
}

/*
 * Trims leading and trailing ASCII whitespace in-place.
 * This keeps URL and username fields free of accidental padding from IME entry.
 */
static void ui_trim_ascii_whitespace(char *text) {
  if (text == NULL) {
    return;
  }

  char *start = text;
  while ((*start != '\0') && isspace((unsigned char)*start)) {
    start++;
  }

  char *end = start + strlen(start);
  while ((end > start) && isspace((unsigned char)*(end - 1))) {
    end--;
  }

  size_t trimmed_len = (size_t)(end - start);
  if (start != text) {
    memmove(text, start, trimmed_len);
  }
  text[trimmed_len] = '\0';
}

/*
 * Converts an ASCII/UTF-8 C string to a UTF-16 buffer for Vita IME APIs.
 * Non-ASCII bytes are mapped to '?' because config fields are ASCII-oriented.
 */
static void ui_to_wchar16(const char *source, SceWChar16 *out_text, size_t out_len) {
  if ((out_text == NULL) || (out_len == 0U)) {
    return;
  }

  out_text[0] = 0;
  if (!has_text(source)) {
    return;
  }

  size_t write = 0U;
  for (size_t i = 0U; source[i] != '\0' && (write + 1U) < out_len; ++i) {
    unsigned char c = (unsigned char)source[i];
    out_text[write++] = (c <= 0x7FU) ? (SceWChar16)c : (SceWChar16)'?';
  }
  out_text[write] = 0;
}

/*
 * Converts Vita IME UTF-16 output back into app ASCII/UTF-8 config buffers.
 * Characters outside printable ASCII are replaced with '?'.
 */
static void ui_from_wchar16(const SceWChar16 *source, char *out_text, size_t out_len) {
  if ((out_text == NULL) || (out_len == 0U)) {
    return;
  }

  out_text[0] = '\0';
  if (source == NULL) {
    return;
  }

  size_t write = 0U;
  for (size_t i = 0U; source[i] != 0 && (write + 1U) < out_len; ++i) {
    unsigned int codepoint = (unsigned int)source[i];
    out_text[write++] = (codepoint <= 0x7FU) ? (char)codepoint : '?';
  }
  out_text[write] = '\0';
}

/*
 * Initializes AppUtil/CommonDialog/IME modules required for system keyboard use.
 */
static int ui_dialog_runtime_init(void) {
  if (g_dialog_runtime_initialized) {
    return 0;
  }

  SceAppUtilInitParam app_util_init;
  SceAppUtilBootParam app_util_boot;
  memset(&app_util_init, 0, sizeof(app_util_init));
  memset(&app_util_boot, 0, sizeof(app_util_boot));

  int status = sceAppUtilInit(&app_util_init, &app_util_boot);
  if (status < 0) {
    app_log_write(APP_LOG_LEVEL_ERROR, "ui", "sceAppUtilInit failed: 0x%08X", (unsigned int)status);
    return status;
  }

  SceCommonDialogConfigParam dialog_config;
  sceCommonDialogConfigParamInit(&dialog_config);

  /*
   * Some runtimes reject the MAX_VALUE placeholders set by
   * sceCommonDialogConfigParamInit(), so resolve concrete system values.
   */
  int system_language = (int)SCE_SYSTEM_PARAM_LANG_ENGLISH_US;
  if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &system_language) < 0 ||
      (system_language < (int)SCE_SYSTEM_PARAM_LANG_JAPANESE) ||
      (system_language >= (int)SCE_SYSTEM_PARAM_LANG_MAX_VALUE)) {
    system_language = (int)SCE_SYSTEM_PARAM_LANG_ENGLISH_US;
  }
  dialog_config.language = (SceSystemParamLang)system_language;

  int enter_button = (int)SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
  if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enter_button) < 0 ||
      (enter_button < (int)SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE) ||
      (enter_button >= (int)SCE_SYSTEM_PARAM_ENTER_BUTTON_MAX_VALUE)) {
    enter_button = (int)SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
  }
  dialog_config.enterButtonAssign = (SceSystemParamEnterButtonAssign)enter_button;

  status = sceCommonDialogSetConfigParam(&dialog_config);
  if (status < 0) {
    /*
     * Defensive retry with safe hardcoded values for older/quirky runtimes.
     */
    dialog_config.language = SCE_SYSTEM_PARAM_LANG_ENGLISH_US;
    dialog_config.enterButtonAssign = SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
    status = sceCommonDialogSetConfigParam(&dialog_config);
  }
  if (status < 0) {
    app_log_write(APP_LOG_LEVEL_ERROR, "ui", "sceCommonDialogSetConfigParam failed: 0x%08X", (unsigned int)status);
    sceAppUtilShutdown();
    return status;
  }

  int ime_was_loaded = (sceSysmoduleIsLoaded(SCE_SYSMODULE_IME) >= 0);
  if (!ime_was_loaded) {
    status = sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
    if (status < 0) {
      app_log_write(APP_LOG_LEVEL_ERROR, "ui", "sceSysmoduleLoadModule(IME) failed: 0x%08X", (unsigned int)status);
      sceAppUtilShutdown();
      return status;
    }
  }

  g_ime_module_loaded = !ime_was_loaded;
  g_dialog_runtime_initialized = 1;
  return 0;
}

/*
 * Releases AppUtil/CommonDialog/IME runtime resources before process exit.
 */
static void ui_dialog_runtime_term(void) {
  if (!g_dialog_runtime_initialized) {
    return;
  }

  if (g_ime_module_loaded) {
    sceSysmoduleUnloadModule(SCE_SYSMODULE_IME);
    g_ime_module_loaded = 0;
  }

  sceAppUtilShutdown();
  g_dialog_runtime_initialized = 0;
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
 * Clamps the remembered sync target so the primary action always points at a valid game.
 */
static void ui_clamp_active_game(UiAppState *state) {
  if (state == NULL) {
    return;
  }

  if (state->game_count <= 0) {
    state->active_game_index = -1;
    return;
  }

  if (state->active_game_index < 0) {
    state->active_game_index = 0;
  }
  if (state->active_game_index >= state->game_count) {
    state->active_game_index = state->game_count - 1;
  }
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

  ui_clamp_active_game(state);
}

/*
 * Returns the currently armed synchronization target, or NULL when none exists.
 */
static const UiGameEntry *ui_active_game(const UiAppState *state) {
  if ((state == NULL) || (state->game_count <= 0)) {
    return NULL;
  }

  if ((state->active_game_index < 0) || (state->active_game_index >= state->game_count)) {
    return NULL;
  }

  return &state->games[state->active_game_index];
}

/*
 * Returns non-zero when the primary synchronization action should be enabled.
 */
static int ui_sync_action_enabled(const UiAppState *state) {
  if (ui_active_game(state) == NULL) {
    return 0;
  }

  if ((state == NULL) || !app_config_has_server_url(&state->config) || !app_config_has_auth(&state->config)) {
    return 0;
  }

  return 1;
}

/*
 * Returns non-zero when the full-library sync action should be enabled.
 */
static int ui_sync_all_action_enabled(const UiAppState *state) {
  if ((state == NULL) || (state->local_count <= 0)) {
    return 0;
  }

  if (!app_config_has_server_url(&state->config) || !app_config_has_auth(&state->config)) {
    return 0;
  }

  return 1;
}

/*
 * Updates game-list scroll offset so the armed game stays in view.
 */
static void ui_update_game_scroll(UiAppState *state) {
  if (state == NULL) {
    return;
  }

  if ((state->game_count <= 0) || (state->active_game_index < 0)) {
    state->game_scroll = 0;
    return;
  }

  if (state->active_game_index < state->game_scroll) {
    state->game_scroll = state->active_game_index;
  } else if (state->active_game_index >= (state->game_scroll + UI_GAME_LIST_VISIBLE)) {
    state->game_scroll = state->active_game_index - UI_GAME_LIST_VISIBLE + 1;
  }

  if (state->game_scroll < 0) {
    state->game_scroll = 0;
  }
  if (state->game_scroll > (state->game_count - UI_GAME_LIST_VISIBLE)) {
    state->game_scroll = state->game_count - UI_GAME_LIST_VISIBLE;
    if (state->game_scroll < 0) {
      state->game_scroll = 0;
    }
  }
}

/*
 * Pumps pending AppUtil events so dialogs stay responsive.
 */
static void ui_pump_app_events(void) {
  if (!g_dialog_runtime_initialized) {
    return;
  }

  SceAppUtilAppEventParam app_event;
  memset(&app_event, 0, sizeof(app_event));
  sceAppUtilReceiveAppEvent(&app_event);
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
 * Snaps drawing coordinates to physical pixels for crisper PGF text rendering.
 */
static float ui_snap_to_pixel(float value) {
  return floorf(value + 0.5f);
}

/*
 * Starts one drawing frame and paints a restrained dark Vita-style background.
 */
static void ui_begin_frame(void) {
  vita2d_start_drawing();
  vita2d_clear_screen();

  vita2d_draw_rectangle(0.0f, 0.0f, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT, UI_COLOR_BACKGROUND);
  vita2d_draw_rectangle(0.0f, 0.0f, UI_SCREEN_WIDTH, 74.0f, UI_COLOR_HEADER);
  vita2d_draw_rectangle(0.0f, 74.0f, UI_SCREEN_WIDTH, 1.0f, UI_COLOR_PANEL_BORDER);
  vita2d_draw_rectangle(0.0f, 74.0f, UI_SCREEN_WIDTH, 421.0f, UI_COLOR_BACKGROUND_ALT);
  vita2d_draw_rectangle(0.0f, 496.0f, UI_SCREEN_WIDTH, UI_SCREEN_HEIGHT - 496.0f, UI_COLOR_FOOTER);
  vita2d_draw_rectangle(0.0f, 495.0f, UI_SCREEN_WIDTH, 1.0f, UI_COLOR_PANEL_BORDER);
  vita2d_draw_rectangle(32.0f, 88.0f, 3.0f, 398.0f, UI_COLOR_ACCENT_SOFT);
}

/*
 * Ends one drawing frame and swaps front/back buffers.
 */
static void ui_end_frame(void) {
  vita2d_end_drawing();
  if (g_common_dialog_active) {
    vita2d_common_dialog_update();
  }
  vita2d_swap_buffers();
}

/*
 * Draws a formatted text line using the default PGF font.
 * Coordinates are snapped to reduce the soft look caused by sub-pixel placement.
 */
static void ui_draw_text(float x, float y, unsigned int color, float scale, const char *format, ...) {
  if ((g_ui_font == NULL) || !has_text(format)) {
    return;
  }

  char line[512];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);

  vita2d_pgf_draw_text(g_ui_font, ui_snap_to_pixel(x), ui_snap_to_pixel(y), color, scale, line);
}

/*
 * Estimates text width to center short labels in panels.
 */
static float ui_estimate_text_width(const char *text, float scale) {
  if (!has_text(text)) {
    return 0.0f;
  }
  return (float)strlen(text) * 11.0f * scale;
}

/*
 * Draws text centered around a target x coordinate.
 */
static void ui_draw_text_center(float center_x, float y, unsigned int color, float scale, const char *text) {
  if (!has_text(text)) {
    return;
  }

  float width = ui_estimate_text_width(text, scale);
  ui_draw_text(center_x - (width * 0.5f), y, color, scale, "%s", text);
}

/*
 * Draws one clean card or panel with a subtle border.
 */
static void ui_draw_panel(float x, float y, float w, float h, unsigned int fill, unsigned int border) {
  if ((w <= 0.0f) || (h <= 0.0f)) {
    return;
  }

  x = ui_snap_to_pixel(x);
  y = ui_snap_to_pixel(y);
  w = ui_snap_to_pixel(w);
  h = ui_snap_to_pixel(h);

  vita2d_draw_rectangle(x, y, w, h, fill);
  vita2d_draw_rectangle(x, y, w, 1.0f, border);
  vita2d_draw_rectangle(x, y + h - 1.0f, w, 1.0f, border);
  vita2d_draw_rectangle(x, y, 1.0f, h, border);
  vita2d_draw_rectangle(x + w - 1.0f, y, 1.0f, h, border);
}

/*
 * Draws one editable field row with label/value hierarchy.
 */
static void ui_draw_field_row(float x, float y, float w, float h, int selected, const char *label, const char *value) {
  unsigned int fill = selected ? UI_COLOR_FIELD_ACTIVE : UI_COLOR_FIELD;
  unsigned int border = selected ? UI_COLOR_PANEL_BORDER_ACTIVE : UI_COLOR_PANEL_BORDER;
  unsigned int value_color = has_text(value) ? UI_COLOR_TEXT : UI_COLOR_TEXT_DIM;

  ui_draw_panel(x, y, w, h, fill, border);
  if (selected) {
    vita2d_draw_rectangle(ui_snap_to_pixel(x), ui_snap_to_pixel(y), 4.0f, ui_snap_to_pixel(h), UI_COLOR_ACCENT);
  }

  ui_draw_text(x + 16.0f, y + 17.0f, UI_COLOR_TEXT_DIM, 0.78f, "%s", label);
  ui_draw_text(x + 16.0f, y + 39.0f, value_color, 0.92f, "%s", value);
}

/*
 * Draws one action button.
 * Primary buttons use the accent color; secondary buttons stay neutral.
 */
static void ui_draw_button(float x, float y, float w, float h, int primary, int selected, int enabled, const char *title) {
  unsigned int fill = UI_COLOR_FIELD;
  unsigned int border = selected ? UI_COLOR_PANEL_BORDER_ACTIVE : UI_COLOR_PANEL_BORDER;
  unsigned int text_color = enabled ? UI_COLOR_TEXT : UI_COLOR_TEXT_DIM;

  if (primary) {
    fill = enabled ? (selected ? UI_COLOR_BUTTON_ACTIVE : UI_COLOR_BUTTON) : UI_COLOR_BUTTON_DISABLED;
    border = enabled ? (selected ? UI_COLOR_PANEL_BORDER_ACTIVE : UI_COLOR_BUTTON_BORDER) : UI_COLOR_PANEL_BORDER;
    text_color = enabled ? UI_COLOR_TEXT : UI_COLOR_TEXT_MUTED;
  } else if (selected) {
    fill = UI_COLOR_FIELD_ACTIVE;
  }

  ui_draw_panel(x, y, w, h, fill, border);
  if (selected) {
    vita2d_draw_rectangle(ui_snap_to_pixel(x), ui_snap_to_pixel(y), 4.0f, ui_snap_to_pixel(h), UI_COLOR_ACCENT);
  }

  ui_draw_text_center(x + (w * 0.5f), y + (h * 0.62f), text_color, primary ? 0.90f : 0.78f, title);
}

/*
 * Draws one compact game-list row.
 * The active sync target remains softly marked even when focus is elsewhere.
 */
static void ui_draw_game_row(
    float x,
    float y,
    float w,
    float h,
    int focused,
    int active,
    const char *title,
    int save_count) {
  unsigned int fill = focused ? UI_COLOR_FIELD_ACTIVE : (active ? UI_COLOR_ACCENT_SOFT : UI_COLOR_PANEL_ALT);
  unsigned int border = focused ? UI_COLOR_PANEL_BORDER_ACTIVE : (active ? UI_COLOR_BUTTON_BORDER : UI_COLOR_PANEL_BORDER);
  unsigned int title_color = focused ? UI_COLOR_TEXT : (active ? UI_COLOR_TEXT : UI_COLOR_TEXT_MUTED);
  unsigned int count_color = active ? UI_COLOR_TEXT : UI_COLOR_TEXT_DIM;

  ui_draw_panel(x, y, w, h, fill, border);
  if (active) {
    vita2d_draw_rectangle(ui_snap_to_pixel(x), ui_snap_to_pixel(y), 3.0f, ui_snap_to_pixel(h), UI_COLOR_ACCENT);
  }

  ui_draw_text(x + 12.0f, y + 16.0f, title_color, 0.82f, "%s", title);
  ui_draw_text(
      x + w - 106.0f,
      y + 16.0f,
      count_color,
      0.76f,
      "%d card%s",
      save_count,
      (save_count == 1) ? "" : "s");
}

/*
 * Renders the top title area.
 */
static void ui_render_header(const UiAppState *state) {
  ui_draw_text(32.0f, 28.0f, UI_COLOR_TEXT_DIM, 0.78f, "RomM Vita Sync");
  ui_draw_text(32.0f, 54.0f, UI_COLOR_TEXT, 1.10f, "Save Synchronization");
  ui_draw_text(32.0f, 70.0f, UI_COLOR_TEXT_MUTED, 0.80f, "Configure RoMM access, choose a PS1 game, then launch a manual sync.");

  ui_draw_text(736.0f, 28.0f, UI_COLOR_TEXT_DIM, 0.78f, "Status");
  ui_draw_text(
      736.0f,
      54.0f,
      ui_sync_action_enabled(state) ? UI_COLOR_SUCCESS : UI_COLOR_WARNING,
      0.82f,
      "%s",
      ui_sync_action_enabled(state) ? "Ready to synchronize" : "Setup required");
}

/*
 * Renders the credentials form.
 */
static void ui_render_connection_panel(const UiAppState *state) {
  if (state == NULL) {
    return;
  }

  char url_display[96];
  char user_display[96];
  char pass_display[96];
  ui_format_field_display(state->config.romm_url, 0, url_display, sizeof(url_display));
  ui_format_field_display(state->config.romm_username, 0, user_display, sizeof(user_display));
  ui_format_field_display(state->config.romm_password, 1, pass_display, sizeof(pass_display));

  ui_draw_panel(32.0f, 88.0f, 430.0f, 258.0f, UI_COLOR_PANEL, UI_COLOR_PANEL_BORDER);
  ui_draw_text(48.0f, 118.0f, UI_COLOR_TEXT, 0.96f, "Connection");
  ui_draw_text(48.0f, 140.0f, UI_COLOR_TEXT_MUTED, 0.78f, "Press X to edit a field. Changes save immediately.");

  ui_draw_field_row(48.0f, 154.0f, 398.0f, 44.0f, state->selected_index == UI_SELECT_SERVER_URL, "RoMM server address", url_display);
  ui_draw_field_row(48.0f, 208.0f, 398.0f, 44.0f, state->selected_index == UI_SELECT_USERNAME, "RoMM username", user_display);
  ui_draw_field_row(48.0f, 262.0f, 398.0f, 44.0f, state->selected_index == UI_SELECT_PASSWORD, "RoMM password", pass_display);

  ui_draw_text(48.0f, 332.0f, UI_COLOR_WARNING, 0.76f, "Credentials remain on-device in plain text.");
}

/*
 * Renders the active sync target summary and the primary action.
 */
static void ui_render_sync_panel(const UiAppState *state) {
  if (state == NULL) {
    return;
  }

  const UiGameEntry *game = ui_active_game(state);
  char title_display[80];
  char detail[128];
  char readiness[96];
  unsigned int readiness_color = UI_COLOR_SUCCESS;
  int sync_enabled = ui_sync_action_enabled(state);
  int sync_all_enabled = ui_sync_all_action_enabled(state);

  if (game != NULL) {
    const char *resolved_title = has_text(game->title) ? game->title : game->game_id;
    ui_truncate_text(resolved_title, title_display, sizeof(title_display));
    snprintf(detail, sizeof(detail), "%s  |  %d save card(s)", game->game_id, game->save_count);
  } else {
    snprintf(title_display, sizeof(title_display), "No PS1 game selected");
    snprintf(detail, sizeof(detail), "Rescan local saves after copying memory cards to the Vita.");
  }

  if (game == NULL) {
    snprintf(readiness, sizeof(readiness), "No local PS1 saves detected yet.");
    readiness_color = UI_COLOR_WARNING;
  } else if (!app_config_has_server_url(&state->config)) {
    snprintf(readiness, sizeof(readiness), "Enter the RoMM server address first.");
    readiness_color = UI_COLOR_WARNING;
  } else if (!app_config_has_auth(&state->config)) {
    snprintf(readiness, sizeof(readiness), "Enter both username and password to enable sync.");
    readiness_color = UI_COLOR_WARNING;
  } else {
    snprintf(readiness, sizeof(readiness), "Connection details look complete.");
  }

  ui_draw_panel(478.0f, 88.0f, 450.0f, 258.0f, UI_COLOR_PANEL, UI_COLOR_PANEL_BORDER);
  ui_draw_text(494.0f, 118.0f, UI_COLOR_TEXT, 0.96f, "Synchronize");
  ui_draw_text(494.0f, 140.0f, UI_COLOR_TEXT_MUTED, 0.78f, "Move through the list below to choose the target game.");
  ui_draw_text(494.0f, 184.0f, game != NULL ? UI_COLOR_TEXT : UI_COLOR_TEXT_MUTED, 0.98f, "%s", title_display);
  ui_draw_text(494.0f, 208.0f, UI_COLOR_TEXT_MUTED, 0.78f, "%s", detail);
  ui_draw_text(494.0f, 232.0f, readiness_color, 0.78f, "%s", readiness);
  ui_draw_button(
      494.0f,
      252.0f,
      418.0f,
      28.0f,
      1,
      state->selected_index == UI_SELECT_SYNC_PRIMARY,
      sync_enabled,
      "Synchronize Selected Game");
  ui_draw_button(
      494.0f,
      284.0f,
      418.0f,
      28.0f,
      0,
      state->selected_index == UI_SELECT_SYNC_ALL,
      sync_all_enabled,
      "Synchronize All Saves");
  ui_draw_button(
      494.0f,
      316.0f,
      418.0f,
      28.0f,
      0,
      state->selected_index == UI_SELECT_RESCAN,
      1,
      "Rescan Local Saves");
}

/*
 * Renders the detected PS1 game list.
 */
static void ui_render_game_panel(const UiAppState *state) {
  if (state == NULL) {
    return;
  }

  ui_draw_panel(32.0f, 354.0f, 896.0f, 132.0f, UI_COLOR_PANEL, UI_COLOR_PANEL_BORDER);
  ui_draw_text(48.0f, 378.0f, UI_COLOR_TEXT, 0.90f, "Detected PS1 Games");

  if (state->game_count <= 0) {
    ui_draw_text(48.0f, 422.0f, UI_COLOR_TEXT_MUTED, 0.84f, "No PS1 memory card files were detected on this Vita.");
    return;
  }

  int start = state->game_scroll;
  int end = start + UI_GAME_LIST_VISIBLE;
  if (end > state->game_count) {
    end = state->game_count;
  }

  ui_draw_text(772.0f, 378.0f, UI_COLOR_TEXT_DIM, 0.76f, "Showing %d-%d of %d", start + 1, end, state->game_count);

  float row_y = 390.0f;
  for (int i = start; i < end; ++i) {
    const UiGameEntry *game = &state->games[i];
    char full_title[128];
    char row_title[96];
    const char *resolved_title = has_text(game->title) ? game->title : game->game_id;
    snprintf(full_title, sizeof(full_title), "%s [%s]", resolved_title, game->game_id);
    ui_truncate_text(full_title, row_title, sizeof(row_title));

    ui_draw_game_row(
        48.0f,
        row_y,
        864.0f,
        24.0f,
        state->selected_index == (UI_SELECT_GAME_BASE + i),
        state->active_game_index == i,
        row_title,
        game->save_count);
    row_y += 24.0f;
  }
}

/*
 * Renders the footer status strip and controls hint.
 */
static void ui_render_footer(const UiAppState *state) {
  if (state == NULL) {
    return;
  }

  char status[UI_STATUS_LINE_LEN];
  ui_truncate_text(state->status_line, status, sizeof(status));

  ui_draw_text(32.0f, 522.0f, UI_COLOR_TEXT_DIM, 0.76f, "Status");
  ui_draw_text(92.0f, 522.0f, UI_COLOR_STATUS, 0.80f, "%s", status);
  ui_draw_text(640.0f, 522.0f, UI_COLOR_TEXT_MUTED, 0.74f, "D-Pad navigate   X edit/apply   START exit");
}

/*
 * Draws the home screen with the form, sync action, and game list.
 */
static void ui_render_main_screen(UiAppState *state) {
  if (state == NULL) {
    return;
  }

  ui_begin_frame();
  ui_render_header(state);
  ui_render_connection_panel(state);
  ui_render_sync_panel(state);
  ui_render_game_panel(state);
  ui_render_footer(state);
  ui_end_frame();
}

/*
 * Draws one centered busy screen while long synchronous tasks execute.
 */
static void ui_render_busy_screen(const char *title, const char *subtitle) {
  ui_begin_frame();
  ui_draw_panel(168.0f, 194.0f, 624.0f, 146.0f, UI_COLOR_PANEL, UI_COLOR_PANEL_BORDER);
  ui_draw_text_center(UI_SCREEN_WIDTH * 0.5f, 244.0f, UI_COLOR_TEXT, 1.00f, has_text(title) ? title : "Please wait");
  if (has_text(subtitle)) {
    ui_draw_text_center(UI_SCREEN_WIDTH * 0.5f, 272.0f, UI_COLOR_TEXT_MUTED, 0.82f, subtitle);
  }
  ui_end_frame();
}

/*
 * Scans local Vita storage for PS1 saves and rebuilds the UI game list.
 * When possible, the previously armed sync target is restored after the rescan.
 */
static int ui_refresh_local_inventory(UiAppState *state) {
  if (state == NULL) {
    return -1;
  }

  char previous_active_key[ROMM_GAME_ID_LEN];
  previous_active_key[0] = '\0';
  const UiGameEntry *previous_active = ui_active_game(state);
  if (previous_active != NULL) {
    snprintf(previous_active_key, sizeof(previous_active_key), "%s", previous_active->key);
  }

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
    ui_set_status(state, "Failed to build sync inventory from scan result");
    app_log_write(APP_LOG_LEVEL_ERROR, "ui", "scan_result_to_sync_saves failed");
    return -1;
  }

  state->game_count = ui_build_game_entries(
      state->local_items,
      state->local_count,
      state->games,
      (int)(sizeof(state->games) / sizeof(state->games[0])));

  if ((state->game_count > 0) && has_text(previous_active_key)) {
    int restored_index = ui_find_game_entry(state->games, state->game_count, previous_active_key);
    state->active_game_index = (restored_index >= 0) ? restored_index : 0;
  } else {
    state->active_game_index = (state->game_count > 0) ? 0 : -1;
  }

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
 * Clears the report line buffer before appending a new operation report.
 */
static void ui_report_clear(UiReportBuffer *buffer) {
  if (buffer == NULL) {
    return;
  }

  memset(buffer, 0, sizeof(*buffer));
}

/*
 * Appends one formatted line to a report buffer when capacity allows.
 */
static void ui_report_add(UiReportBuffer *buffer, const char *format, ...) {
  if ((buffer == NULL) || (format == NULL)) {
    return;
  }

  if (buffer->count >= UI_REPORT_MAX_LINES) {
    return;
  }

  va_list args;
  va_start(args, format);
  vsnprintf(buffer->lines[buffer->count], sizeof(buffer->lines[buffer->count]), format, args);
  va_end(args);
  buffer->count += 1;
}

/*
 * Draws one report screen page with optional vertical scrolling.
 */
static void ui_render_report_screen(const char *title, const UiReportBuffer *buffer, int scroll) {
  ui_begin_frame();
  ui_draw_panel(32.0f, 24.0f, 896.0f, 496.0f, UI_COLOR_PANEL, UI_COLOR_PANEL_BORDER);
  ui_draw_text(52.0f, 56.0f, UI_COLOR_TEXT, 0.95f, "%s", has_text(title) ? title : "Report");

  int total = (buffer != NULL) ? buffer->count : 0;
  int start = scroll;
  if (start < 0) {
    start = 0;
  }
  if (start > total) {
    start = total;
  }

  int end = start + UI_REPORT_VISIBLE_LINES;
  if (end > total) {
    end = total;
  }

  float y = 86.0f;
  if (total <= 0) {
    ui_draw_text(54.0f, 120.0f, UI_COLOR_TEXT_MUTED, 0.80f, "No details available.");
  } else {
    for (int i = start; i < end; ++i) {
      char clipped[UI_STATUS_LINE_LEN];
      ui_truncate_text(buffer->lines[i], clipped, sizeof(clipped));
      ui_draw_text(54.0f, y, UI_COLOR_TEXT, 0.74f, "%s", clipped);
      y += 24.0f;
    }
  }

  if (total > UI_REPORT_VISIBLE_LINES) {
    ui_draw_text(
        744.0f,
        502.0f,
        UI_COLOR_TEXT_MUTED,
        0.68f,
        "UP/DOWN scroll %d-%d/%d",
        start + 1,
        end,
        total);
  }

  ui_draw_text(54.0f, 502.0f, UI_COLOR_TEXT_MUTED, 0.68f, "O/X/START: return");
  ui_end_frame();
}

/*
 * Presents a scrollable report until user confirms return to home screen.
 */
static void ui_present_report(const char *title, const UiReportBuffer *buffer) {
  int total = (buffer != NULL) ? buffer->count : 0;
  int max_scroll = total - UI_REPORT_VISIBLE_LINES;
  if (max_scroll < 0) {
    max_scroll = 0;
  }

  int scroll = 0;
  unsigned int previous_buttons = 0U;
  for (;;) {
    ui_pump_app_events();
    ui_render_report_screen(title, buffer, scroll);

    unsigned int pressed = ui_poll_pressed(&previous_buttons);
    if ((pressed & SCE_CTRL_UP) && (scroll > 0)) {
      scroll -= 1;
    }
    if ((pressed & SCE_CTRL_DOWN) && (scroll < max_scroll)) {
      scroll += 1;
    }
    if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_CROSS | SCE_CTRL_START)) {
      return;
    }

    sceKernelDelayThread(16 * 1000);
  }
}

/*
 * Appends sync summary metrics and action lines to a report buffer.
 */
static void ui_append_sync_report(UiReportBuffer *buffer, const SyncRunReport *report) {
  if ((buffer == NULL) || (report == NULL)) {
    return;
  }

  ui_report_add(buffer, "");
  ui_report_add(buffer, "Sync summary");
  ui_report_add(buffer, "  local saves        : %d", report->local_count);
  ui_report_add(buffer, "  remote saves       : %d", report->remote_count);
  ui_report_add(buffer, "  uploads planned    : %d", report->uploads_planned);
  ui_report_add(buffer, "  downloads planned  : %d", report->downloads_planned);
  ui_report_add(buffer, "  uploads executed   : %d", report->uploads_executed);
  ui_report_add(buffer, "  downloads executed : %d", report->downloads_executed);
  ui_report_add(buffer, "  conflicts          : %d", report->conflicts_detected);
  ui_report_add(buffer, "  skipped            : %d", report->skipped);
  ui_report_add(buffer, "  errors             : %d", report->transfer_errors);

  if (report->action_count <= 0) {
    return;
  }

  ui_report_add(buffer, "");
  ui_report_add(buffer, "Action log");

  int render_count = report->action_count;
  if (render_count > 24) {
    render_count = 24;
  }

  for (int i = 0; i < render_count; ++i) {
    const SyncActionRecord *action = &report->actions[i];
    ui_report_add(
        buffer,
        "[%02d] %s %s %s status=%d reason=%s",
        i + 1,
        sync_slot_str(action->slot),
        sync_action_type_str(action->action),
        action->executed ? "executed" : "planned",
        action->status_code,
        action->reason);
  }

  if (report->action_count > render_count) {
    ui_report_add(buffer, "... %d more action(s) not shown", report->action_count - render_count);
  }
}

/*
 * Runs synchronization for one selected game and renders a full report modal.
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

  UiReportBuffer report;
  ui_report_clear(&report);
  app_log_clear_history();

  ui_report_add(&report, "Game sync report");
  ui_report_add(&report, "Game ID : %s", game->game_id);
  ui_report_add(&report, "Title   : %s", has_text(game->title) ? game->title : "(no title)");
  ui_report_add(&report, "Saves   : %d", game_item_count);

  char confirm_msg[256];
  snprintf(confirm_msg, sizeof(confirm_msg),
           "Synchronize %s?\n%d save card(s) will be checked.",
           has_text(game->title) ? game->title : game->game_id,
           game_item_count);
  if (ui_dialog_confirm(confirm_msg) != 1) {
    ui_set_status(state, "Sync canceled for %s", game->game_id);
    return;
  }

  if (game_item_count <= 0) {
    ui_set_status(state, "No local save found for %s", game->game_id);
    ui_report_add(&report, "");
    ui_report_add(&report, "No local save found for this game.");
    ui_present_report("Synchronization", &report);
    return;
  }

  ui_render_busy_screen("Synchronizing game", game->game_id);

  ui_report_add(&report, "");
  ui_report_add(&report, "[1/4] Validating RomM configuration...");
  if (!app_config_has_server_url(&state->config)) {
    ui_report_add(&report, "ERROR: RomM server URL is empty.");
    ui_set_status(state, "Sync canceled: RomM URL is missing");
    ui_present_report("Synchronization", &report);
    return;
  }
  if (!app_config_has_auth(&state->config)) {
    ui_report_add(&report, "ERROR: username/password or token is required.");
    ui_set_status(state, "Sync canceled: credentials are missing");
    ui_present_report("Synchronization", &report);
    return;
  }
  ui_report_add(&report, "OK: credentials are configured.");

  ui_report_add(&report, "[2/4] Ensuring device registration...");
  int wrote_config = ensure_device_registration(&state->config, &state->romm_client);
  if (wrote_config) {
    ui_report_add(&report, "OK: device_id persisted to settings.ini (%s)", state->config.device_id);
  } else if (has_text(state->config.device_id)) {
    ui_report_add(&report, "OK: device_id available (%s)", state->config.device_id);
  } else {
    ui_report_add(&report, "WARN: no device_id available, sync continues if endpoint allows it.");
  }

  ui_report_add(&report, "[3/4] Resolving RomM rom_id mapping...");
  int mapped_count = romm_http_resolve_rom_ids(&state->config, state->sync_work_items, game_item_count);
  if (mapped_count < 0) {
    ui_report_add(
        &report,
        "WARN: Rom mapping failed: %s (%d)",
        romm_client_status_str(mapped_count),
        mapped_count);
  } else {
    ui_report_add(&report, "OK: mapped %d/%d save(s).", mapped_count, game_item_count);
  }

  ui_report_add(&report, "[4/4] Running synchronization...");
  if (state->config.sync_dry_run) {
    ui_report_add(&report, "INFO: dry-run is enabled (no upload/download execution).");
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
    char err_msg[256];
    snprintf(err_msg, sizeof(err_msg),
             "Sync failed for %s:\n%s (%d)",
             game->game_id,
             sync_engine_status_str(sync_status),
             sync_status);
    ui_dialog_error(err_msg);
    ui_report_add(
        &report,
        "Synchronization failed: %s (%d)",
        sync_engine_status_str(sync_status),
        sync_status);
    ui_set_status(
        state,
        "Sync failed for %s: %s",
        game->game_id,
        sync_engine_status_str(sync_status));
  } else {
    ui_report_add(&report, "Synchronization finished.");
    ui_append_sync_report(&report, &state->sync_report);
    ui_set_status(
        state,
        "Sync finished for %s (uploads=%d, downloads=%d, errors=%d)",
        game->game_id,
        state->sync_report.uploads_executed,
        state->sync_report.downloads_executed,
        state->sync_report.transfer_errors);
  }

  ui_present_report("Synchronization", &report);
}

/*
 * Runs one synchronization pass for all detected local saves and renders one report modal.
 */
static void ui_run_sync_all_saves(UiAppState *state) {
  if (state == NULL) {
    return;
  }

  if (state->local_count <= 0) {
    ui_set_status(state, "No local PS1 saves were detected");
    return;
  }

  int work_item_count = state->local_count;
  if (work_item_count > (int)(sizeof(state->sync_work_items) / sizeof(state->sync_work_items[0]))) {
    work_item_count = (int)(sizeof(state->sync_work_items) / sizeof(state->sync_work_items[0]));
  }
  memcpy(state->sync_work_items, state->local_items, sizeof(state->sync_work_items[0]) * (size_t)work_item_count);

  UiReportBuffer report;
  ui_report_clear(&report);
  app_log_clear_history();

  ui_report_add(&report, "Full sync report");
  ui_report_add(&report, "Games   : %d", state->game_count);
  ui_report_add(&report, "Saves   : %d", work_item_count);

  char confirm_msg[256];
  snprintf(
      confirm_msg,
      sizeof(confirm_msg),
      "Synchronize all detected PS1 saves?\n%d save card(s) across %d game(s) will be checked.",
      work_item_count,
      state->game_count);
  if (ui_dialog_confirm(confirm_msg) != 1) {
    ui_set_status(state, "Sync canceled for all games");
    return;
  }

  ui_render_busy_screen("Synchronizing all games", "All detected saves are being checked");

  ui_report_add(&report, "");
  ui_report_add(&report, "[1/4] Validating RomM configuration...");
  if (!app_config_has_server_url(&state->config)) {
    ui_report_add(&report, "ERROR: RomM server URL is empty.");
    ui_set_status(state, "Sync canceled: RomM URL is missing");
    ui_present_report("Synchronization", &report);
    return;
  }
  if (!app_config_has_auth(&state->config)) {
    ui_report_add(&report, "ERROR: username/password or token is required.");
    ui_set_status(state, "Sync canceled: credentials are missing");
    ui_present_report("Synchronization", &report);
    return;
  }
  ui_report_add(&report, "OK: credentials are configured.");

  ui_report_add(&report, "[2/4] Ensuring device registration...");
  int wrote_config = ensure_device_registration(&state->config, &state->romm_client);
  if (wrote_config) {
    ui_report_add(&report, "OK: device_id persisted to settings.ini (%s)", state->config.device_id);
  } else if (has_text(state->config.device_id)) {
    ui_report_add(&report, "OK: device_id available (%s)", state->config.device_id);
  } else {
    ui_report_add(&report, "WARN: no device_id available, sync continues if endpoint allows it.");
  }

  ui_report_add(&report, "[3/4] Resolving RomM rom_id mapping...");
  int mapped_count = romm_http_resolve_rom_ids(&state->config, state->sync_work_items, work_item_count);
  if (mapped_count < 0) {
    ui_report_add(
        &report,
        "WARN: Rom mapping failed: %s (%d)",
        romm_client_status_str(mapped_count),
        mapped_count);
  } else {
    ui_report_add(&report, "OK: mapped %d/%d save(s).", mapped_count, work_item_count);
  }

  ui_report_add(&report, "[4/4] Running synchronization...");
  if (state->config.sync_dry_run) {
    ui_report_add(&report, "INFO: dry-run is enabled (no upload/download execution).");
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
      work_item_count,
      &state->romm_client,
      &state->sync_report);
  if (sync_status < 0) {
    char err_msg[256];
    snprintf(err_msg, sizeof(err_msg),
             "Sync failed for all games:\n%s (%d)",
             sync_engine_status_str(sync_status),
             sync_status);
    ui_dialog_error(err_msg);
    ui_report_add(
        &report,
        "Synchronization failed: %s (%d)",
        sync_engine_status_str(sync_status),
        sync_status);
    ui_set_status(
        state,
        "Sync failed for all games: %s",
        sync_engine_status_str(sync_status));
  } else {
    ui_report_add(&report, "Synchronization finished.");
    ui_append_sync_report(&report, &state->sync_report);
    ui_set_status(
        state,
        "Sync finished for all games (uploads=%d, downloads=%d, errors=%d)",
        state->sync_report.uploads_executed,
        state->sync_report.downloads_executed,
        state->sync_report.transfer_errors);
  }

  ui_present_report("Synchronization", &report);
}

/*
 * Opens the official PS Vita IME keyboard for one text field.
 */
static int ui_edit_text_field(
    UiAppState *state,
    const char *field_name,
    char *value,
    size_t value_size,
    unsigned int ime_type,
    unsigned int textbox_mode) {
  if (!has_text(field_name) || (value == NULL) || (value_size == 0U) || (value_size > UI_EDITOR_BUFFER_LEN)) {
    return -1;
  }

  if (!g_dialog_runtime_initialized) {
    int runtime_status = ui_dialog_runtime_init();
    if (runtime_status < 0) {
      if (state != NULL) {
        ui_set_status(state, "System keyboard is unavailable (0x%08X)", (unsigned int)runtime_status);
      }
      app_log_write(
          APP_LOG_LEVEL_ERROR,
          "ui",
          "IME edit requested but dialog runtime init failed: 0x%08X",
          (unsigned int)runtime_status);
      return -1;
    }
    app_log_write(APP_LOG_LEVEL_INFO, "ui", "dialog runtime initialized on-demand for keyboard input");
  }

  size_t max_chars = value_size - 1U;
  if (max_chars > (size_t)SCE_IME_DIALOG_MAX_TEXT_LENGTH) {
    max_chars = (size_t)SCE_IME_DIALOG_MAX_TEXT_LENGTH;
  }
  if (max_chars >= UI_EDITOR_BUFFER_LEN) {
    max_chars = UI_EDITOR_BUFFER_LEN - 1U;
  }

  SceWChar16 title_utf16[SCE_IME_DIALOG_MAX_TITLE_LENGTH + 1];
  SceWChar16 initial_utf16[UI_EDITOR_BUFFER_LEN];
  SceWChar16 input_utf16[UI_EDITOR_BUFFER_LEN];
  ui_to_wchar16(field_name, title_utf16, sizeof(title_utf16) / sizeof(title_utf16[0]));
  ui_to_wchar16(value, initial_utf16, sizeof(initial_utf16) / sizeof(initial_utf16[0]));
  ui_to_wchar16(value, input_utf16, sizeof(input_utf16) / sizeof(input_utf16[0]));

  SceImeDialogParam ime_param;
  sceImeDialogParamInit(&ime_param);
  ime_param.supportedLanguages =
      SCE_IME_LANGUAGE_ENGLISH |
      SCE_IME_LANGUAGE_ENGLISH_GB |
      SCE_IME_LANGUAGE_FRENCH |
      SCE_IME_LANGUAGE_GERMAN |
      SCE_IME_LANGUAGE_ITALIAN |
      SCE_IME_LANGUAGE_SPANISH |
      SCE_IME_LANGUAGE_PORTUGUESE |
      SCE_IME_LANGUAGE_PORTUGUESE_BR;
  ime_param.languagesForced = SCE_FALSE;
  ime_param.type = ime_type;
  ime_param.option = SCE_IME_OPTION_NO_ASSISTANCE;
  ime_param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
  ime_param.textBoxMode = textbox_mode;
  ime_param.enterLabel = (ime_type == SCE_IME_TYPE_URL) ? SCE_IME_ENTER_LABEL_GO : SCE_IME_ENTER_LABEL_DEFAULT;
  ime_param.title = title_utf16;
  ime_param.maxTextLength = (SceUInt32)max_chars;
  ime_param.initialText = initial_utf16;
  ime_param.inputTextBuffer = input_utf16;

  int init_status = sceImeDialogInit(&ime_param);
  if (init_status < 0) {
    if (state != NULL) {
      ui_set_status(state, "Cannot open system keyboard: 0x%08X", (unsigned int)init_status);
    }
    app_log_write(APP_LOG_LEVEL_ERROR, "ui", "sceImeDialogInit failed: 0x%08X", (unsigned int)init_status);
    return -1;
  }

  g_common_dialog_active = 1;
  for (;;) {
    ui_pump_app_events();
    if (sceImeDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_FINISHED) {
      break;
    }

    if (state != NULL) {
      ui_render_main_screen(state);
    } else {
      ui_begin_frame();
      ui_end_frame();
    }
    sceKernelDelayThread(16 * 1000);
  }

  SceImeDialogResult ime_result;
  memset(&ime_result, 0, sizeof(ime_result));
  int result_status = sceImeDialogGetResult(&ime_result);
  sceImeDialogTerm();
  g_common_dialog_active = 0;

  if (result_status < 0) {
    app_log_write(APP_LOG_LEVEL_ERROR, "ui", "sceImeDialogGetResult failed: 0x%08X", (unsigned int)result_status);
    if (state != NULL) {
      ui_set_status(state, "System keyboard result failed: 0x%08X", (unsigned int)result_status);
    }
    return -1;
  }

  if (ime_result.button != SCE_IME_DIALOG_BUTTON_ENTER) {
    return 0;
  }

  char edited[UI_EDITOR_BUFFER_LEN];
  ui_from_wchar16(input_utf16, edited, sizeof(edited));
  snprintf(value, value_size, "%s", edited);
  return 1;
}

/*
 * Handles editing of one text field and persists settings on successful edit.
 * URL and username fields can optionally trim accidental surrounding whitespace.
 */
static void ui_edit_config_field(
    UiAppState *state,
    const char *label,
    char *field,
    size_t field_size,
    unsigned int ime_type,
    int password_mode,
    int trim_whitespace) {
  if ((state == NULL) || !has_text(label) || (field == NULL) || (field_size == 0U)) {
    return;
  }

  char previous_value[UI_EDITOR_BUFFER_LEN];
  snprintf(previous_value, sizeof(previous_value), "%s", field);

  unsigned int textbox_mode = password_mode
                                  ? SCE_IME_DIALOG_TEXTBOX_MODE_PASSWORD
                                  : SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
  int edited = ui_edit_text_field(state, label, field, field_size, ime_type, textbox_mode);
  if (edited == 0) {
    ui_set_status(state, "%s edit canceled", label);
    return;
  }
  if (edited < 0) {
    return;
  }

  if (trim_whitespace) {
    ui_trim_ascii_whitespace(field);
  }

  if (strcmp(previous_value, field) == 0) {
    ui_set_status(state, "%s unchanged", label);
    return;
  }

  char success_message[64];
  snprintf(success_message, sizeof(success_message), "%s saved", label);
  if (ui_save_config(state, success_message) == APP_CONFIG_OK) {
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
        SCE_IME_TYPE_URL,
        0,
        1);
    return;
  }

  if (state->selected_index == UI_SELECT_USERNAME) {
    ui_edit_config_field(
        state,
        "Username",
        state->config.romm_username,
        sizeof(state->config.romm_username),
        SCE_IME_TYPE_BASIC_LATIN,
        0,
        1);
    return;
  }

  if (state->selected_index == UI_SELECT_PASSWORD) {
    ui_edit_config_field(
        state,
        "Password",
        state->config.romm_password,
        sizeof(state->config.romm_password),
        SCE_IME_TYPE_BASIC_LATIN,
        1,
        0);
    return;
  }

  if (state->selected_index == UI_SELECT_SYNC_PRIMARY) {
    if (ui_active_game(state) == NULL) {
      ui_set_status(state, "No PS1 game is selected");
      return;
    }

    if (!app_config_has_server_url(&state->config)) {
      ui_set_status(state, "Enter the RoMM server address before synchronizing");
      return;
    }

    if (!app_config_has_auth(&state->config)) {
      ui_set_status(state, "Enter the RoMM username and password before synchronizing");
      return;
    }

    ui_run_sync_for_game(state, state->active_game_index);
    ui_refresh_local_inventory(state);
    ui_clamp_selection(state);
    return;
  }

  if (state->selected_index == UI_SELECT_SYNC_ALL) {
    if (state->local_count <= 0) {
      ui_set_status(state, "No local PS1 saves were detected");
      return;
    }

    if (!app_config_has_server_url(&state->config)) {
      ui_set_status(state, "Enter the RoMM server address before synchronizing");
      return;
    }

    if (!app_config_has_auth(&state->config)) {
      ui_set_status(state, "Enter the RoMM username and password before synchronizing");
      return;
    }

    ui_run_sync_all_saves(state);
    ui_refresh_local_inventory(state);
    ui_clamp_selection(state);
    return;
  }

  if (state->selected_index == UI_SELECT_RESCAN) {
    ui_refresh_local_inventory(state);
    ui_clamp_selection(state);
    return;
  }

  if (state->selected_index >= UI_SELECT_GAME_BASE) {
    int game_index = state->selected_index - UI_SELECT_GAME_BASE;
    state->active_game_index = game_index;
    state->selected_index = UI_SELECT_SYNC_PRIMARY;
    ui_set_status(state, "Selected %s for synchronization", state->games[game_index].game_id);
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
  state->active_game_index = -1;
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
    ui_set_status(state, "Connection settings not found. Enter the server, username, and password.");
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
 * Initializes the Vita2D renderer and the default PGF font.
 */
static int ui_renderer_init(void) {
  int status = vita2d_init();
  if (status < 0) {
    return status;
  }

  g_ui_font = vita2d_load_default_pgf();
  if (g_ui_font == NULL) {
    vita2d_fini();
    return -1;
  }

  return 0;
}

/*
 * Releases Vita2D renderer resources before application exit.
 */
static void ui_renderer_term(void) {
  vita2d_wait_rendering_done();
  if (g_ui_font != NULL) {
    vita2d_free_pgf(g_ui_font);
    g_ui_font = NULL;
  }
  vita2d_fini();
}

/*
 * Shows a one-shot graceful exit screen before terminating the process.
 */
static void ui_render_exit_screen(void) {
  ui_begin_frame();
  ui_draw_panel(220.0f, 214.0f, 520.0f, 112.0f, UI_COLOR_PANEL, UI_COLOR_PANEL_BORDER);
  ui_draw_text_center(UI_SCREEN_WIDTH * 0.5f, 274.0f, UI_COLOR_TEXT, 0.95f, "Exiting RomM Vita Sync...");
  ui_end_frame();
}

/*
 * Entry point for the interactive PS Vita UI flow.
 */
int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
  app_log_clear_history();

  if (ui_renderer_init() < 0) {
    psvDebugScreenInit();
    psvDebugScreenPrintf("Failed to initialize vita2d renderer.\n");
    sceKernelDelayThread(900 * 1000);
    ui_dialog_runtime_term();
    return 1;
  }

  if (ui_dialog_runtime_init() < 0) {
    app_log_write(APP_LOG_LEVEL_WARN, "main", "system keyboard runtime unavailable; text editing will be disabled");
  }
  ui_dialog_init();

  UiAppState *state = &g_app_state;
  ui_initialize_state(state);

  unsigned int previous_buttons = 0U;
  for (;;) {
    ui_pump_app_events();
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
    if (state->selected_index >= UI_SELECT_GAME_BASE) {
      state->active_game_index = state->selected_index - UI_SELECT_GAME_BASE;
    }
    if (pressed & SCE_CTRL_CROSS) {
      ui_activate_selection(state);
      previous_buttons = 0U;
    }

    sceKernelDelayThread(16 * 1000);
  }

  ui_render_exit_screen();
  sceKernelDelayThread(400 * 1000);
  ui_dialog_runtime_term();
  ui_renderer_term();
  return 0;
}
