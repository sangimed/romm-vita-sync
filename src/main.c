#include <psp2/ctrl.h>
#include <psp2/ime_dialog.h>
#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/touch.h>

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <vita2d.h>

#include "debugScreen.h"

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
#include "ui_dialogs.h"

#define UI_SELECT_SERVER_URL 0
#define UI_SELECT_USERNAME 1
#define UI_SELECT_PASSWORD 2
#define UI_SELECT_DRY_RUN 3
#define UI_SELECT_SYNC_PRIMARY 4
#define UI_SELECT_SYNC_ALL 5
#define UI_SELECT_RESCAN 6
#define UI_SELECT_GAME_BASE 7

#define UI_SCREEN_WIDTH 960.0f
#define UI_SCREEN_HEIGHT 544.0f

#define UI_GAME_LIST_VISIBLE 4
#define UI_GAME_ROW_HEIGHT 28.0f
#define UI_LOG_TOP_PADDING 18.0f
#define UI_LOG_BOTTOM_PADDING 10.0f
#define UI_LOG_LINE_HEIGHT 18.0f
#define UI_LOG_TEXT_SCALE 0.66f
#define UI_WRAP_BUFFER_LEN 384
#define UI_WRAP_MAX_LINES_PER_BLOCK 48
#define UI_STATUS_LINE_LEN 192
#define UI_EDITOR_BUFFER_LEN 512
#define UI_SCROLL_REPEAT_DELAY_FRAMES 18
#define UI_SCROLL_REPEAT_INTERVAL_FRAMES 3
#define UI_TOUCH_DRAG_DEADZONE 4.0f
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

#define UI_TEXT_SCALE_BOOST 1.14f
#define UI_TEXT_SCALE_MIN 0.82f
#define UI_TEXT_SCALE_MAX 1.46f
#define UI_TEXT_SCALE_STEP 0.125f
#define UI_TEXT_SHADOW_OFFSET_X 1.0f
#define UI_TEXT_SHADOW_OFFSET_Y 1.0f
#define UI_TEXT_SHADOW_ALPHA_MIN 32U
#define UI_TEXT_SHADOW_ALPHA_MAX 96U

#define UI_NAV_UP 0
#define UI_NAV_DOWN 1
#define UI_NAV_LEFT 2
#define UI_NAV_RIGHT 3
#define UI_NAV_NONE -1

#define UI_ANALOG_CENTER 127
#define UI_ANALOG_DEADZONE 40
#define UI_NAV_REPEAT_DELAY_FRAMES 14
#define UI_NAV_REPEAT_INTERVAL_FRAMES 4

typedef struct UiControllerState {
  unsigned int buttons;
  unsigned char left_x;
  unsigned char left_y;
} UiControllerState;

typedef struct UiGameEntry {
  char key[ROMM_GAME_ID_LEN];
  char game_id[ROMM_GAME_ID_LEN];
  char title[ROMM_GAME_TITLE_LEN];
  int save_count;
  int card_count;
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
  int modal_scroll_hold_direction;
  int modal_scroll_hold_frames;
  int modal_touch_active;
  int modal_touch_id;
  float modal_touch_last_y;
  float modal_touch_scroll_remainder;
  UiSyncTrigger trigger;
  char title[64];
  char message[UI_STATUS_LINE_LEN];
  char context[UI_STATUS_LINE_LEN];
} UiSyncFeedback;

typedef struct UiMainLayout {
  float connection_x;
  float connection_y;
  float connection_w;
  float connection_h;
  float connection_row_x;
  float connection_row_w;
  float connection_row_h;
  float connection_row_gap;
  float connection_first_row_y;

  float sync_x;
  float sync_y;
  float sync_w;
  float sync_h;
  float sync_content_x;
  float sync_content_w;
  float sync_button_x;
  float sync_button_w;
  float sync_button_h;
  float sync_button_gap;
  float sync_first_button_y;

  float game_x;
  float game_y;
  float game_w;
  float game_h;
  float game_row_x;
  float game_row_w;
  float game_first_row_y;

  float footer_status_x;
  float footer_status_w;
  float footer_hint_right_x;
  float footer_hint_w;
} UiMainLayout;

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
  int nav_hold_direction;
  int nav_hold_frames;
  char status_line[UI_STATUS_LINE_LEN];
  UiSyncFeedback sync_feedback;
  int pending_auto_sync;
} UiAppState;

static UiAppState g_app_state;
static vita2d_pgf *g_ui_font = NULL;
static int g_common_dialog_active = 0;
static int g_dialog_runtime_initialized = 0;
static SceSystemParamEnterButtonAssign g_dialog_enter_button_assign = SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
static int g_ime_module_loaded = 0;
static int g_touch_front_initialized = 0;
static int g_touch_front_panel_info_ready = 0;
static SceTouchPanelInfo g_touch_front_panel_info;

static float ui_estimate_text_width(const char *text, float scale);
static void ui_build_main_layout(UiMainLayout *layout);
static void ui_render_sync_modal(UiAppState *state);

/*
 * Returns non-zero when a string is non-null and non-empty.
 */
static int has_text(const char *value) {
  return (value != NULL) && (value[0] != '\0');
}

/*
 * Returns the label of the system button that confirms common dialogs.
 */
static const char *ui_dialog_confirm_button_label(void) {
  return (g_dialog_enter_button_assign == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE) ? "O" : "X";
}

/*
 * Returns the label of the system button that declines common dialogs.
 */
static const char *ui_dialog_decline_button_label(void) {
  return (g_dialog_enter_button_assign == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE) ? "X" : "O";
}

/*
 * Returns the controller button used as primary action in this runtime.
 */
