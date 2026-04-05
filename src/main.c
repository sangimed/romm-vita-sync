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
#define UI_SELECT_FILE_LOGGING 3
#define UI_SELECT_SYNC_PRIMARY 4
#define UI_SELECT_SYNC_ALL 5
#define UI_SELECT_RESCAN 6
#define UI_SELECT_GAME_BASE 7

#define UI_SCREEN_WIDTH 960.0f
#define UI_SCREEN_HEIGHT 544.0f

#define UI_GAME_LIST_VISIBLE 4
#define UI_GAME_ROW_HEIGHT 28.0f
#define UI_LOG_VISIBLE_LINES 3
#define UI_LOG_EXPANDED_VISIBLE_LINES 7
#define UI_SYNC_MODAL_VISIBLE_LINES 12
#define UI_LOG_TOP_PADDING 18.0f
#define UI_LOG_LINE_HEIGHT 18.0f
#define UI_STATUS_LINE_LEN 192
#define UI_EDITOR_BUFFER_LEN 512
#define APP_RUNTIME_DATA_DIRECTORY "ux0:data/romm-vita-sync"
#define APP_RUNTIME_LOG_FILE_PATH "ux0:data/romm-vita-sync/romm-vita-sync.log"

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

#define UI_TEXT_SCALE_BOOST 1.20f
#define UI_TEXT_SCALE_MIN 1.00f
#define UI_TEXT_SCALE_MAX 1.52f

#define UI_NAV_UP 0
#define UI_NAV_DOWN 1
#define UI_NAV_LEFT 2
#define UI_NAV_RIGHT 3

typedef struct UiGameEntry {
  char key[ROMM_GAME_ID_LEN];
  char game_id[ROMM_GAME_ID_LEN];
  char title[ROMM_GAME_TITLE_LEN];
  int save_count;
} UiGameEntry;

typedef enum UiSyncTrigger {
  UI_SYNC_TRIGGER_MANUAL = 0,
  UI_SYNC_TRIGGER_AUTOMATIC = 1
} UiSyncTrigger;

typedef struct UiSyncFeedback {
  int running;
  int completed;
  int success;
  int sync_status;
  int completed_units;
  int total_units;
  int modal_log_scroll;
  int modal_auto_scroll;
  int persistent_logs_expanded;
  UiSyncTrigger trigger;
  char title[64];
  char message[UI_STATUS_LINE_LEN];
  char context[UI_STATUS_LINE_LEN];
} UiSyncFeedback;

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
  UiSyncFeedback sync_feedback;
  int pending_auto_sync;
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
 * Applies logging preferences (level + optional file logging) from config.
 */
