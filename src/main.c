#include <psp2/apputil.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/system_param.h>
#include <psp2/touch.h>

#include <string.h>

#include <vita2d.h>

#include "debugScreen.h"

#include "app_config.h"
#include "app_log.h"
#include "backup_manager.h"
#include "romm_client.h"
#include "romm_http_client.h"
#include "scan_to_sync_adapter.h"
#include "save_scanner.h"
#include "sync_engine.h"
#include "ui/ui_common.h"
#include "ui/ui_config_editor.h"
#include "ui/ui_navigation.h"
#include "ui/ui_render.h"
#include "ui/ui_screens.h"
#include "ui/ui_sync_modal.h"
#include "ui/ui_sync_orchestrator.h"
#include "ui_dialogs.h"

/*
 * Global state shared across UI modules via extern declarations.
 */
UiAppState g_app_state;
vita2d_pgf *g_ui_font = NULL;
int g_common_dialog_active = 0;
int g_dialog_runtime_initialized = 0;
SceSystemParamEnterButtonAssign g_dialog_enter_button_assign = SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
int g_ime_module_loaded = 0;
int g_touch_front_initialized = 0;
int g_touch_front_panel_info_ready = 0;
SceTouchPanelInfo g_touch_front_panel_info;

/*
 * Ensures the app runtime directory exists on first launch.
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

  state->config.sync_auto_apply_conflicts = 1;
  ui_apply_logging_preferences(&state->config);

  if (state->config_status == APP_CONFIG_ERR_NOT_FOUND) {
    app_log_write(APP_LOG_LEVEL_WARN, "main", "settings.ini not found, using defaults");
    ui_set_status(state, "Connection settings not found. Enter the server URL and API token (or username/password).");
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