static unsigned int ui_primary_action_button(void) {
  return (g_dialog_enter_button_assign == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE) ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS;
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
  feedback->modal_touch_id = -1;
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
 * Polls one controller snapshot (buttons + left stick).
 */
static UiControllerState ui_poll_controller_state(void) {
  UiControllerState state;
  memset(&state, 0, sizeof(state));

  SceCtrlData pad;
  memset(&pad, 0, sizeof(pad));
  sceCtrlPeekBufferPositive(0, &pad, 1);
  state.buttons = pad.buttons;
  state.left_x = pad.lx;
  state.left_y = pad.ly;
  return state;
}

/*
 * Polls the controller and returns the currently held buttons.
 */
static unsigned int ui_poll_buttons(void) {
  return ui_poll_controller_state().buttons;
}

/*
 * Computes edge-triggered button presses from one sampled held-button bitmask.
 */
static unsigned int ui_compute_pressed(unsigned int buttons, unsigned int *io_previous_buttons) {
  if (io_previous_buttons == NULL) {
    return 0U;
  }

  unsigned int pressed = buttons & (~(*io_previous_buttons));
  *io_previous_buttons = buttons;
  return pressed;
}

/*
 * Polls controller and returns buttons that transitioned to pressed state.
 */
static unsigned int ui_poll_pressed(unsigned int *io_previous_buttons) {
  return ui_compute_pressed(ui_poll_buttons(), io_previous_buttons);
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
 * Truncates one rendered line to a target pixel width using PGF measurements.
 * UTF-8 continuation bytes are skipped when shrinking so ellipsis stays valid.
 */
static void ui_truncate_text_to_width(
    const char *source,
    float scale,
    float max_width,
    char *out_text,
    size_t out_size) {
  if ((out_text == NULL) || (out_size == 0U)) {
    return;
  }

  out_text[0] = '\0';
  if (!has_text(source) || (max_width <= 0.0f)) {
    return;
  }

  snprintf(out_text, out_size, "%s", source);
  if (ui_estimate_text_width(out_text, scale) <= max_width) {
    return;
  }

  if (ui_estimate_text_width("...", scale) > max_width) {
    return;
  }

  while (has_text(out_text)) {
    size_t length = strlen(out_text);
    if (length == 0U) {
      break;
    }

    length -= 1U;
    while ((length > 0U) && (((unsigned char)out_text[length] & 0xC0U) == 0x80U)) {
      length -= 1U;
    }
    out_text[length] = '\0';

    char candidate[UI_EDITOR_BUFFER_LEN];
    snprintf(candidate, sizeof(candidate), "%s...", out_text);
    if (ui_estimate_text_width(candidate, scale) <= max_width) {
      snprintf(out_text, out_size, "%s", candidate);
      return;
    }
  }

  snprintf(out_text, out_size, "...");
}

/*
 * Renders secret values as asterisks for screen-safe display.
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

  snprintf(out_text, out_size, "%s", value);
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
  g_dialog_enter_button_assign = dialog_config.enterButtonAssign;
  app_log_write(
      APP_LOG_LEVEL_DEBUG,
      "ui",
      "common dialog buttons confirm=%s decline=%s",
      ui_dialog_confirm_button_label(),
      ui_dialog_decline_button_label());

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
 * Enables front touchscreen sampling for optional modal log scrolling.
 */
static void ui_touch_init(void) {
  if (g_touch_front_initialized) {
    return;
  }

  int status = sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
  if (status < 0) {
    app_log_write(APP_LOG_LEVEL_WARN, "ui", "front touch sampling unavailable: 0x%08X", (unsigned int)status);
    return;
  }

  g_touch_front_initialized = 1;
  memset(&g_touch_front_panel_info, 0, sizeof(g_touch_front_panel_info));
  status = sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &g_touch_front_panel_info);
  if (status < 0) {
    app_log_write(APP_LOG_LEVEL_WARN, "ui", "sceTouchGetPanelInfo failed: 0x%08X", (unsigned int)status);
    return;
  }

  g_touch_front_panel_info_ready = 1;
}

/*
 * Stops front touchscreen sampling before process exit.
 */
static void ui_touch_term(void) {
  if (!g_touch_front_initialized) {
    return;
  }

  sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_STOP);
  g_touch_front_initialized = 0;
  g_touch_front_panel_info_ready = 0;
  memset(&g_touch_front_panel_info, 0, sizeof(g_touch_front_panel_info));
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
 * Returns non-zero when one UI inventory item corresponds to an actual local
 * VMP card file, not just a synthetic restore target.
 */
static int ui_item_has_local_memory_card(const SyncSaveDescriptor *item) {
  if (item == NULL) {
    return 0;
  }

  return item->size_bytes > 0U;
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
      if (ui_item_has_local_memory_card(item)) {
        out_games[existing].card_count += 1;
      }
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
    entry->card_count = ui_item_has_local_memory_card(item) ? 1 : 0;
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
 * Returns non-zero when one selection index points at a sync action button.
 */
static int ui_is_sync_button_index(int index) {
  return (index >= UI_SELECT_SYNC_PRIMARY) && (index <= UI_SELECT_RESCAN);
}

/*
 * Applies a few explicit D-pad shortcuts so the main action stays easy to reach.
 * This keeps navigation more console-like than pure geometric nearest-neighbor moves.
 */
static int ui_try_move_selection_shortcut(UiAppState *state, int direction) {
  if (state == NULL) {
    return 0;
  }

  if ((direction == UI_NAV_RIGHT) &&
      ((state->selected_index == UI_SELECT_SERVER_URL) ||
       (state->selected_index == UI_SELECT_USERNAME) ||
       (state->selected_index == UI_SELECT_PASSWORD) ||
       (state->selected_index == UI_SELECT_DRY_RUN) ||
       (state->selected_index >= UI_SELECT_GAME_BASE))) {
    state->selected_index = UI_SELECT_SYNC_PRIMARY;
    return 1;
  }

  if ((direction == UI_NAV_LEFT) && ui_is_sync_button_index(state->selected_index)) {
    if ((state->active_game_index >= 0) && (state->active_game_index < state->game_count)) {
      state->selected_index = UI_SELECT_GAME_BASE + state->active_game_index;
    } else {
      state->selected_index = UI_SELECT_DRY_RUN;
    }
    return 1;
  }

  return 0;
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

  UiMainLayout layout;
  ui_build_main_layout(&layout);

  if (index == UI_SELECT_SERVER_URL) {
    *out_x = layout.connection_row_x + (layout.connection_row_w * 0.5f);
    *out_y = layout.connection_first_row_y + (layout.connection_row_h * 0.5f);
    return 0;
  }
  if (index == UI_SELECT_USERNAME) {
    *out_x = layout.connection_row_x + (layout.connection_row_w * 0.5f);
    *out_y = layout.connection_first_row_y + layout.connection_row_h +
             layout.connection_row_gap + (layout.connection_row_h * 0.5f);
    return 0;
  }
  if (index == UI_SELECT_PASSWORD) {
    *out_x = layout.connection_row_x + (layout.connection_row_w * 0.5f);
    *out_y = layout.connection_first_row_y + ((layout.connection_row_h + layout.connection_row_gap) * 2.0f) +
             (layout.connection_row_h * 0.5f);
    return 0;
  }
  if (index == UI_SELECT_DRY_RUN) {
    *out_x = layout.connection_row_x + (layout.connection_row_w * 0.5f);
    *out_y = layout.connection_first_row_y + ((layout.connection_row_h + layout.connection_row_gap) * 3.0f) +
             (layout.connection_row_h * 0.5f);
    return 0;
  }
  if (index == UI_SELECT_SYNC_PRIMARY) {
    *out_x = layout.sync_button_x + (layout.sync_button_w * 0.5f);
    *out_y = layout.sync_first_button_y + (layout.sync_button_h * 0.5f);
    return 0;
  }
  if (index == UI_SELECT_SYNC_ALL) {
    *out_x = layout.sync_button_x + (layout.sync_button_w * 0.5f);
    *out_y = layout.sync_first_button_y + layout.sync_button_h +
             layout.sync_button_gap + (layout.sync_button_h * 0.5f);
    return 0;
  }
  if (index == UI_SELECT_RESCAN) {
    *out_x = layout.sync_button_x + (layout.sync_button_w * 0.5f);
    *out_y = layout.sync_first_button_y + ((layout.sync_button_h + layout.sync_button_gap) * 2.0f) +
             (layout.sync_button_h * 0.5f);
    return 0;
  }

  if (index >= UI_SELECT_GAME_BASE) {
    int game_index = index - UI_SELECT_GAME_BASE;
    if ((game_index < 0) || (game_index >= state->game_count)) {
      return -1;
    }

    *out_x = layout.game_row_x + (layout.game_row_w * 0.5f);
    *out_y = layout.game_first_row_y + (UI_GAME_ROW_HEIGHT * (float)game_index) + (UI_GAME_ROW_HEIGHT * 0.5f);
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

  if (ui_try_move_selection_shortcut(state, direction)) {
    if (state->selected_index >= UI_SELECT_GAME_BASE) {
      state->active_game_index = state->selected_index - UI_SELECT_GAME_BASE;
    }
    return 1;
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
 * Applies a deterministic wrap fallback when geometric directional lookup fails.
 * This keeps D-pad and left-stick traversal predictable in every panel.
 */
static void ui_move_selection_with_fallback(UiAppState *state, int direction) {
  if (state == NULL) {
    return;
  }

  int total_entries = ui_total_selectable_entries(state);
  if (total_entries <= 0) {
    return;
  }

  if (!ui_move_selection_direction(state, direction)) {
    if ((direction == UI_NAV_UP) || (direction == UI_NAV_LEFT)) {
      state->selected_index -= 1;
      if (state->selected_index < 0) {
        state->selected_index = total_entries - 1;
      }
    } else if ((direction == UI_NAV_DOWN) || (direction == UI_NAV_RIGHT)) {
      state->selected_index += 1;
      if (state->selected_index >= total_entries) {
        state->selected_index = 0;
      }
    }
  }

  if (state->selected_index >= UI_SELECT_GAME_BASE) {
    state->active_game_index = state->selected_index - UI_SELECT_GAME_BASE;
  }
}

/*
 * Converts one analog axis to a signed direction using a deadzone around center.
 */
static int ui_analog_axis_direction(unsigned char axis_value) {
  int delta = (int)axis_value - UI_ANALOG_CENTER;
  if (delta <= -UI_ANALOG_DEADZONE) {
    return -1;
  }
  if (delta >= UI_ANALOG_DEADZONE) {
    return 1;
  }
  return 0;
}

/*
 * Resolves left-stick movement to one cardinal direction, preferring the dominant axis.
 */
static int ui_analog_navigation_direction(unsigned char left_x, unsigned char left_y) {
  int horizontal = ui_analog_axis_direction(left_x);
  int vertical = ui_analog_axis_direction(left_y);
  if ((horizontal == 0) && (vertical == 0)) {
    return UI_NAV_NONE;
  }

  int horizontal_delta = abs((int)left_x - UI_ANALOG_CENTER);
  int vertical_delta = abs((int)left_y - UI_ANALOG_CENTER);

  if (vertical_delta >= horizontal_delta) {
    if (vertical < 0) {
      return UI_NAV_UP;
    }
    if (vertical > 0) {
      return UI_NAV_DOWN;
    }
  }

  if (horizontal < 0) {
    return UI_NAV_LEFT;
  }
  if (horizontal > 0) {
    return UI_NAV_RIGHT;
  }

  return UI_NAV_NONE;
}

/*
 * Resolves one frame of held input to a cardinal navigation direction.
 * D-pad takes priority and left stick is used when D-pad is neutral.
 */
static int ui_resolve_navigation_direction(unsigned int buttons, unsigned char left_x, unsigned char left_y) {
  if (buttons & SCE_CTRL_UP) {
    return UI_NAV_UP;
  }
  if (buttons & SCE_CTRL_DOWN) {
    return UI_NAV_DOWN;
  }
  if (buttons & SCE_CTRL_LEFT) {
    return UI_NAV_LEFT;
  }
  if (buttons & SCE_CTRL_RIGHT) {
    return UI_NAV_RIGHT;
  }
  return ui_analog_navigation_direction(left_x, left_y);
}

/*
 * Applies repeat-aware directional navigation from D-pad and left analog stick.
 */
static void ui_handle_navigation_input(UiAppState *state, unsigned int buttons, unsigned char left_x, unsigned char left_y) {
  if (state == NULL) {
    return;
  }

  int direction = ui_resolve_navigation_direction(buttons, left_x, left_y);
  if (direction == UI_NAV_NONE) {
    state->nav_hold_direction = UI_NAV_NONE;
    state->nav_hold_frames = 0;
    return;
  }

  int trigger_move = 0;
  if (state->nav_hold_direction != direction) {
    state->nav_hold_direction = direction;
    state->nav_hold_frames = 0;
    trigger_move = 1;
  } else {
    state->nav_hold_frames += 1;
    if ((state->nav_hold_frames >= UI_NAV_REPEAT_DELAY_FRAMES) &&
        (((state->nav_hold_frames - UI_NAV_REPEAT_DELAY_FRAMES) % UI_NAV_REPEAT_INTERVAL_FRAMES) == 0)) {
      trigger_move = 1;
    }
  }

  if (trigger_move) {
    ui_move_selection_with_fallback(state, direction);
  }
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
 * Snaps coordinates in a scale-aware way so transformed text still lands on pixel boundaries.
 * This reduces blur introduced by fractional scaling and sub-pixel glyph origins.
 */
static float ui_snap_to_text_grid(float value, float scale) {
  if (scale <= 0.0f) {
    return ui_snap_to_pixel(value);
  }

  return ui_snap_to_pixel(value * scale) / scale;
}

/*
 * Quantizes text scale to stable increments.
 * Restricting the number of effective scales improves consistency and sharpness.
 */
static float ui_quantize_text_scale(float scale) {
  if (scale <= 0.0f) {
    return UI_TEXT_SCALE_MIN;
  }

  float stepped = floorf((scale / UI_TEXT_SCALE_STEP) + 0.5f) * UI_TEXT_SCALE_STEP;
  if (stepped < UI_TEXT_SCALE_MIN) {
    return UI_TEXT_SCALE_MIN;
  }
  if (stepped > UI_TEXT_SCALE_MAX) {
    return UI_TEXT_SCALE_MAX;
  }
  return stepped;
}

/*
 * Builds a subtle black shadow from text alpha.
 * This keeps text readable on busy panel edges without overpowering the glyph shape.
 */
static unsigned int ui_text_shadow_color(unsigned int text_color) {
  unsigned int alpha = (text_color >> 24) & 0xFFU;
  if (alpha == 0U) {
    return 0U;
  }

  unsigned int shadow_alpha = alpha / 3U;
  if (shadow_alpha < UI_TEXT_SHADOW_ALPHA_MIN) {
    shadow_alpha = UI_TEXT_SHADOW_ALPHA_MIN;
  }
  if (shadow_alpha > UI_TEXT_SHADOW_ALPHA_MAX) {
    shadow_alpha = UI_TEXT_SHADOW_ALPHA_MAX;
  }

  return RGBA8(0, 0, 0, shadow_alpha);
}

/*
 * Applies a readability-oriented text scale policy for Vita's display.
 * Tiny scales are lifted and then quantized to reduce blurred interpolation states.
 */
static float ui_resolve_text_scale(float scale) {
  float resolved = scale * UI_TEXT_SCALE_BOOST;
  if (resolved < UI_TEXT_SCALE_MIN) {
    resolved = UI_TEXT_SCALE_MIN;
  }
  if (resolved > UI_TEXT_SCALE_MAX) {
    resolved = UI_TEXT_SCALE_MAX;
  }
  return ui_quantize_text_scale(resolved);
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
  float draw_x = ui_snap_to_text_grid(x, draw_scale);
  float draw_y = ui_snap_to_text_grid(y, draw_scale);

  unsigned int shadow_color = ui_text_shadow_color(color);
  if (shadow_color != 0U) {
    vita2d_pgf_draw_text(
        g_ui_font,
        ui_snap_to_text_grid(draw_x + UI_TEXT_SHADOW_OFFSET_X, draw_scale),
        ui_snap_to_text_grid(draw_y + UI_TEXT_SHADOW_OFFSET_Y, draw_scale),
        shadow_color,
        draw_scale,
        line);
  }

  vita2d_pgf_draw_text(g_ui_font, draw_x, draw_y, color, draw_scale, line);
}

/*
 * Estimates text width to center short labels in panels.
 */
static float ui_estimate_text_width(const char *text, float scale) {
  if ((g_ui_font == NULL) || !has_text(text)) {
    return 0.0f;
  }
  return (float)vita2d_pgf_text_width(g_ui_font, ui_resolve_text_scale(scale), text);
}

/*
 * Estimates text height for one rendered line at the requested scale.
 */
static float ui_estimate_text_height(float scale) {
  if (g_ui_font == NULL) {
    return 18.0f * ui_resolve_text_scale(scale);
  }

  int width = 0;
  int height = 0;
  vita2d_pgf_text_dimensions(g_ui_font, ui_resolve_text_scale(scale), "Ag", &width, &height);
  if (height <= 0) {
    return 18.0f * ui_resolve_text_scale(scale);
  }

  return (float)height;
}

/*
 * Draws plain text aligned to the provided right edge.
 */
static void ui_draw_text_right(float right_x, float y, unsigned int color, float scale, const char *text) {
  if (!has_text(text)) {
    return;
  }

  float width = ui_estimate_text_width(text, scale);
  ui_draw_text(right_x - width, y, color, scale, "%s", text);
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
 * Draws one single-line label that always stays inside the requested width.
 */
static void ui_draw_truncated_text(
    float x,
    float y,
    float max_width,
    unsigned int color,
    float scale,
    const char *text) {
  char truncated[UI_EDITOR_BUFFER_LEN];
  ui_truncate_text_to_width(text, scale, max_width, truncated, sizeof(truncated));
  if (!has_text(truncated)) {
    return;
  }

  ui_draw_text(x, y, color, scale, "%s", truncated);
}

/*
 * Draws one right-aligned single-line label that ellipsizes to a max width.
 */
static void ui_draw_truncated_text_right(
    float right_x,
    float y,
    float max_width,
    unsigned int color,
    float scale,
    const char *text) {
  char truncated[UI_EDITOR_BUFFER_LEN];
  ui_truncate_text_to_width(text, scale, max_width, truncated, sizeof(truncated));
  if (!has_text(truncated)) {
    return;
  }

  ui_draw_text_right(right_x, y, color, scale, truncated);
}

/*
 * Stores one wrapped line when the destination buffer still has capacity.
 * The running line count always reflects the full logical line total.
 */
static void ui_wrap_store_line(
    char out_lines[][UI_WRAP_BUFFER_LEN],
    int max_lines,
    int *io_line_count,
    const char *line) {
  if (io_line_count == NULL) {
    return;
  }

  if ((out_lines != NULL) && (*io_line_count >= 0) && (*io_line_count < max_lines)) {
    snprintf(out_lines[*io_line_count], UI_WRAP_BUFFER_LEN, "%s", has_text(line) ? line : "");
  }
  *io_line_count += 1;
}

/*
 * Appends ellipsis to the final stored wrapped line without exceeding max width.
 */
static void ui_wrap_append_ellipsis(char *line, float scale, float max_width) {
  if (line == NULL) {
    return;
  }

  if (!has_text(line)) {
    snprintf(line, UI_WRAP_BUFFER_LEN, "...");
    return;
  }

  char candidate[UI_WRAP_BUFFER_LEN];
  snprintf(candidate, sizeof(candidate), "%s...", line);
  while (has_text(line) && (ui_estimate_text_width(candidate, scale) > max_width)) {
    size_t length = strlen(line);
    if (length == 0U) {
      break;
    }
    line[length - 1U] = '\0';
    snprintf(candidate, sizeof(candidate), "%s...", line);
  }

  char final_line[UI_WRAP_BUFFER_LEN];
  snprintf(final_line, sizeof(final_line), "%s...", line);
  snprintf(line, UI_WRAP_BUFFER_LEN, "%s", final_line);
}

/*
 * Pushes one word into the current wrapped line, splitting long words if needed.
 */
static void ui_wrap_push_word(
    const char *word,
    float scale,
    float max_width,
    char out_lines[][UI_WRAP_BUFFER_LEN],
    int max_lines,
    int *io_line_count,
    char *current_line,
    size_t current_line_size) {
  if (!has_text(word) || (io_line_count == NULL) || (current_line == NULL) || (current_line_size == 0U)) {
    return;
  }

  char remaining[UI_WRAP_BUFFER_LEN];
  snprintf(remaining, sizeof(remaining), "%s", word);

  while (has_text(remaining)) {
    char candidate[UI_WRAP_BUFFER_LEN];
    if (has_text(current_line)) {
      snprintf(candidate, sizeof(candidate), "%s %s", current_line, remaining);
    } else {
      snprintf(candidate, sizeof(candidate), "%s", remaining);
    }

    if (ui_estimate_text_width(candidate, scale) <= max_width) {
      snprintf(current_line, current_line_size, "%s", candidate);
      return;
    }

    if (has_text(current_line)) {
      ui_wrap_store_line(out_lines, max_lines, io_line_count, current_line);
      current_line[0] = '\0';
      continue;
    }

    char chunk[UI_WRAP_BUFFER_LEN];
    size_t chunk_len = 0U;
    chunk[0] = '\0';

    while (remaining[chunk_len] != '\0') {
      char next_chunk[UI_WRAP_BUFFER_LEN];
      snprintf(next_chunk, sizeof(next_chunk), "%s%c", chunk, remaining[chunk_len]);
      if ((chunk[0] != '\0') && (ui_estimate_text_width(next_chunk, scale) > max_width)) {
        break;
      }

      snprintf(chunk, sizeof(chunk), "%s", next_chunk);
      chunk_len += 1U;
    }

    if (remaining[chunk_len] != '\0') {
      ui_wrap_store_line(out_lines, max_lines, io_line_count, chunk);
      memmove(remaining, remaining + chunk_len, strlen(remaining + chunk_len) + 1U);
      continue;
    }

    snprintf(current_line, current_line_size, "%s", chunk);
    return;
  }
}

/*
 * Wraps text to a maximum pixel width using the active PGF font metrics.
 * Explicit newlines are preserved, spaces are collapsed between wrapped words.
 */
static int ui_wrap_text_lines(
    const char *text,
    float scale,
    float max_width,
    char out_lines[][UI_WRAP_BUFFER_LEN],
    int max_lines,
    int append_ellipsis_on_truncate) {
  if (!has_text(text) || (max_lines <= 0) || (max_width <= 0.0f)) {
    return 0;
  }

  int line_count = 0;
  char current_line[UI_WRAP_BUFFER_LEN];
  current_line[0] = '\0';

  const char *cursor = text;
  while (*cursor != '\0') {
    if (*cursor == '\n') {
      ui_wrap_store_line(out_lines, max_lines, &line_count, current_line);
      current_line[0] = '\0';
      cursor += 1;
      continue;
    }

    while ((*cursor == ' ') || (*cursor == '\t') || (*cursor == '\r')) {
      cursor += 1;
    }
    if (*cursor == '\0') {
      break;
    }
    if (*cursor == '\n') {
      continue;
    }

    char word[UI_WRAP_BUFFER_LEN];
    size_t word_len = 0U;
    while ((*cursor != '\0') &&
           (*cursor != '\n') &&
           (*cursor != ' ') &&
           (*cursor != '\t') &&
           (*cursor != '\r')) {
      if ((word_len + 1U) < sizeof(word)) {
        word[word_len++] = *cursor;
      }
      cursor += 1;
    }
    word[word_len] = '\0';

    ui_wrap_push_word(word, scale, max_width, out_lines, max_lines, &line_count, current_line, sizeof(current_line));
  }

  if (has_text(current_line)) {
    ui_wrap_store_line(out_lines, max_lines, &line_count, current_line);
  }

  if (append_ellipsis_on_truncate && (line_count > max_lines) && (out_lines != NULL)) {
    ui_wrap_append_ellipsis(out_lines[max_lines - 1], scale, max_width);
  }

  return line_count;
}

/*
 * Draws a wrapped text block and returns the y position for the next block.
 */
static float ui_draw_wrapped_text_block(
    float x,
    float y,
    float max_width,
    unsigned int color,
    float scale,
    float line_spacing,
    int max_lines,
    const char *text) {
  if (!has_text(text) || (max_lines <= 0)) {
    return y;
  }

  char lines[UI_WRAP_MAX_LINES_PER_BLOCK][UI_WRAP_BUFFER_LEN];
  int total_lines = ui_wrap_text_lines(text, scale, max_width, lines, max_lines, 1);
  int render_lines = total_lines;
  if (render_lines > max_lines) {
    render_lines = max_lines;
  }

  float line_height = ui_estimate_text_height(scale) + line_spacing;
  for (int i = 0; i < render_lines; ++i) {
    ui_draw_text(x, y + (line_height * (float)i), color, scale, "%s", lines[i]);
  }

  return y + (line_height * (float)render_lines);
}

/*
 * Builds the fixed home-screen geometry used by rendering and D-pad anchors.
 */
static void ui_build_main_layout(UiMainLayout *layout) {
  if (layout == NULL) {
    return;
  }

  memset(layout, 0, sizeof(*layout));

  layout->connection_x = 32.0f;
  layout->connection_y = 88.0f;
  layout->connection_w = 332.0f;
  layout->connection_h = 256.0f;
  layout->connection_row_x = layout->connection_x + 16.0f;
  layout->connection_row_w = layout->connection_w - 32.0f;
  layout->connection_row_h = 38.0f;
  layout->connection_row_gap = 3.0f;
  layout->connection_first_row_y = layout->connection_y + 46.0f;

  layout->sync_x = layout->connection_x + layout->connection_w + 20.0f;
  layout->sync_y = layout->connection_y;
  layout->sync_w = 544.0f;
  layout->sync_h = layout->connection_h;
  layout->sync_content_x = layout->sync_x + 16.0f;
  layout->sync_content_w = layout->sync_w - 32.0f;
  layout->sync_button_x = layout->sync_content_x;
  layout->sync_button_w = layout->sync_content_w;
  layout->sync_button_h = 30.0f;
  layout->sync_button_gap = 8.0f;
  layout->sync_first_button_y = layout->sync_y + layout->sync_h - 16.0f -
                                (layout->sync_button_h * 3.0f) -
                                (layout->sync_button_gap * 2.0f);

  layout->game_x = 32.0f;
  layout->game_y = layout->connection_y + layout->connection_h + 18.0f;
  layout->game_w = 896.0f;
  layout->game_h = 148.0f;
  layout->game_row_x = layout->game_x + 16.0f;
  layout->game_row_w = layout->game_w - 32.0f;
  layout->game_first_row_y = layout->game_y + 36.0f;

  layout->footer_status_x = 92.0f;
  layout->footer_status_w = 392.0f;
  layout->footer_hint_right_x = 928.0f;
  layout->footer_hint_w = 380.0f;
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
 * Returns how many wrapped log rows fit inside a viewport height.
 */
static int ui_log_viewport_visible_lines(float h) {
  float usable_height = h - UI_LOG_TOP_PADDING - UI_LOG_BOTTOM_PADDING;
  if (usable_height < UI_LOG_LINE_HEIGHT) {
    return 1;
  }

  int visible_lines = (int)floorf(usable_height / UI_LOG_LINE_HEIGHT);
  if (visible_lines < 1) {
    visible_lines = 1;
  }
  return visible_lines;
}

/*
 * Counts wrapped visual log rows for the current history and viewport width.
 */
static int ui_log_total_visual_lines(float viewport_width) {
  float inner_width = viewport_width - 20.0f;
  if (inner_width <= 0.0f) {
    return 0;
  }

  int total_visual_lines = 0;
  int total_entries = app_log_history_count();
  for (int i = 0; i < total_entries; ++i) {
    const char *line = app_log_history_line(i);
    if (!has_text(line)) {
      continue;
    }

    char wrapped_lines[UI_WRAP_MAX_LINES_PER_BLOCK][UI_WRAP_BUFFER_LEN];
    int wrapped_count = ui_wrap_text_lines(line, UI_LOG_TEXT_SCALE, inner_width, wrapped_lines, UI_WRAP_MAX_LINES_PER_BLOCK, 0);
    total_visual_lines += (wrapped_count > 0) ? wrapped_count : 1;
  }

  return total_visual_lines;
}

/*
 * Draws one scrollable log viewport from global app_log history.
 * Scrolling is expressed in wrapped visual line offsets rather than raw entries.
 */
static void ui_draw_log_viewport(
    float x,
    float y,
    float w,
    float h,
    int start_visual_line) {
  ui_draw_panel(x, y, w, h, UI_COLOR_PANEL_ALT, UI_COLOR_PANEL_BORDER);

  int visible_lines = ui_log_viewport_visible_lines(h);
  int total_visual_lines = ui_log_total_visual_lines(w);
  int max_start = total_visual_lines - visible_lines;
  if (max_start < 0) {
    max_start = 0;
  }

  int start = clamp_int(start_visual_line, 0, max_start);
  int end = start + visible_lines;

  float line_y = y + UI_LOG_TOP_PADDING;
  int visual_index = 0;
  int rendered_lines = 0;
  int total_entries = app_log_history_count();
  for (int i = 0; (i < total_entries) && (visual_index < end); ++i) {
    const char *line = app_log_history_line(i);
    if (!has_text(line)) {
      continue;
    }

    char wrapped_lines[UI_WRAP_MAX_LINES_PER_BLOCK][UI_WRAP_BUFFER_LEN];
    int wrapped_count = ui_wrap_text_lines(line, UI_LOG_TEXT_SCALE, w - 20.0f, wrapped_lines, UI_WRAP_MAX_LINES_PER_BLOCK, 0);
    if (wrapped_count <= 0) {
      continue;
    }

    for (int j = 0; (j < wrapped_count) && (visual_index < end); ++j) {
      if (visual_index >= start) {
        ui_draw_text(x + 10.0f, line_y, ui_log_line_color(line), UI_LOG_TEXT_SCALE, "%s", wrapped_lines[j]);
        line_y += UI_LOG_LINE_HEIGHT;
        rendered_lines += 1;
        if (rendered_lines >= visible_lines) {
          return;
        }
      }
      visual_index += 1;
    }
  }
}

/*
 * Draws one editable field row with label/value hierarchy.
 */
static void ui_draw_field_row(float x, float y, float w, float h, int selected, const char *label, const char *value) {
  unsigned int fill = selected ? UI_COLOR_FIELD_ACTIVE : UI_COLOR_FIELD;
  unsigned int border = selected ? UI_COLOR_PANEL_BORDER_ACTIVE : UI_COLOR_PANEL_BORDER;
  unsigned int value_color = has_text(value) ? UI_COLOR_TEXT : UI_COLOR_TEXT_DIM;
  float label_scale = (h >= 52.0f) ? 0.66f : 0.60f;
  float value_scale = (h >= 52.0f) ? 0.70f : 0.64f;
  float label_y = y + ((h >= 52.0f) ? 16.0f : 13.0f);
  float value_y = y + ((h >= 52.0f) ? 32.0f : 25.0f);
  float line_spacing = (h >= 52.0f) ? 1.0f : 0.0f;

  ui_draw_panel(x, y, w, h, fill, border);
  if (selected) {
    vita2d_draw_rectangle(ui_snap_to_pixel(x), ui_snap_to_pixel(y), 4.0f, ui_snap_to_pixel(h), UI_COLOR_ACCENT);
  }

  float inner_x = x + 14.0f;
  float inner_w = w - 28.0f;
  ui_draw_truncated_text(inner_x, label_y, inner_w, UI_COLOR_TEXT_DIM, label_scale, label);
  ui_draw_wrapped_text_block(inner_x, value_y, inner_w, value_color, value_scale, line_spacing, 2, value);
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

  ui_draw_text_center(x + (w * 0.5f), y + (h * 0.62f), text_color, primary ? 0.82f : 0.76f, title);
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
    int card_count) {
  unsigned int fill = focused ? UI_COLOR_FIELD_ACTIVE : (active ? UI_COLOR_ACCENT_SOFT : UI_COLOR_PANEL_ALT);
  unsigned int border = focused ? UI_COLOR_PANEL_BORDER_ACTIVE : (active ? UI_COLOR_BUTTON_BORDER : UI_COLOR_PANEL_BORDER);
  unsigned int title_color = focused ? UI_COLOR_TEXT : (active ? UI_COLOR_TEXT : UI_COLOR_TEXT_MUTED);
  unsigned int count_color = active ? UI_COLOR_TEXT : UI_COLOR_TEXT_DIM;

  ui_draw_panel(x, y, w, h, fill, border);
  if (active) {
    vita2d_draw_rectangle(ui_snap_to_pixel(x), ui_snap_to_pixel(y), 3.0f, ui_snap_to_pixel(h), UI_COLOR_ACCENT);
  }

  char count_text[32];
  snprintf(count_text, sizeof(count_text), "%d card%s", card_count, (card_count == 1) ? "" : "s");

  float count_scale = 0.64f;
  float count_width = ui_estimate_text_width(count_text, count_scale);
  float title_width = w - 36.0f - count_width;
  if (title_width < 80.0f) {
    title_width = 80.0f;
  }

  ui_draw_truncated_text(x + 12.0f, y + 18.0f, title_width, title_color, 0.72f, title);
  ui_draw_text_right(x + w - 12.0f, y + 18.0f, count_color, count_scale, count_text);
}

/*
 * Renders the top title area.
 */
static void ui_render_header(const UiAppState *state) {
  const char *status_text = ui_sync_action_enabled(state) ? "Ready to synchronize" : "Setup required";
  unsigned int status_color = ui_sync_action_enabled(state) ? UI_COLOR_SUCCESS : UI_COLOR_WARNING;
  if (state->sync_feedback.running) {
    status_text = "Synchronization running";
    status_color = UI_COLOR_ACCENT;
  } else if (state->sync_feedback.completed) {
    status_text = state->sync_feedback.success ? "Last sync completed" : "Last sync failed";
    status_color = state->sync_feedback.success ? UI_COLOR_SUCCESS : UI_COLOR_DANGER;
  }

  ui_draw_text(32.0f, 28.0f, UI_COLOR_TEXT_DIM, 0.72f, "RomM Vita Sync");
  ui_draw_text(32.0f, 52.0f, UI_COLOR_TEXT, 1.02f, "Save Synchronization");
  ui_draw_truncated_text(
      32.0f,
      70.0f,
      620.0f,
      UI_COLOR_TEXT_MUTED,
      0.68f,
      "Configure the RoMM server, choose a PS1 game, then start a manual or startup synchronization.");

  ui_draw_text_right(928.0f, 28.0f, UI_COLOR_TEXT_DIM, 0.72f, "Status");
  ui_draw_truncated_text_right(928.0f, 52.0f, 240.0f, status_color, 0.76f, status_text);
}

/*
 * Renders the connection form and sync safety toggles.
 */
static void ui_render_connection_panel(const UiAppState *state) {
  if (state == NULL) {
    return;
  }

  UiMainLayout layout;
  ui_build_main_layout(&layout);

  char url_display[APP_CONFIG_MAX_URL_LEN + 16];
  char username_display[APP_CONFIG_MAX_USERNAME_LEN + 16];
  char password_display[APP_CONFIG_MAX_PASSWORD_LEN + 16];
  char dry_run_display[24];
  ui_format_field_display(state->config.romm_url, 0, url_display, sizeof(url_display));
  ui_format_field_display(state->config.romm_username, 0, username_display, sizeof(username_display));
  ui_format_field_display(state->config.romm_password, 1, password_display, sizeof(password_display));
  snprintf(dry_run_display, sizeof(dry_run_display), "%s", state->config.sync_dry_run ? "Enabled" : "Disabled");

  ui_draw_panel(layout.connection_x, layout.connection_y, layout.connection_w, layout.connection_h, UI_COLOR_PANEL, UI_COLOR_PANEL_BORDER);
  ui_draw_text(layout.connection_x + 16.0f, layout.connection_y + 30.0f, UI_COLOR_TEXT, 0.88f, "Connection");

  ui_draw_field_row(
      layout.connection_row_x,
      layout.connection_first_row_y,
      layout.connection_row_w,
      layout.connection_row_h,
      state->selected_index == UI_SELECT_SERVER_URL,
      "RoMM server address",
      url_display);
  ui_draw_field_row(
      layout.connection_row_x,
      layout.connection_first_row_y + layout.connection_row_h + layout.connection_row_gap,
      layout.connection_row_w,
      layout.connection_row_h,
      state->selected_index == UI_SELECT_USERNAME,
      "RoMM username",
      username_display);
  ui_draw_field_row(
      layout.connection_row_x,
      layout.connection_first_row_y + ((layout.connection_row_h + layout.connection_row_gap) * 2.0f),
      layout.connection_row_w,
      layout.connection_row_h,
      state->selected_index == UI_SELECT_PASSWORD,
      "RoMM password",
      password_display);
  ui_draw_field_row(
      layout.connection_row_x,
      layout.connection_first_row_y + ((layout.connection_row_h + layout.connection_row_gap) * 3.0f),
      layout.connection_row_w,
      layout.connection_row_h,
      state->selected_index == UI_SELECT_DRY_RUN,
      "Dry-run mode",
      dry_run_display);
}

/*
 * Renders the active sync target summary and the primary action.
 */
static void ui_render_sync_panel(const UiAppState *state) {
  if (state == NULL) {
    return;
  }

  UiMainLayout layout;
  ui_build_main_layout(&layout);

  const UiGameEntry *game = ui_active_game(state);
  char title_display[ROMM_GAME_TITLE_LEN + 16];
  char detail[96];
  char readiness[UI_STATUS_LINE_LEN];
  unsigned int readiness_color = UI_COLOR_SUCCESS;
  int sync_enabled = ui_sync_action_enabled(state);
  int sync_all_enabled = ui_sync_all_action_enabled(state);

  if (game != NULL) {
    const char *resolved_title = has_text(game->title) ? game->title : game->game_id;
    snprintf(title_display, sizeof(title_display), "%s", resolved_title);
    if (game->card_count == game->save_count) {
      snprintf(detail, sizeof(detail), "%s | %d card%s", game->game_id, game->card_count, (game->card_count == 1) ? "" : "s");
    } else if (game->card_count == 0) {
      snprintf(detail, sizeof(detail), "%s | 0 cards | restore target", game->game_id);
    } else {
      snprintf(
          detail,
          sizeof(detail),
          "%s | %d card%s | %d target%s",
          game->game_id,
          game->card_count,
          (game->card_count == 1) ? "" : "s",
          game->save_count,
          (game->save_count == 1) ? "" : "s");
    }
  } else {
    snprintf(title_display, sizeof(title_display), "No PS1 game selected");
    snprintf(detail, sizeof(detail), "Rescan local saves after copying memory cards to the Vita.");
  }

  if (state->sync_feedback.running) {
    snprintf(readiness, sizeof(readiness), "%s", state->sync_feedback.message);
    readiness_color = UI_COLOR_ACCENT;
  } else if (state->sync_feedback.completed) {
    snprintf(readiness, sizeof(readiness), "%s", state->sync_feedback.message);
    readiness_color = state->sync_feedback.success ? UI_COLOR_SUCCESS : UI_COLOR_DANGER;
  } else if (game == NULL) {
    snprintf(readiness, sizeof(readiness), "No local PS1 saves detected yet.");
    readiness_color = UI_COLOR_WARNING;
  } else if (!app_config_has_server_url(&state->config)) {
    snprintf(readiness, sizeof(readiness), "Enter the RoMM server address first.");
    readiness_color = UI_COLOR_WARNING;
  } else if (!app_config_has_auth(&state->config)) {
    snprintf(readiness, sizeof(readiness), "Enter your RoMM username and password to enable sync.");
    readiness_color = UI_COLOR_WARNING;
  } else {
    snprintf(readiness, sizeof(readiness), "Connection details look complete.");
  }

  ui_draw_panel(layout.sync_x, layout.sync_y, layout.sync_w, layout.sync_h, UI_COLOR_PANEL, UI_COLOR_PANEL_BORDER);
  ui_draw_text(layout.sync_content_x, layout.sync_y + 30.0f, UI_COLOR_TEXT, 0.88f, "Synchronize");

  float cursor_y = layout.sync_y + 58.0f;
  cursor_y = ui_draw_wrapped_text_block(
      layout.sync_content_x,
      cursor_y,
      layout.sync_content_w,
      game != NULL ? UI_COLOR_TEXT : UI_COLOR_TEXT_MUTED,
      0.88f,
      2.0f,
      2,
      title_display);
  cursor_y += 4.0f;
  ui_draw_truncated_text(layout.sync_content_x, cursor_y, layout.sync_content_w, UI_COLOR_TEXT_MUTED, 0.68f, detail);
  cursor_y += ui_estimate_text_height(0.68f) + 8.0f;
  ui_draw_wrapped_text_block(
      layout.sync_content_x,
      cursor_y,
      layout.sync_content_w,
      readiness_color,
      0.64f,
      1.0f,
      2,
      readiness);

  ui_draw_button(
      layout.sync_button_x,
      layout.sync_first_button_y,
      layout.sync_button_w,
      layout.sync_button_h,
      1,
      state->selected_index == UI_SELECT_SYNC_PRIMARY,
      sync_enabled,
      "Synchronize Selected Game");
  ui_draw_button(
      layout.sync_button_x,
      layout.sync_first_button_y + layout.sync_button_h + layout.sync_button_gap,
      layout.sync_button_w,
      layout.sync_button_h,
      0,
      state->selected_index == UI_SELECT_SYNC_ALL,
      sync_all_enabled,
      "Synchronize All Saves");
  ui_draw_button(
      layout.sync_button_x,
      layout.sync_first_button_y + ((layout.sync_button_h + layout.sync_button_gap) * 2.0f),
      layout.sync_button_w,
      layout.sync_button_h,
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

  UiMainLayout layout;
  ui_build_main_layout(&layout);

  ui_draw_panel(layout.game_x, layout.game_y, layout.game_w, layout.game_h, UI_COLOR_PANEL, UI_COLOR_PANEL_BORDER);
  ui_draw_text(layout.game_x + 16.0f, layout.game_y + 24.0f, UI_COLOR_TEXT, 0.84f, "Detected PS1 Games");

  if (state->game_count <= 0) {
    ui_draw_truncated_text(
        layout.game_x + 16.0f,
        layout.game_y + 60.0f,
        layout.game_w - 32.0f,
        UI_COLOR_TEXT_MUTED,
        0.72f,
        "No PS1 save targets were detected on this Vita.");
    return;
  }

  int start = state->game_scroll;
  int end = start + UI_GAME_LIST_VISIBLE;
  if (end > state->game_count) {
    end = state->game_count;
  }

  char summary[48];
  snprintf(summary, sizeof(summary), "Showing %d-%d of %d", start + 1, end, state->game_count);
  ui_draw_text_right(layout.game_x + layout.game_w - 16.0f, layout.game_y + 24.0f, UI_COLOR_TEXT_DIM, 0.64f, summary);

  float row_y = layout.game_first_row_y;
  for (int i = start; i < end; ++i) {
    const UiGameEntry *game = &state->games[i];
    char full_title[ROMM_GAME_TITLE_LEN + ROMM_GAME_ID_LEN + 8];
    const char *resolved_title = has_text(game->title) ? game->title : game->game_id;
    snprintf(full_title, sizeof(full_title), "%s [%s]", resolved_title, game->game_id);

    ui_draw_game_row(
        layout.game_row_x,
        row_y,
        layout.game_row_w,
        UI_GAME_ROW_HEIGHT,
        state->selected_index == (UI_SELECT_GAME_BASE + i),
        state->active_game_index == i,
        full_title,
        game->card_count);
    row_y += UI_GAME_ROW_HEIGHT;
  }
}

/*
 * Renders the footer status strip and controls hint.
 */
static void ui_render_footer(const UiAppState *state) {
  if (state == NULL) {
    return;
  }

  UiMainLayout layout;
  ui_build_main_layout(&layout);

  char controls_hint[96];
  snprintf(
      controls_hint,
      sizeof(controls_hint),
      "D-Pad/Left Stick move   %s select/apply   START quit",
      ui_dialog_confirm_button_label());

  ui_draw_text(32.0f, 522.0f, UI_COLOR_TEXT_DIM, 0.78f, "Status");
  ui_draw_truncated_text(layout.footer_status_x, 522.0f, layout.footer_status_w, UI_COLOR_STATUS, 0.72f, state->status_line);
  ui_draw_truncated_text_right(
      layout.footer_hint_right_x,
      522.0f,
      layout.footer_hint_w,
      UI_COLOR_TEXT_MUTED,
      0.66f,
      controls_hint);
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
 * Renders one background frame while a blocking message dialog is active.
 * This avoids blank-frame flicker during confirm prompts.
 */
static void ui_render_dialog_background_frame(void *user_data) {
  UiAppState *state = (UiAppState *)user_data;
  if (state == NULL) {
    return;
  }

  ui_pump_app_events();

  int previous_common_dialog_active = g_common_dialog_active;
  g_common_dialog_active = 1;
  if (state->sync_feedback.running && (state->sync_feedback.trigger == UI_SYNC_TRIGGER_MANUAL)) {
    ui_render_sync_modal(state);
  } else {
    ui_render_main_screen(state);
  }
  g_common_dialog_active = previous_common_dialog_active;
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
      "Scan complete: %d PS1 games (%d local target%s)",
      state->game_count,
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
 * Estimates how many local PS1 cards remain after the latest-card rule is
 * applied per game. This keeps confirmation copy aligned with actual sync work.
 */
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

/*
 * Finds the slot 1 peer that lost an equal-timestamp tie against a selected
 * slot 0 item so the UI can explain the deterministic fallback to the user.
 */
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

/*
 * Returns one short label for the local latest-card selection outcome.
 */
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

/*
 * Applies the PS1 latest-card rule to the current work list, logs the outcome
 * for the user, and compacts the selected items in place for mapping/sync.
 */
static int ui_prepare_ps1_sync_candidates(
    SyncSaveDescriptor *items,
    int item_count,
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
        "PS1 latest-card rule active: %d sync candidate(s) selected from %d local target(s)",
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
        "PS1 candidate selected: game=%s file=%s slot=%s timestamp=%s unix=%lld reason=%s",
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
 * Captures the current manual-sync modal geometry for rendering and input.
 */
typedef struct UiSyncModalLayout {
  float panel_x;
  float panel_y;
  float panel_w;
  float panel_h;
  float content_x;
  float content_w;
  float title_y;
  float context_y;
  float progress_y;
  float progress_text_y;
  float log_label_y;
  float log_x;
  float log_y;
  float log_w;
  float log_h;
  float scroll_hint_y;
  float message_y;
  float footer_y;
} UiSyncModalLayout;

/*
 * Converts one raw front-touch report into current screen coordinates.
 */
static int ui_touch_report_to_screen(const SceTouchReport *report, float *out_x, float *out_y) {
  if ((report == NULL) || (out_x == NULL) || (out_y == NULL) || !g_touch_front_initialized) {
    return 0;
  }

  float min_x = 0.0f;
  float max_x = 1919.0f;
  float min_y = 0.0f;
  float max_y = 1087.0f;
  if (g_touch_front_panel_info_ready &&
      (g_touch_front_panel_info.maxDispX > g_touch_front_panel_info.minDispX) &&
      (g_touch_front_panel_info.maxDispY > g_touch_front_panel_info.minDispY)) {
    min_x = (float)g_touch_front_panel_info.minDispX;
    max_x = (float)g_touch_front_panel_info.maxDispX;
    min_y = (float)g_touch_front_panel_info.minDispY;
    max_y = (float)g_touch_front_panel_info.maxDispY;
  }

  float normalized_x = ((float)report->x - min_x) / (max_x - min_x);
  float normalized_y = ((float)report->y - min_y) / (max_y - min_y);
  if (normalized_x < 0.0f) {
    normalized_x = 0.0f;
  }
  if (normalized_x > 1.0f) {
    normalized_x = 1.0f;
  }
  if (normalized_y < 0.0f) {
    normalized_y = 0.0f;
  }
  if (normalized_y > 1.0f) {
    normalized_y = 1.0f;
  }

  *out_x = normalized_x * UI_SCREEN_WIDTH;
  *out_y = normalized_y * UI_SCREEN_HEIGHT;
  return 1;
}

/*
 * Computes the current sync modal layout with wider safe margins and wrapped text.
 */
static void ui_build_sync_modal_layout(const UiSyncFeedback *feedback, UiSyncModalLayout *layout) {
  if ((feedback == NULL) || (layout == NULL)) {
    return;
  }

  memset(layout, 0, sizeof(*layout));
  layout->panel_x = 40.0f;
  layout->panel_y = 22.0f;
  layout->panel_w = 880.0f;
  layout->panel_h = 500.0f;
  layout->content_x = layout->panel_x + 28.0f;
  layout->content_w = layout->panel_w - 56.0f;

  float cursor_y = layout->panel_y + 28.0f;

  int title_lines = ui_wrap_text_lines(
      has_text(feedback->title) ? feedback->title : "Synchronization",
      0.94f,
      layout->content_w,
      NULL,
      2,
      1);
  if (title_lines < 1) {
    title_lines = 1;
  }
  if (title_lines > 2) {
    title_lines = 2;
  }

  layout->title_y = cursor_y;
  cursor_y += (ui_estimate_text_height(0.94f) + 6.0f) * (float)title_lines;

  int context_lines = 0;
  if (has_text(feedback->context)) {
    cursor_y += 2.0f;
    layout->context_y = cursor_y;
    context_lines = ui_wrap_text_lines(feedback->context, 0.72f, layout->content_w, NULL, 2, 1);
    if (context_lines < 1) {
      context_lines = 1;
    }
    if (context_lines > 2) {
      context_lines = 2;
    }
    cursor_y += (ui_estimate_text_height(0.72f) + 4.0f) * (float)context_lines;
  } else {
    layout->context_y = cursor_y;
  }

  cursor_y += 14.0f;
  layout->progress_y = cursor_y;
  cursor_y += 30.0f;
  layout->progress_text_y = cursor_y;
  cursor_y += 22.0f;
  layout->log_label_y = cursor_y;
  cursor_y += 12.0f;

  layout->log_x = layout->content_x;
  layout->log_y = cursor_y;
  layout->log_w = layout->content_w;

  float footer_height = ui_estimate_text_height(0.68f);
  float message_height = (ui_estimate_text_height(0.76f) + 4.0f) * 2.0f;
  float scroll_hint_height = ui_estimate_text_height(0.62f);
  layout->footer_y = layout->panel_y + layout->panel_h - 24.0f;
  layout->message_y = layout->footer_y - footer_height - 12.0f - message_height;
  layout->scroll_hint_y = layout->message_y - 10.0f - scroll_hint_height;
  layout->log_h = layout->scroll_hint_y - 14.0f - layout->log_y;
  if (layout->log_h < 110.0f) {
    layout->log_h = 110.0f;
    layout->scroll_hint_y = layout->log_y + layout->log_h + 14.0f;
    layout->message_y = layout->scroll_hint_y + scroll_hint_height + 10.0f;
  }
}

/*
 * Returns the maximum wrapped-line scroll offset for the current modal viewport.
 */
static int ui_sync_modal_max_scroll(const UiSyncModalLayout *layout) {
  if (layout == NULL) {
    return 0;
  }

  int visible_lines = ui_log_viewport_visible_lines(layout->log_h);
  int total_visual_lines = ui_log_total_visual_lines(layout->log_w);
  int max_scroll = total_visual_lines - visible_lines;
  if (max_scroll < 0) {
    max_scroll = 0;
  }
  return max_scroll;
}

/*
 * Updates modal scroll position while keeping auto-scroll enabled only at the bottom.
 */
static void ui_sync_modal_scroll_by(UiSyncFeedback *feedback, const UiSyncModalLayout *layout, int delta_lines) {
  if ((feedback == NULL) || (layout == NULL)) {
    return;
  }

  int max_scroll = ui_sync_modal_max_scroll(layout);
  int next_scroll = clamp_int(feedback->modal_log_scroll + delta_lines, 0, max_scroll);
  feedback->modal_log_scroll = next_scroll;
  feedback->modal_auto_scroll = (next_scroll >= max_scroll) ? 1 : 0;
}

/*
 * Clears any active touchscreen drag state for the sync modal.
 */
static void ui_sync_modal_reset_touch(UiSyncFeedback *feedback) {
  if (feedback == NULL) {
    return;
  }

  feedback->modal_touch_active = 0;
  feedback->modal_touch_id = -1;
  feedback->modal_touch_last_y = 0.0f;
  feedback->modal_touch_scroll_remainder = 0.0f;
}

/*
 * Applies held-button repeat scrolling inside the sync modal.
 */
static void ui_sync_modal_handle_controller_scroll(
    UiSyncFeedback *feedback,
    const UiSyncModalLayout *layout,
    unsigned int buttons) {
  if ((feedback == NULL) || (layout == NULL)) {
    return;
  }

  int direction = 0;
  if (buttons & SCE_CTRL_DOWN) {
    direction = 1;
  } else if (buttons & SCE_CTRL_UP) {
    direction = -1;
  }

  if (direction == 0) {
    feedback->modal_scroll_hold_direction = 0;
    feedback->modal_scroll_hold_frames = 0;
    return;
  }

  if (feedback->modal_scroll_hold_direction != direction) {
    feedback->modal_scroll_hold_direction = direction;
    feedback->modal_scroll_hold_frames = 0;
    ui_sync_modal_scroll_by(feedback, layout, direction);
    return;
  }

  feedback->modal_scroll_hold_frames += 1;
  if ((feedback->modal_scroll_hold_frames >= UI_SCROLL_REPEAT_DELAY_FRAMES) &&
      (((feedback->modal_scroll_hold_frames - UI_SCROLL_REPEAT_DELAY_FRAMES) % UI_SCROLL_REPEAT_INTERVAL_FRAMES) == 0)) {
    ui_sync_modal_scroll_by(feedback, layout, direction);
  }
}

/*
 * Applies front-touch drag scrolling inside the sync modal log viewport.
 */
static void ui_sync_modal_handle_touch_scroll(UiSyncFeedback *feedback, const UiSyncModalLayout *layout) {
  if ((feedback == NULL) || (layout == NULL) || !g_touch_front_initialized) {
    ui_sync_modal_reset_touch(feedback);
    return;
  }

  SceTouchData touch_data;
  memset(&touch_data, 0, sizeof(touch_data));
  int peek_status = sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch_data, 1);
  if ((peek_status < 0) || (touch_data.reportNum == 0U)) {
    ui_sync_modal_reset_touch(feedback);
    return;
  }

  int matched_report = -1;
  float matched_x = 0.0f;
  float matched_y = 0.0f;

  for (unsigned int i = 0; i < touch_data.reportNum; ++i) {
    float screen_x = 0.0f;
    float screen_y = 0.0f;
    if (!ui_touch_report_to_screen(&touch_data.report[i], &screen_x, &screen_y)) {
      continue;
    }

    if (feedback->modal_touch_active && (feedback->modal_touch_id == (int)touch_data.report[i].id)) {
      matched_report = (int)i;
      matched_x = screen_x;
      matched_y = screen_y;
      break;
    }

    if (!feedback->modal_touch_active &&
        (screen_x >= layout->log_x) &&
        (screen_x <= (layout->log_x + layout->log_w)) &&
        (screen_y >= layout->log_y) &&
        (screen_y <= (layout->log_y + layout->log_h))) {
      feedback->modal_touch_active = 1;
      feedback->modal_touch_id = (int)touch_data.report[i].id;
      feedback->modal_touch_last_y = screen_y;
      feedback->modal_touch_scroll_remainder = 0.0f;
      matched_report = (int)i;
      matched_x = screen_x;
      matched_y = screen_y;
      break;
    }
  }

  if (matched_report < 0) {
    ui_sync_modal_reset_touch(feedback);
    return;
  }

  if (!feedback->modal_touch_active) {
    return;
  }

  float delta_y = feedback->modal_touch_last_y - matched_y;
  if (fabsf(delta_y) < UI_TOUCH_DRAG_DEADZONE) {
    return;
  }

  feedback->modal_touch_scroll_remainder += delta_y;
  int scroll_lines = (int)(feedback->modal_touch_scroll_remainder / UI_LOG_LINE_HEIGHT);
  if (scroll_lines != 0) {
    ui_sync_modal_scroll_by(feedback, layout, scroll_lines);
    feedback->modal_touch_scroll_remainder -= (float)scroll_lines * UI_LOG_LINE_HEIGHT;
  }
  feedback->modal_touch_last_y = matched_y;
  (void)matched_x;
}

/*
 * Processes held controller input and optional touch dragging for the manual-sync modal.
 */
static void ui_sync_modal_handle_input(UiAppState *state) {
  if (state == NULL) {
    return;
  }

  UiSyncModalLayout layout;
  ui_build_sync_modal_layout(&state->sync_feedback, &layout);

  ui_sync_modal_handle_controller_scroll(&state->sync_feedback, &layout, ui_poll_buttons());
  ui_sync_modal_handle_touch_scroll(&state->sync_feedback, &layout);
}

/*
 * Renders the blocking manual-sync modal with progress and scrolling log area.
 */
static void ui_render_sync_modal(UiAppState *state) {
  if (state == NULL) {
    return;
  }

  UiSyncFeedback *feedback = &state->sync_feedback;
  UiSyncModalLayout layout;
  ui_build_sync_modal_layout(feedback, &layout);

  float ratio = 0.0f;
  if (feedback->total_units > 0) {
    ratio = (float)feedback->completed_units / (float)feedback->total_units;
  }

  int total_visual_lines = ui_log_total_visual_lines(layout.log_w);
  int visible_lines = ui_log_viewport_visible_lines(layout.log_h);
  int max_scroll = total_visual_lines - visible_lines;
  if (max_scroll < 0) {
    max_scroll = 0;
  }
  if (feedback->modal_auto_scroll) {
    feedback->modal_log_scroll = max_scroll;
  }
  feedback->modal_log_scroll = clamp_int(feedback->modal_log_scroll, 0, max_scroll);

  ui_begin_frame();
  ui_draw_panel(layout.panel_x, layout.panel_y, layout.panel_w, layout.panel_h, UI_COLOR_PANEL, UI_COLOR_PANEL_BORDER_ACTIVE);
  ui_draw_wrapped_text_block(
      layout.content_x,
      layout.title_y,
      layout.content_w,
      UI_COLOR_TEXT,
      0.94f,
      6.0f,
      2,
      has_text(feedback->title) ? feedback->title : "Synchronization");
  if (has_text(feedback->context)) {
    ui_draw_wrapped_text_block(
        layout.content_x,
        layout.context_y,
        layout.content_w,
        UI_COLOR_TEXT_MUTED,
        0.72f,
        4.0f,
        2,
        feedback->context);
  }

  ui_draw_progress_bar(layout.content_x, layout.progress_y, layout.content_w, 18.0f, ratio);
  ui_draw_text(
      layout.content_x,
      layout.progress_text_y,
      UI_COLOR_TEXT_MUTED,
      0.72f,
      "%d%% (%d/%d)",
      (int)(ratio * 100.0f),
      feedback->completed_units,
      feedback->total_units);

  ui_draw_text(layout.content_x, layout.log_label_y, UI_COLOR_TEXT, 0.82f, "Live logs");
  ui_draw_log_viewport(layout.log_x, layout.log_y, layout.log_w, layout.log_h, feedback->modal_log_scroll);

  if (max_scroll > 0) {
    int visible_end = feedback->modal_log_scroll + visible_lines;
    if (visible_end > total_visual_lines) {
      visible_end = total_visual_lines;
    }
    ui_draw_text(
        layout.content_x,
        layout.scroll_hint_y,
        UI_COLOR_TEXT_DIM,
        0.64f,
        "Hold UP/DOWN or drag the touchscreen to scroll %d-%d/%d",
        feedback->modal_log_scroll + 1,
        visible_end,
        total_visual_lines);
  }

  unsigned int result_color = UI_COLOR_TEXT_MUTED;
  if (feedback->running) {
    result_color = UI_COLOR_ACCENT;
  } else if (feedback->success) {
    result_color = UI_COLOR_SUCCESS;
  } else {
    result_color = UI_COLOR_DANGER;
  }

  ui_draw_wrapped_text_block(
      layout.content_x,
      layout.message_y,
      layout.content_w,
      result_color,
      0.76f,
      4.0f,
      2,
      has_text(feedback->message) ? feedback->message : "");
  ui_draw_text(
      layout.content_x,
      layout.footer_y,
      UI_COLOR_TEXT_DIM,
      0.68f,
      feedback->running ? "Synchronization is running..." : "CIRCLE, CROSS, or START: close");
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
    ui_sync_modal_handle_input(state);
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

typedef struct UiSyncConflictResolutionContext {
  UiAppState *state;
  UiSyncTrigger trigger;
  int dry_run;
  int auto_apply_conflicts;
} UiSyncConflictResolutionContext;

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
  ui_sync_render_live(state);
}

/*
 * Formats a Unix timestamp for conflict prompts using local device time when available.
 */
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

/*
 * Returns a short user-facing explanation for one conflict kind.
 */
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

/*
 * Returns a short user-facing phrase for a recommended sync action.
 */
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

/*
 * Builds one concise multi-line conflict prompt message for the system dialog,
 * including the current Vita confirm/decline button mapping.
 */
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

/*
 * Resolves one conflict through either automatic recommended-action handling
 * or blocking UI prompts, depending on the current sync settings.
 */
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
        prompt,
        sizeof(prompt),
        local_item,
        remote_item,
        conflict,
        SYNC_ACTION_UPLOAD,
        context->dry_run,
        "Upload the local save to RomM?",
        "show the download option");
    response = ui_dialog_confirm(prompt);
    if (response < 0) {
      if (candidate_action != NULL) {
        snprintf(candidate_action->reason, sizeof(candidate_action->reason), "conflict dialog failed during upload choice");
      }
      ui_sync_log_write(APP_LOG_LEVEL_ERROR, "Conflict dialog failed for file=%s", filename);
      return SYNC_ACTION_SKIP;
    }
    if (response == 1) {
      ui_sync_log_write(
          APP_LOG_LEVEL_INFO,
          "%s approved: file=%s action=upload",
          context->dry_run ? "Dry-run conflict plan" : "Conflict action",
          filename);
      return SYNC_ACTION_UPLOAD;
    }

    ui_build_conflict_prompt_message(
        prompt,
        sizeof(prompt),
        local_item,
        remote_item,
        conflict,
        SYNC_ACTION_DOWNLOAD,
        context->dry_run,
        "Download the remote save to this Vita?",
        "skip this save");
    response = ui_dialog_confirm(prompt);
    if (response < 0) {
      if (candidate_action != NULL) {
        snprintf(candidate_action->reason, sizeof(candidate_action->reason), "conflict dialog failed during download choice");
      }
      ui_sync_log_write(APP_LOG_LEVEL_ERROR, "Conflict dialog failed for file=%s", filename);
      return SYNC_ACTION_SKIP;
    }
    if (response == 1) {
      ui_sync_log_write(
          APP_LOG_LEVEL_INFO,
          "%s approved: file=%s action=download",
          context->dry_run ? "Dry-run conflict plan" : "Conflict action",
          filename);
      return SYNC_ACTION_DOWNLOAD;
    }

    if (candidate_action != NULL) {
      snprintf(
          candidate_action->reason,
          sizeof(candidate_action->reason),
          "user skipped after reviewing same-content conflict");
    }
    ui_sync_log_write(
        APP_LOG_LEVEL_INFO,
        "Conflict skipped by user: file=%s conflict=%s alternatives=upload,download",
        filename,
        sync_conflict_type_str(conflict));
    return SYNC_ACTION_SKIP;
  }

  ui_build_conflict_prompt_message(
      prompt,
      sizeof(prompt),
      local_item,
      remote_item,
      conflict,
      recommended_action,
      context->dry_run,
      "Apply the recommended action?",
      "skip this save");
  response = ui_dialog_confirm(prompt);
  if (response < 0) {
    if (candidate_action != NULL) {
      snprintf(candidate_action->reason, sizeof(candidate_action->reason), "conflict dialog failed");
    }
    ui_sync_log_write(APP_LOG_LEVEL_ERROR, "Conflict dialog failed for file=%s", filename);
    return SYNC_ACTION_SKIP;
  }

  if (response == 1) {
    ui_sync_log_write(
        APP_LOG_LEVEL_INFO,
        "%s approved: file=%s action=%s",
        context->dry_run ? "Dry-run conflict plan" : "Conflict action",
        filename,
        sync_action_type_str(recommended_action));
    return recommended_action;
  }

  if (candidate_action != NULL) {
    snprintf(
        candidate_action->reason,
        sizeof(candidate_action->reason),
        "user skipped after reviewing conflict=%s",
        sync_conflict_type_str(conflict));
  }
  ui_sync_log_write(
      APP_LOG_LEVEL_INFO,
      "Conflict skipped by user: file=%s conflict=%s recommended=%s",
      filename,
      sync_conflict_type_str(conflict),
      sync_action_type_str(recommended_action));
  return SYNC_ACTION_SKIP;
}

/*
 * Keeps the completed manual sync modal open until the user closes it.
 */
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
      &selection_warning_count);
  if (work_item_count <= 0) {
    ui_sync_log_write(APP_LOG_LEVEL_ERROR, "Sync failed: no PS1 sync candidate was selected");
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
    ui_sync_log_write(APP_LOG_LEVEL_ERROR, "Sync failed: username/password is missing");
    ui_sync_feedback_set_message(&state->sync_feedback, "Sync failed: username/password is missing");
    state->sync_feedback.running = 0;
    state->sync_feedback.completed = 1;
    state->sync_feedback.success = 0;
    state->sync_feedback.sync_status = SYNC_ENGINE_ERR_INVALID_ARGUMENT;
    ui_sync_feedback_set_progress(&state->sync_feedback, total_units, total_units);
    ui_sync_render_live(state);
    return SYNC_ENGINE_ERR_INVALID_ARGUMENT;
  }
  ui_sync_feedback_set_progress(&state->sync_feedback, 1, total_units);
  ui_sync_log_write(APP_LOG_LEVEL_INFO, "Username/password present in configuration");
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
    ui_sync_log_write(
        APP_LOG_LEVEL_ERROR,
        "Sync aborted: %d save(s) have no rom_id after mapping",
        unresolved_count);
    ui_sync_feedback_set_message(&state->sync_feedback, "Sync failed: unresolved RomM mapping");
    state->sync_feedback.running = 0;
    state->sync_feedback.completed = 1;
    state->sync_feedback.success = 0;
    state->sync_feedback.sync_status = SYNC_ENGINE_ERR_UNRESOLVED_ROM_ID;
    ui_sync_feedback_set_progress(&state->sync_feedback, total_units, total_units);
    ui_sync_render_live(state);
    return SYNC_ENGINE_ERR_UNRESOLVED_ROM_ID;
  }
  ui_sync_feedback_set_progress(&state->sync_feedback, 3, total_units);
  ui_sync_render_live(state);

  /* Conflict auto-apply is always enabled to keep sync flow deterministic. */
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
  ui_sync_log_write(
      APP_LOG_LEVEL_INFO,
      "Auto-apply conflicts enabled: recommended actions will execute without confirmation");

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
    if (selection_warning_count > 0) {
      ui_sync_feedback_set_message(&state->sync_feedback, "Sync completed with warnings.");
    } else {
      ui_sync_feedback_set_message(&state->sync_feedback, "Sync completed successfully.");
    }
  } else {
    char failure_message[96];
    snprintf(
        failure_message,
        sizeof(failure_message),
        "Sync failed: %s.",
        sync_engine_status_str(sync_status));
    ui_sync_log_write(
        APP_LOG_LEVEL_ERROR,
        "Sync failed: %s (%d)",
        sync_engine_status_str(sync_status),
        sync_status);
    ui_sync_feedback_set_message(&state->sync_feedback, failure_message);
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
  int sync_candidate_count = ui_estimate_ps1_sync_candidate_count(
      state->sync_work_items,
      game_item_count);

  char confirm_msg[256];
  snprintf(
      confirm_msg,
      sizeof(confirm_msg),
      "Synchronize %s?\n%d sync candidate(s) selected from %d local target(s).",
      has_text(game->title) ? game->title : game->game_id,
      sync_candidate_count,
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
      "Game: %s (%d sync candidate%s)",
      game->game_id,
      sync_candidate_count,
      (sync_candidate_count == 1) ? "" : "s");

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
  int sync_candidate_count = ui_estimate_ps1_sync_candidate_count(
      state->sync_work_items,
      work_item_count);

  char confirm_msg[256];
  snprintf(
      confirm_msg,
      sizeof(confirm_msg),
      "Synchronize all detected PS1 saves?\n%d sync candidate(s) selected from %d local target(s) across %d game(s).",
      sync_candidate_count,
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
      "All games: %d sync candidate%s across %d game(s)",
      sync_candidate_count,
      (sync_candidate_count == 1) ? "" : "s",
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
    ui_set_status(state, "Auto sync skipped: configure the RomM URL, username, and password first");
    return;
  }

  int work_item_count = state->local_count;
  if (work_item_count > (int)(sizeof(state->sync_work_items) / sizeof(state->sync_work_items[0]))) {
    work_item_count = (int)(sizeof(state->sync_work_items) / sizeof(state->sync_work_items[0]));
  }
  memcpy(state->sync_work_items, state->local_items, sizeof(state->sync_work_items[0]) * (size_t)work_item_count);
  int sync_candidate_count = ui_estimate_ps1_sync_candidate_count(
      state->sync_work_items,
      work_item_count);

  char context[UI_STATUS_LINE_LEN];
  snprintf(
      context,
      sizeof(context),
      "Startup auto sync: %d sync candidate%s",
      sync_candidate_count,
      (sync_candidate_count == 1) ? "" : "s");

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
    int secret_mode,
    int trim_whitespace) {
  if ((state == NULL) || !has_text(label) || (field == NULL) || (field_size == 0U)) {
    return;
  }

  char previous_value[UI_EDITOR_BUFFER_LEN];
  snprintf(previous_value, sizeof(previous_value), "%s", field);

  unsigned int textbox_mode = secret_mode
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

  if (state->selected_index == UI_SELECT_DRY_RUN) {
    int previous_dry_run = state->config.sync_dry_run;
    state->config.sync_dry_run = state->config.sync_dry_run ? 0 : 1;
    int save_status = ui_save_config(state, state->config.sync_dry_run ? "Dry-run enabled" : "Dry-run disabled");
    if (save_status != APP_CONFIG_OK) {
      state->config.sync_dry_run = previous_dry_run;
      return;
    }

    if (save_status == APP_CONFIG_OK) {
      app_log_write(
          APP_LOG_LEVEL_INFO,
          "ui",
          "dry-run %s from the home screen",
          state->config.sync_dry_run ? "enabled" : "disabled");
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

  /* Backup coverage is mandatory, so conflict auto-apply stays enabled and hidden. */
  state->config.sync_auto_apply_conflicts = 1;
  ui_apply_logging_preferences(&state->config);

  if (state->config_status == APP_CONFIG_ERR_NOT_FOUND) {
    app_log_write(APP_LOG_LEVEL_WARN, "main", "settings.ini not found, using defaults");
    ui_set_status(state, "Connection settings not found. Enter the server URL, username, and password.");
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
  state->nav_hold_direction = UI_NAV_NONE;
  state->nav_hold_frames = 0;
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

  sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
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
  ui_touch_init();

  UiAppState *state = &g_app_state;
  ui_initialize_state(state);
  ui_dialog_set_frame_callback(ui_render_dialog_background_frame, state);

  unsigned int previous_buttons = ui_poll_buttons();
  for (;;) {
    ui_run_pending_auto_sync(state);
    ui_pump_app_events();
    ui_clamp_selection(state);
    ui_update_game_scroll(state);
    ui_render_main_screen(state);

    UiControllerState controller = ui_poll_controller_state();
    unsigned int pressed = ui_compute_pressed(controller.buttons, &previous_buttons);
    if (pressed & SCE_CTRL_START) {
      break;
    }

    ui_handle_navigation_input(state, controller.buttons, controller.left_x, controller.left_y);
    if (state->selected_index >= UI_SELECT_GAME_BASE) {
      state->active_game_index = state->selected_index - UI_SELECT_GAME_BASE;
    }
    if (pressed & ui_primary_action_button()) {
      ui_activate_selection(state);
      previous_buttons = ui_poll_buttons();
      state->nav_hold_direction = UI_NAV_NONE;
      state->nav_hold_frames = 0;
    }

    sceKernelDelayThread(16 * 1000);
  }

  ui_render_exit_screen();
  sceKernelDelayThread(400 * 1000);
  ui_touch_term();
  ui_dialog_runtime_term();
  ui_renderer_term();
  return 0;
}