static void ui_apply_logging_preferences(const AppConfig *config) {
  if (config == NULL) {
    app_log_set_level(APP_LOG_LEVEL_INFO);
    app_log_set_file_output(0, APP_RUNTIME_LOG_FILE_PATH);
    return;
  }

  app_log_set_level(app_log_level_from_config(config->log_level));
  app_log_set_file_output(config->log_file_enabled, APP_RUNTIME_LOG_FILE_PATH);
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
 * Clamps one integer into a closed [min_value, max_value] range.
 */
static int clamp_int(int value, int min_value, int max_value) {
  if (value < min_value) {
    return min_value;
  }
  if (value > max_value) {
    return max_value;
  }
  return value;
}

/*
 * Resets sync feedback state before a new manual or automatic run starts.
 */
static void ui_sync_feedback_reset(
    UiSyncFeedback *feedback,
    UiSyncTrigger trigger,
    const char *title,
    const char *context) {
  if (feedback == NULL) {
    return;
  }

  memset(feedback, 0, sizeof(*feedback));
  feedback->running = 1;
  feedback->trigger = trigger;
  feedback->total_units = 1;
  feedback->modal_auto_scroll = 1;
  feedback->persistent_logs_expanded = 0;
  snprintf(feedback->title, sizeof(feedback->title), "%s", has_text(title) ? title : "Synchronization");
  snprintf(feedback->context, sizeof(feedback->context), "%s", has_text(context) ? context : "");
  snprintf(feedback->message, sizeof(feedback->message), "Preparing synchronization...");
}

/*
 * Updates the short sync feedback message shown near the progress bar.
 */
static void ui_sync_feedback_set_message(UiSyncFeedback *feedback, const char *message) {
  if (feedback == NULL) {
    return;
  }

  snprintf(
      feedback->message,
      sizeof(feedback->message),
      "%s",
      has_text(message) ? message : "");
}

/*
 * Updates sync feedback progress counters with defensive clamping.
 */
static void ui_sync_feedback_set_progress(UiSyncFeedback *feedback, int completed_units, int total_units) {
  if (feedback == NULL) {
    return;
  }

  if (total_units <= 0) {
    total_units = 1;
  }

  feedback->total_units = total_units;
  feedback->completed_units = clamp_int(completed_units, 0, total_units);
}

/*
 * Writes one sync log message through the app logger.
 * This keeps sync logging transport independent from UI rendering.
 */
static void ui_sync_log_write(AppLogLevel level, const char *format, ...) {
  if (format == NULL) {
    return;
  }

  char message[256];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);

  app_log_write(level, "sync-ui", "%s", message);
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
 * Returns a stable 2D anchor (screen-like coordinates) for one selectable item.
 * Game list items use a virtual y-coordinate so directional navigation still works
 * even when the item is currently outside the visible window.
 */
static int ui_get_selection_anchor(const UiAppState *state, int index, float *out_x, float *out_y) {
  if ((state == NULL) || (out_x == NULL) || (out_y == NULL)) {
    return -1;
  }

  if (index == UI_SELECT_SERVER_URL) {
    *out_x = 48.0f + (398.0f * 0.5f);
    *out_y = 154.0f + (44.0f * 0.5f);
    return 0;
  }
  if (index == UI_SELECT_USERNAME) {
    *out_x = 48.0f + (398.0f * 0.5f);
    *out_y = 208.0f + (44.0f * 0.5f);
    return 0;
  }
  if (index == UI_SELECT_PASSWORD) {
    *out_x = 48.0f + (398.0f * 0.5f);
    *out_y = 262.0f + (44.0f * 0.5f);
    return 0;
  }
  if (index == UI_SELECT_FILE_LOGGING) {
    *out_x = 48.0f + (398.0f * 0.5f);
    *out_y = 316.0f + (28.0f * 0.5f);
    return 0;
  }
  if (index == UI_SELECT_SYNC_PRIMARY) {
    *out_x = 494.0f + (418.0f * 0.5f);
    *out_y = 252.0f + (28.0f * 0.5f);
    return 0;
  }
  if (index == UI_SELECT_SYNC_ALL) {
    *out_x = 494.0f + (418.0f * 0.5f);
    *out_y = 284.0f + (28.0f * 0.5f);
    return 0;
  }
  if (index == UI_SELECT_RESCAN) {
    *out_x = 494.0f + (418.0f * 0.5f);
    *out_y = 316.0f + (28.0f * 0.5f);
    return 0;
  }

  if (index >= UI_SELECT_GAME_BASE) {
    int game_index = index - UI_SELECT_GAME_BASE;
    if ((game_index < 0) || (game_index >= state->game_count)) {
      return -1;
    }

    *out_x = 48.0f + (516.0f * 0.5f);
    *out_y = 390.0f + (UI_GAME_ROW_HEIGHT * (float)game_index) + (UI_GAME_ROW_HEIGHT * 0.5f);
    return 0;
  }

  return -1;
}

/*
 * Moves selection in one directional axis by choosing the nearest candidate item
 * that lies in the requested direction.
 */
static int ui_move_selection_direction(UiAppState *state, int direction) {
  if (state == NULL) {
    return 0;
  }

  int total = ui_total_selectable_entries(state);
  if (total <= 0) {
    return 0;
  }

  float current_x = 0.0f;
  float current_y = 0.0f;
  if (ui_get_selection_anchor(state, state->selected_index, &current_x, &current_y) < 0) {
    return 0;
  }

  int best_index = -1;
  float best_primary = 0.0f;
  float best_secondary = 0.0f;

  for (int i = 0; i < total; ++i) {
    if (i == state->selected_index) {
      continue;
    }

    float candidate_x = 0.0f;
    float candidate_y = 0.0f;
    if (ui_get_selection_anchor(state, i, &candidate_x, &candidate_y) < 0) {
      continue;
    }

    float dx = candidate_x - current_x;
    float dy = candidate_y - current_y;

    int valid = 0;
    float primary = 0.0f;
    float secondary = 0.0f;
    if (direction == UI_NAV_UP) {
      if (dy < -0.5f) {
        valid = 1;
        primary = -dy;
        secondary = fabsf(dx);
      }
    } else if (direction == UI_NAV_DOWN) {
      if (dy > 0.5f) {
        valid = 1;
        primary = dy;
        secondary = fabsf(dx);
      }
    } else if (direction == UI_NAV_LEFT) {
      if (dx < -0.5f) {
        valid = 1;
        primary = -dx;
        secondary = fabsf(dy);
      }
    } else if (direction == UI_NAV_RIGHT) {
      if (dx > 0.5f) {
        valid = 1;
        primary = dx;
        secondary = fabsf(dy);
      }
    }

    if (!valid) {
      continue;
    }

    if ((best_index < 0) ||
        (primary < best_primary) ||
        ((fabsf(primary - best_primary) < 0.01f) && (secondary < best_secondary))) {
      best_index = i;
      best_primary = primary;
      best_secondary = secondary;
    }
  }

  if (best_index < 0) {
    return 0;
  }

  state->selected_index = best_index;
  if (state->selected_index >= UI_SELECT_GAME_BASE) {
    state->active_game_index = state->selected_index - UI_SELECT_GAME_BASE;
  }
  return 1;
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
  ui_apply_logging_preferences(&state->config);
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
 * Applies a readability-oriented text scale policy for Vita's display.
 * Tiny scales are lifted to avoid hard-to-read pixelated labels.
 */
static float ui_resolve_text_scale(float scale) {
  float resolved = scale * UI_TEXT_SCALE_BOOST;
  if (resolved < UI_TEXT_SCALE_MIN) {
    resolved = UI_TEXT_SCALE_MIN;
  }
  if (resolved > UI_TEXT_SCALE_MAX) {
    resolved = UI_TEXT_SCALE_MAX;
  }
  return resolved;
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

  float draw_scale = ui_resolve_text_scale(scale);
  vita2d_pgf_draw_text(g_ui_font, ui_snap_to_pixel(x), ui_snap_to_pixel(y), color, draw_scale, line);
}

/*
 * Estimates text width to center short labels in panels.
 */
static float ui_estimate_text_width(const char *text, float scale) {
  if (!has_text(text)) {
    return 0.0f;
  }
  return (float)strlen(text) * 11.0f * ui_resolve_text_scale(scale);
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
 * Draws a compact progress bar using the current accent palette.
 */
static void ui_draw_progress_bar(float x, float y, float w, float h, float ratio) {
  if ((w <= 0.0f) || (h <= 0.0f)) {
    return;
  }

  if (ratio < 0.0f) {
    ratio = 0.0f;
  }
  if (ratio > 1.0f) {
    ratio = 1.0f;
  }

  ui_draw_panel(x, y, w, h, UI_COLOR_FIELD, UI_COLOR_PANEL_BORDER);
  float fill_width = (w - 2.0f) * ratio;
  if (fill_width < 0.0f) {
    fill_width = 0.0f;
  }

  if (fill_width > 0.0f) {
    vita2d_draw_rectangle(
        ui_snap_to_pixel(x + 1.0f),
        ui_snap_to_pixel(y + 1.0f),
        ui_snap_to_pixel(fill_width),
        ui_snap_to_pixel(h - 2.0f),
        UI_COLOR_ACCENT);
  }
}

/*
 * Picks one text color for a rendered log line based on its level prefix.
 */
static unsigned int ui_log_line_color(const char *line) {
  if (!has_text(line)) {
    return UI_COLOR_TEXT_DIM;
  }

  if (strncmp(line, "[ERROR]", 7) == 0) {
    return UI_COLOR_DANGER;
  }
  if (strncmp(line, "[WARN]", 6) == 0) {
    return UI_COLOR_WARNING;
  }
  if (strncmp(line, "[INFO]", 6) == 0) {
    return UI_COLOR_TEXT;
  }
  return UI_COLOR_TEXT_MUTED;
}

/*
 * Draws one scrollable log viewport from global app_log history.
 */
static void ui_draw_log_viewport(
    float x,
    float y,
    float w,
    float h,
    int start_index,
    int visible_lines) {
  ui_draw_panel(x, y, w, h, UI_COLOR_PANEL_ALT, UI_COLOR_PANEL_BORDER);

  if (visible_lines <= 0) {
    return;
  }

  int total = app_log_history_count();
  int max_start = total - visible_lines;
  if (max_start < 0) {
    max_start = 0;
  }

  int start = clamp_int(start_index, 0, max_start);
  int end = start + visible_lines;
  if (end > total) {
    end = total;
  }

  float line_y = y + UI_LOG_TOP_PADDING;
  for (int i = start; i < end; ++i) {
    const char *line = app_log_history_line(i);
    if (!has_text(line)) {
      continue;
    }

    char clipped[UI_STATUS_LINE_LEN];
    ui_truncate_text(line, clipped, sizeof(clipped));
    ui_draw_text(x + 10.0f, line_y, ui_log_line_color(line), 0.66f, "%s", clipped);
    line_y += UI_LOG_LINE_HEIGHT;
  }
}

/*
 * Draws one editable field row with label/value hierarchy.
 */
static void ui_draw_field_row(float x, float y, float w, float h, int selected, const char *label, const char *value) {
  unsigned int fill = selected ? UI_COLOR_FIELD_ACTIVE : UI_COLOR_FIELD;
  unsigned int border = selected ? UI_COLOR_PANEL_BORDER_ACTIVE : UI_COLOR_PANEL_BORDER;
  unsigned int value_color = has_text(value) ? UI_COLOR_TEXT : UI_COLOR_TEXT_DIM;
  float label_scale = (h < 36.0f) ? 0.68f : 0.78f;
  float value_scale = (h < 36.0f) ? 0.80f : 0.92f;
  float label_y = y + ((h < 36.0f) ? 11.0f : 17.0f);
  float value_y = y + ((h < 36.0f) ? 24.0f : 39.0f);

  ui_draw_panel(x, y, w, h, fill, border);
  if (selected) {
    vita2d_draw_rectangle(ui_snap_to_pixel(x), ui_snap_to_pixel(y), 4.0f, ui_snap_to_pixel(h), UI_COLOR_ACCENT);
  }

  ui_draw_text(x + 16.0f, label_y, UI_COLOR_TEXT_DIM, label_scale, "%s", label);
  ui_draw_text(x + 16.0f, value_y, value_color, value_scale, "%s", value);
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

  ui_draw_text(x + 12.0f, y + 17.0f, title_color, 0.82f, "%s", title);
  ui_draw_text(
      x + w - 106.0f,
      y + 17.0f,
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
  ui_draw_text(32.0f, 70.0f, UI_COLOR_TEXT_MUTED, 0.80f, "Configure RoMM access, choose a PS1 game, then run manual or startup auto sync.");

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
  char file_log_display[24];
  ui_format_field_display(state->config.romm_url, 0, url_display, sizeof(url_display));
  ui_format_field_display(state->config.romm_username, 0, user_display, sizeof(user_display));
  ui_format_field_display(state->config.romm_password, 1, pass_display, sizeof(pass_display));
  snprintf(file_log_display, sizeof(file_log_display), "%s", state->config.log_file_enabled ? "Enabled" : "Disabled");

  ui_draw_panel(32.0f, 88.0f, 430.0f, 258.0f, UI_COLOR_PANEL, UI_COLOR_PANEL_BORDER);
  ui_draw_text(48.0f, 118.0f, UI_COLOR_TEXT, 0.96f, "Connection");
  ui_draw_text(48.0f, 140.0f, UI_COLOR_TEXT_MUTED, 0.78f, "X: edit/toggle selected field (auto-save).");

  ui_draw_field_row(48.0f, 154.0f, 398.0f, 44.0f, state->selected_index == UI_SELECT_SERVER_URL, "RoMM server address", url_display);
  ui_draw_field_row(48.0f, 208.0f, 398.0f, 44.0f, state->selected_index == UI_SELECT_USERNAME, "RoMM username", user_display);
  ui_draw_field_row(48.0f, 262.0f, 398.0f, 44.0f, state->selected_index == UI_SELECT_PASSWORD, "RoMM password", pass_display);
  ui_draw_field_row(
      48.0f,
      316.0f,
      398.0f,
      28.0f,
      state->selected_index == UI_SELECT_FILE_LOGGING,
      "File logging (10MB x3)",
      file_log_display);
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
  ui_draw_text(494.0f, 140.0f, UI_COLOR_TEXT_MUTED, 0.78f, "Choose the target game in the list below.");
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

  ui_draw_panel(32.0f, 354.0f, 548.0f, 132.0f, UI_COLOR_PANEL, UI_COLOR_PANEL_BORDER);
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

  ui_draw_text(404.0f, 378.0f, UI_COLOR_TEXT_DIM, 0.72f, "Showing %d-%d of %d", start + 1, end, state->game_count);

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
        516.0f,
        UI_GAME_ROW_HEIGHT,
        state->selected_index == (UI_SELECT_GAME_BASE + i),
        state->active_game_index == i,
        row_title,
        game->save_count);
    row_y += UI_GAME_ROW_HEIGHT;
  }
}

/*
 * Renders persistent sync progress and tail logs in the main layout.
 * Expanded mode opens an anchored dropdown with additional recent log lines.
 */
static void ui_render_sync_activity_panel(const UiAppState *state) {
  if (state == NULL) {
    return;
  }

  const UiSyncFeedback *feedback = &state->sync_feedback;
  float ratio = 0.0f;
  if (feedback->total_units > 0) {
    ratio = (float)feedback->completed_units / (float)feedback->total_units;
  }

  const char *trigger_text = (feedback->trigger == UI_SYNC_TRIGGER_AUTOMATIC) ? "Auto" : "Manual";
  const char *state_text = "Idle";
  unsigned int state_color = UI_COLOR_TEXT_DIM;
  if (feedback->running) {
    state_text = "Running";
    state_color = UI_COLOR_ACCENT;
  } else if (feedback->completed) {
    state_text = feedback->success ? "Completed" : "Failed";
    state_color = feedback->success ? UI_COLOR_SUCCESS : UI_COLOR_DANGER;
  }

  ui_draw_panel(592.0f, 354.0f, 336.0f, 132.0f, UI_COLOR_PANEL, UI_COLOR_PANEL_BORDER);
  ui_draw_text(608.0f, 378.0f, UI_COLOR_TEXT, 0.88f, "Sync Activity");
  ui_draw_text(608.0f, 396.0f, UI_COLOR_TEXT_MUTED, 0.68f, "%s | %s", trigger_text, state_text);
  ui_draw_text(856.0f, 396.0f, state_color, 0.68f, "%d%%", (int)(ratio * 100.0f));

  ui_draw_progress_bar(608.0f, 402.0f, 304.0f, 14.0f, ratio);

  char message[UI_STATUS_LINE_LEN];
  ui_truncate_text(feedback->message, message, sizeof(message));
  ui_draw_text(608.0f, 428.0f, UI_COLOR_TEXT_MUTED, 0.66f, "%s", has_text(message) ? message : "No sync activity yet.");

  int total_logs = app_log_history_count();
  int tail_start = total_logs - UI_LOG_VISIBLE_LINES;
  if (tail_start < 0) {
    tail_start = 0;
  }
  ui_draw_log_viewport(608.0f, 432.0f, 304.0f, 48.0f, tail_start, UI_LOG_VISIBLE_LINES);
  ui_draw_text(
      608.0f,
      480.0f,
      UI_COLOR_TEXT_DIM,
      0.62f,
      feedback->persistent_logs_expanded ? "SQUARE hide logs" : "SQUARE show logs");

  if (!feedback->persistent_logs_expanded) {
    return;
  }

  int expanded_start = total_logs - UI_LOG_EXPANDED_VISIBLE_LINES;
  if (expanded_start < 0) {
    expanded_start = 0;
  }

  ui_draw_panel(592.0f, 166.0f, 336.0f, 180.0f, UI_COLOR_PANEL, UI_COLOR_PANEL_BORDER_ACTIVE);
  ui_draw_text(608.0f, 188.0f, UI_COLOR_TEXT, 0.76f, "Recent Sync Logs");
  ui_draw_log_viewport(608.0f, 194.0f, 304.0f, 146.0f, expanded_start, UI_LOG_EXPANDED_VISIBLE_LINES);
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
  ui_draw_text(498.0f, 522.0f, UI_COLOR_TEXT_MUTED, 0.72f, "D-Pad move   X apply   SQUARE logs   START quit");
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
  ui_render_sync_activity_panel(state);
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
 * Appends a concise sync summary and action tail to the shared sync logger.
 */
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
    ui_sync_log_write(
        level,
        "Action %02d: %s %s %s (%s)",
        i + 1,
        sync_slot_str(action->slot),
        sync_action_type_str(action->action),
        action->executed ? "executed" : "planned",
        action->reason);
  }

  if (report->action_count > render_count) {
    ui_sync_log_write(APP_LOG_LEVEL_INFO, "... %d additional action(s) omitted", report->action_count - render_count);
  }
}

/*
 * Renders the blocking manual-sync modal with progress and scrolling log area.
 */
static void ui_render_sync_modal(UiAppState *state) {
  if (state == NULL) {
    return;
  }

  UiSyncFeedback *feedback = &state->sync_feedback;
  float ratio = 0.0f;
  if (feedback->total_units > 0) {
    ratio = (float)feedback->completed_units / (float)feedback->total_units;
  }

  ui_begin_frame();
  ui_draw_panel(96.0f, 30.0f, 768.0f, 484.0f, UI_COLOR_PANEL, UI_COLOR_PANEL_BORDER_ACTIVE);
  ui_draw_text(124.0f, 60.0f, UI_COLOR_TEXT, 0.96f, "%s", has_text(feedback->title) ? feedback->title : "Synchronization");
  if (has_text(feedback->context)) {
    ui_draw_text(124.0f, 82.0f, UI_COLOR_TEXT_MUTED, 0.74f, "%s", feedback->context);
  }

  ui_draw_progress_bar(124.0f, 98.0f, 712.0f, 18.0f, ratio);
  ui_draw_text(
      124.0f,
      134.0f,
      UI_COLOR_TEXT_MUTED,
      0.72f,
      "%d%% (%d/%d)",
      (int)(ratio * 100.0f),
      feedback->completed_units,
      feedback->total_units);

  int total_logs = app_log_history_count();
  int max_scroll = total_logs - UI_SYNC_MODAL_VISIBLE_LINES;
  if (max_scroll < 0) {
    max_scroll = 0;
  }

  if (feedback->running || feedback->modal_auto_scroll) {
    feedback->modal_log_scroll = max_scroll;
  }
  feedback->modal_log_scroll = clamp_int(feedback->modal_log_scroll, 0, max_scroll);

  ui_draw_text(124.0f, 160.0f, UI_COLOR_TEXT, 0.82f, "Live logs");
  ui_draw_log_viewport(
      124.0f,
      166.0f,
      712.0f,
      230.0f,
      feedback->modal_log_scroll,
      UI_SYNC_MODAL_VISIBLE_LINES);

  if (max_scroll > 0) {
    int visible_end = feedback->modal_log_scroll + UI_SYNC_MODAL_VISIBLE_LINES;
    if (visible_end > total_logs) {
      visible_end = total_logs;
    }
    ui_draw_text(
        124.0f,
        416.0f,
        UI_COLOR_TEXT_DIM,
        0.64f,
        "UP/DOWN scroll %d-%d/%d",
        feedback->modal_log_scroll + 1,
        visible_end,
        total_logs);
  }

  unsigned int result_color = UI_COLOR_TEXT_MUTED;
  if (feedback->running) {
    result_color = UI_COLOR_ACCENT;
  } else if (feedback->success) {
    result_color = UI_COLOR_SUCCESS;
  } else {
    result_color = UI_COLOR_DANGER;
  }

  ui_draw_text(124.0f, 442.0f, result_color, 0.80f, "%s", has_text(feedback->message) ? feedback->message : "");
  ui_draw_text(
      124.0f,
      486.0f,
      UI_COLOR_TEXT_DIM,
      0.70f,
      feedback->running ? "Synchronization is running..." : "O/X/START: close");
  ui_end_frame();
}

/*
 * Pumps one UI frame while sync is running so logs/progress update live.
 * Manual runs render a blocking modal, automatic runs keep the main layout visible.
 */
static void ui_sync_render_live(UiAppState *state) {
  if (state == NULL) {
    return;
  }

  ui_pump_app_events();
  if (state->sync_feedback.trigger == UI_SYNC_TRIGGER_MANUAL) {
    ui_render_sync_modal(state);
  } else {
    ui_render_main_screen(state);
  }
}

typedef struct UiSyncProgressBridge {
  UiAppState *state;
  int base_completed_units;
  int overall_total_units;
} UiSyncProgressBridge;

/*
 * Bridges sync_engine progress callbacks into UI progress + live redraw.
 */
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
  feedback->modal_auto_scroll = 1;
  ui_sync_render_live(state);
}

