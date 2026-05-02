#include "ui_common.h"

#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/sysmodule.h>
#include <psp2/system_param.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "app_log.h"

extern int g_dialog_runtime_initialized;
extern SceSystemParamEnterButtonAssign g_dialog_enter_button_assign;

int has_text(const char *value) {
  return (value != NULL) && (value[0] != '\0');
}

int clamp_int(int value, int min_value, int max_value) {
  if (value < min_value) {
    return min_value;
  }
  if (value > max_value) {
    return max_value;
  }
  return value;
}

void ui_set_status(UiAppState *state, const char *format, ...) {
  if ((state == NULL) || (format == NULL)) {
    return;
  }

  va_list args;
  va_start(args, format);
  vsnprintf(state->status_line, sizeof(state->status_line), format, args);
  va_end(args);
}

void ui_pump_app_events(void) {
  if (!g_dialog_runtime_initialized) {
    return;
  }

  SceAppUtilAppEventParam app_event;
  memset(&app_event, 0, sizeof(app_event));
  sceAppUtilReceiveAppEvent(&app_event);
}

void ui_build_main_layout(UiMainLayout *layout) {
  if (layout == NULL) {
    return;
  }

  memset(layout, 0, sizeof(*layout));

  layout->connection_x = 48.0f;
  layout->connection_y = 94.0f;
  layout->connection_w = 864.0f;
  layout->connection_h = 398.0f;
  layout->connection_row_x = layout->connection_x + 28.0f;
  layout->connection_row_w = layout->connection_w - 56.0f;
  layout->connection_row_h = 38.0f;
  layout->connection_row_gap = 6.0f;
  layout->connection_first_row_y = layout->connection_y + 62.0f;

  layout->sync_x = 32.0f;
  layout->sync_y = 94.0f;
  layout->sync_w = 624.0f;
  layout->sync_h = 164.0f;
  layout->sync_content_x = layout->sync_x + 22.0f;
  layout->sync_content_w = layout->sync_w - 306.0f;
  layout->sync_button_x = layout->sync_x + layout->sync_w - 258.0f;
  layout->sync_button_w = 236.0f;
  layout->sync_button_h = 36.0f;
  layout->sync_button_gap = 8.0f;
  layout->sync_first_button_y = layout->sync_y + 34.0f;

  layout->settings_x = layout->sync_x + layout->sync_w + 16.0f;
  layout->settings_y = layout->sync_y;
  layout->settings_w = 272.0f;
  layout->settings_h = layout->sync_h;
  layout->settings_button_x = layout->settings_x + 16.0f;
  layout->settings_button_y = layout->settings_y + layout->settings_h - 44.0f;
  layout->settings_button_w = layout->settings_w - 32.0f;
  layout->settings_button_h = 34.0f;

  layout->settings_options_x = layout->connection_x;
  layout->settings_options_y = layout->connection_y;
  layout->settings_options_w = layout->connection_w;
  layout->settings_options_h = layout->connection_h;
  layout->settings_options_row_x = layout->connection_row_x;
  layout->settings_options_row_w = layout->connection_row_w;
  layout->settings_options_row_h = layout->connection_row_h;
  layout->settings_options_row_gap = layout->connection_row_gap;
  layout->settings_options_first_row_y = layout->connection_first_row_y +
                                         ((layout->connection_row_h + layout->connection_row_gap) * 4.0f) +
                                         32.0f;
  layout->settings_back_button_x = layout->settings_options_row_x;
  layout->settings_back_button_y = layout->settings_options_y + layout->settings_options_h - 40.0f;
  layout->settings_back_button_w = layout->settings_options_row_w;
  layout->settings_back_button_h = 34.0f;

  layout->game_x = 32.0f;
  layout->game_y = layout->sync_y + layout->sync_h + 16.0f;
  layout->game_w = 896.0f;
  layout->game_h = 222.0f;
  layout->game_row_x = layout->game_x + 22.0f;
  layout->game_row_w = layout->game_w - 44.0f;
  layout->search_row_x = layout->game_x + 22.0f;
  layout->search_row_y = layout->game_y + 46.0f;
  layout->search_row_w = layout->game_w - 44.0f;
  layout->search_row_h = 38.0f;
  layout->game_first_row_y = layout->search_row_y + layout->search_row_h + 8.0f;

  layout->footer_status_x = 104.0f;
  layout->footer_status_w = 440.0f;
  layout->footer_hint_right_x = 928.0f;
  layout->footer_hint_w = 350.0f;
}

const char *ui_dialog_confirm_button_label(void) {
  return (g_dialog_enter_button_assign == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE) ? "O" : "X";
}

const char *ui_dialog_decline_button_label(void) {
  return (g_dialog_enter_button_assign == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE) ? "X" : "O";
}

unsigned int ui_primary_action_button(void) {
  return (g_dialog_enter_button_assign == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE) ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS;
}

void ui_sync_feedback_reset(
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

void ui_sync_feedback_set_message(UiSyncFeedback *feedback, const char *message) {
  if (feedback == NULL) {
    return;
  }

  snprintf(
      feedback->message,
      sizeof(feedback->message),
      "%s",
      has_text(message) ? message : "");
}

void ui_sync_feedback_set_progress(UiSyncFeedback *feedback, int completed_units, int total_units) {
  if (feedback == NULL) {
    return;
  }

  if (total_units <= 0) {
    total_units = 1;
  }

  feedback->total_units = total_units;
  feedback->completed_units = clamp_int(completed_units, 0, total_units);
}

void ui_sync_log_write(AppLogLevel level, const char *format, ...) {
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
