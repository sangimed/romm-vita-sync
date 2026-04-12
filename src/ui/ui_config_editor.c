#include "ui_config_editor.h"

#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/system_param.h>
#include <psp2/touch.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "app_log.h"
#include "backup_manager.h"
#include "romm_client.h"
#include "ui_common.h"
#include "ui_navigation.h"
#include "ui_render.h"
#include "ui_screens.h"
#include "ui_sync_orchestrator.h"

extern int g_dialog_runtime_initialized;
extern int g_common_dialog_active;
extern int g_ime_module_loaded;
extern int g_touch_front_initialized;
extern int g_touch_front_panel_info_ready;
extern SceTouchPanelInfo g_touch_front_panel_info;
extern SceSystemParamEnterButtonAssign g_dialog_enter_button_assign;

int ui_dialog_runtime_init(void) {
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
  app_log_write(APP_LOG_LEVEL_DEBUG, "ui", "common dialog buttons confirm=%s decline=%s",
      ui_dialog_confirm_button_label(), ui_dialog_decline_button_label());

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

void ui_dialog_runtime_term(void) {
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

void ui_touch_init(void) {
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

void ui_touch_term(void) {
  if (!g_touch_front_initialized) {
    return;
  }

  sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_STOP);
  g_touch_front_initialized = 0;
  g_touch_front_panel_info_ready = 0;
  memset(&g_touch_front_panel_info, 0, sizeof(g_touch_front_panel_info));
}

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

void ui_apply_logging_preferences(const AppConfig *config) {
  if (config == NULL) {
    app_log_set_level(APP_LOG_LEVEL_INFO);
    app_log_set_file_output(0, APP_RUNTIME_LOG_FILE_PATH);
    return;
  }

  app_log_set_level(app_log_level_from_config(config->log_level));
  app_log_set_file_output(config->log_file_enabled, APP_RUNTIME_LOG_FILE_PATH);
}

int ui_save_config(UiAppState *state, const char *success_message) {
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

int ensure_device_registration(AppConfig *config, const RommClient *romm_client) {
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
      app_log_write(APP_LOG_LEVEL_ERROR, "ui",
          "IME edit requested but dialog runtime init failed: 0x%08X", (unsigned int)runtime_status);
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

void ui_activate_selection(UiAppState *state) {
  if (state == NULL) {
    return;
  }

  if (state->selected_index == UI_SELECT_SERVER_URL) {
    ui_edit_config_field(state, "Server URL", state->config.romm_url,
        sizeof(state->config.romm_url), SCE_IME_TYPE_URL, 0, 1);
    return;
  }

  if (state->selected_index == UI_SELECT_USERNAME) {
    ui_edit_config_field(state, "Username", state->config.romm_username,
        sizeof(state->config.romm_username), SCE_IME_TYPE_BASIC_LATIN, 0, 1);
    return;
  }

  if (state->selected_index == UI_SELECT_PASSWORD) {
    ui_edit_config_field(state, "Password", state->config.romm_password,
        sizeof(state->config.romm_password), SCE_IME_TYPE_BASIC_LATIN, 1, 0);
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
      app_log_write(APP_LOG_LEVEL_INFO, "ui",
          "dry-run %s from the home screen", state->config.sync_dry_run ? "enabled" : "disabled");
    }
    return;
  }

  if (state->selected_index == UI_SELECT_SYNC_PRIMARY) {
    if (ui_selected_game_count(state) <= 0) {
      ui_set_status(state, "Check at least one PS1 game before synchronizing");
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
    ui_run_sync_for_selected_games(state);
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
    state->games[game_index].selected_for_sync = state->games[game_index].selected_for_sync ? 0 : 1;
    ui_set_status(state, "%s %s for synchronization",
        state->games[game_index].selected_for_sync ? "Checked" : "Unchecked",
        state->games[game_index].game_id);
  }
}