/*
 * Keeps the completed manual sync modal open until the user closes it.
 */
static void ui_present_completed_manual_sync(UiAppState *state) {
  if (state == NULL) {
    return;
  }

  unsigned int previous_buttons = 0U;
  for (;;) {
    ui_pump_app_events();
    ui_render_sync_modal(state);

    unsigned int pressed = ui_poll_pressed(&previous_buttons);
    if ((pressed & SCE_CTRL_UP) && (state->sync_feedback.modal_log_scroll > 0)) {
      state->sync_feedback.modal_auto_scroll = 0;
      state->sync_feedback.modal_log_scroll -= 1;
    }
    if (pressed & SCE_CTRL_DOWN) {
      int total_logs = app_log_history_count();
      int max_scroll = total_logs - UI_SYNC_MODAL_VISIBLE_LINES;
      if (max_scroll < 0) {
        max_scroll = 0;
      }
      if (state->sync_feedback.modal_log_scroll < max_scroll) {
        state->sync_feedback.modal_auto_scroll = 0;
        state->sync_feedback.modal_log_scroll += 1;
      }
    }
    if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_CROSS | SCE_CTRL_START)) {
      return;
    }

    sceKernelDelayThread(16 * 1000);
  }
}

/*
 * Runs one sync pipeline and routes feedback to modal or persistent area.
 * Progress uses real steps: validate, register device, map rom IDs, engine run.
 */
static int ui_run_sync_pipeline(
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

  int engine_units = work_item_count + 1;
  if (engine_units < 1) {
    engine_units = 1;
  }
  int total_units = 3 + engine_units;
  ui_sync_feedback_set_progress(&state->sync_feedback, 0, total_units);
  ui_sync_render_live(state);

  ui_sync_log_write(APP_LOG_LEVEL_INFO, "Scanning local saves...");
  int preview_count = work_item_count;
  if (preview_count > 24) {
    preview_count = 24;
  }
  for (int i = 0; i < preview_count; ++i) {
    const SyncSaveDescriptor *item = &work_items[i];
    const char *name = has_text(item->filename) ? item->filename : item->path;
    ui_sync_log_write(APP_LOG_LEVEL_INFO, "Save detected: %s", has_text(name) ? name : "(unknown)");
  }
  if (work_item_count > preview_count) {
    ui_sync_log_write(APP_LOG_LEVEL_INFO, "... %d more local save(s) omitted", work_item_count - preview_count);
  }

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
    ui_sync_log_write(APP_LOG_LEVEL_ERROR, "Sync failed: credentials are missing");
    ui_sync_feedback_set_message(&state->sync_feedback, "Sync failed: credentials are missing");
    state->sync_feedback.running = 0;
    state->sync_feedback.completed = 1;
    state->sync_feedback.success = 0;
    state->sync_feedback.sync_status = SYNC_ENGINE_ERR_INVALID_ARGUMENT;
    ui_sync_feedback_set_progress(&state->sync_feedback, total_units, total_units);
    ui_sync_render_live(state);
    return SYNC_ENGINE_ERR_INVALID_ARGUMENT;
  }
  ui_sync_feedback_set_progress(&state->sync_feedback, 1, total_units);
  ui_sync_log_write(APP_LOG_LEVEL_INFO, "Credentials validated");
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
    ui_sync_log_write(
        APP_LOG_LEVEL_ERROR,
        "Sync failed: rom mapping error (%s)",
        romm_client_status_str(mapped_count));
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
  ui_sync_feedback_set_progress(&state->sync_feedback, 3, total_units);
  ui_sync_render_live(state);

  if (state->config.sync_dry_run) {
    ui_sync_log_write(APP_LOG_LEVEL_INFO, "Dry-run enabled: transfers will not execute");
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
  config.progress_callback = ui_sync_engine_progress_callback;
  config.progress_user_data = &progress_bridge;

  int sync_status = sync_engine_run(
      &config,
      work_items,
      work_item_count,
      &state->romm_client,
      &state->sync_report);

  state->sync_feedback.running = 0;
  state->sync_feedback.completed = 1;
  state->sync_feedback.sync_status = sync_status;
  state->sync_feedback.success = (sync_status == SYNC_ENGINE_OK);
  ui_sync_feedback_set_progress(&state->sync_feedback, total_units, total_units);

  if (sync_status == SYNC_ENGINE_OK) {
    ui_sync_log_write(APP_LOG_LEVEL_INFO, "Sync completed");
    ui_sync_append_report_logs(&state->sync_report);
    ui_sync_feedback_set_message(&state->sync_feedback, "Sync completed successfully.");
  } else {
    ui_sync_log_write(
        APP_LOG_LEVEL_ERROR,
        "Sync failed: %s (%d)",
        sync_engine_status_str(sync_status),
        sync_status);
    ui_sync_feedback_set_message(&state->sync_feedback, "Sync failed.");
  }

  ui_sync_render_live(state);
  return sync_status;
}

/*
 * Runs synchronization for one selected game using the manual modal feedback path.
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

  char confirm_msg[256];
  snprintf(
      confirm_msg,
      sizeof(confirm_msg),
      "Synchronize %s?\n%d save card(s) will be checked.",
      has_text(game->title) ? game->title : game->game_id,
      game_item_count);
  if (ui_dialog_confirm(confirm_msg) != 1) {
    ui_set_status(state, "Sync canceled for %s", game->game_id);
    return;
  }

  if (game_item_count <= 0) {
    ui_set_status(state, "No local save found for %s", game->game_id);
    return;
  }

  char context[UI_STATUS_LINE_LEN];
  snprintf(
      context,
      sizeof(context),
      "Game: %s (%d save card%s)",
      game->game_id,
      game_item_count,
      (game_item_count == 1) ? "" : "s");

  int sync_status = ui_run_sync_pipeline(
      state,
      state->sync_work_items,
      game_item_count,
      UI_SYNC_TRIGGER_MANUAL,
      "Manual Synchronization",
      context);

  if (sync_status == SYNC_ENGINE_OK) {
    ui_set_status(
        state,
        "Sync finished for %s (uploads=%d, downloads=%d, errors=%d)",
        game->game_id,
        state->sync_report.uploads_executed,
        state->sync_report.downloads_executed,
        state->sync_report.transfer_errors);
  } else {
    ui_set_status(state, "Sync failed for %s", game->game_id);
  }

  ui_present_completed_manual_sync(state);
}

/*
 * Runs one full local-inventory synchronization using the manual modal path.
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

  char context[UI_STATUS_LINE_LEN];
  snprintf(
      context,
      sizeof(context),
      "All games: %d save card%s across %d game(s)",
      work_item_count,
      (work_item_count == 1) ? "" : "s",
      state->game_count);

  int sync_status = ui_run_sync_pipeline(
      state,
      state->sync_work_items,
      work_item_count,
      UI_SYNC_TRIGGER_MANUAL,
      "Manual Synchronization",
      context);

  if (sync_status == SYNC_ENGINE_OK) {
    ui_set_status(
        state,
        "Sync finished for all games (uploads=%d, downloads=%d, errors=%d)",
        state->sync_report.uploads_executed,
        state->sync_report.downloads_executed,
        state->sync_report.transfer_errors);
  } else {
    ui_set_status(state, "Sync failed for all games");
  }

  ui_present_completed_manual_sync(state);
}

/*
 * Runs one optional startup auto-sync using background feedback routing.
 */
static void ui_run_pending_auto_sync(UiAppState *state) {
  if ((state == NULL) || !state->pending_auto_sync) {
    return;
  }

  state->pending_auto_sync = 0;

  if (state->local_count <= 0) {
    ui_set_status(state, "Auto sync skipped: no local PS1 saves detected");
    return;
  }

  if (!app_config_has_server_url(&state->config) || !app_config_has_auth(&state->config)) {
    ui_set_status(state, "Auto sync skipped: configure RomM URL and credentials first");
    return;
  }

  int work_item_count = state->local_count;
  if (work_item_count > (int)(sizeof(state->sync_work_items) / sizeof(state->sync_work_items[0]))) {
    work_item_count = (int)(sizeof(state->sync_work_items) / sizeof(state->sync_work_items[0]));
  }
  memcpy(state->sync_work_items, state->local_items, sizeof(state->sync_work_items[0]) * (size_t)work_item_count);

  char context[UI_STATUS_LINE_LEN];
  snprintf(
      context,
      sizeof(context),
      "Startup auto sync: %d save card%s",
      work_item_count,
      (work_item_count == 1) ? "" : "s");

  int sync_status = ui_run_sync_pipeline(
      state,
      state->sync_work_items,
      work_item_count,
      UI_SYNC_TRIGGER_AUTOMATIC,
      "Automatic Synchronization",
      context);

  if (sync_status == SYNC_ENGINE_OK) {
    ui_set_status(
        state,
        "Auto sync finished (uploads=%d, downloads=%d, errors=%d)",
        state->sync_report.uploads_executed,
        state->sync_report.downloads_executed,
        state->sync_report.transfer_errors);
  } else {
    ui_set_status(state, "Auto sync failed");
  }

  ui_refresh_local_inventory(state);
  ui_clamp_selection(state);
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

  if (state->selected_index == UI_SELECT_FILE_LOGGING) {
    state->config.log_file_enabled = state->config.log_file_enabled ? 0 : 1;
    const char *message = state->config.log_file_enabled
                              ? "File logging enabled"
                              : "File logging disabled";
    int save_status = ui_save_config(state, message);
    if (save_status == APP_CONFIG_OK) {
      app_log_write(
          APP_LOG_LEVEL_INFO,
          "ui",
          "file logging %s (%s)",
          state->config.log_file_enabled ? "enabled" : "disabled",
          APP_RUNTIME_LOG_FILE_PATH);
    }
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
  ui_apply_logging_preferences(&state->config);

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

  state->romm_client.context = &state->config;
  state->romm_client.list_remote_saves = romm_http_list_remote_saves_callback;
  state->romm_client.upload_save = romm_http_upload_save_callback;
  state->romm_client.download_save = romm_http_download_save_callback;
  state->romm_client.register_device = romm_http_register_device_callback;

  ui_sync_feedback_reset(&state->sync_feedback, UI_SYNC_TRIGGER_MANUAL, "Synchronization", "");
  state->sync_feedback.running = 0;
  state->sync_feedback.completed = 0;
  state->sync_feedback.success = 0;
  state->sync_feedback.total_units = 1;
  state->sync_feedback.completed_units = 0;
  ui_sync_feedback_set_message(&state->sync_feedback, "No sync activity yet.");

  ui_refresh_local_inventory(state);
  state->pending_auto_sync = state->config.sync_auto_on_startup ? 1 : 0;
  if (state->pending_auto_sync) {
    ui_set_status(state, "Automatic startup sync is queued");
  }
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
    ui_run_pending_auto_sync(state);
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
      if (!ui_move_selection_direction(state, UI_NAV_UP)) {
        state->selected_index--;
        if (state->selected_index < 0) {
          state->selected_index = total_entries - 1;
        }
      }
    }
    if (pressed & SCE_CTRL_DOWN) {
      if (!ui_move_selection_direction(state, UI_NAV_DOWN)) {
        state->selected_index++;
        if (state->selected_index >= total_entries) {
          state->selected_index = 0;
        }
      }
    }
    if (pressed & SCE_CTRL_LEFT) {
      ui_move_selection_direction(state, UI_NAV_LEFT);
    }
    if (pressed & SCE_CTRL_RIGHT) {
      ui_move_selection_direction(state, UI_NAV_RIGHT);
    }
    if (pressed & SCE_CTRL_SQUARE) {
      state->sync_feedback.persistent_logs_expanded = !state->sync_feedback.persistent_logs_expanded;
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
