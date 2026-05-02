#include "ui_sync_modal.h"

#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui_common.h"
#include "ui_navigation.h"
#include "ui_render.h"
#include "ui_screens.h"

extern int g_touch_front_initialized;
extern int g_touch_front_panel_info_ready;
extern SceTouchPanelInfo g_touch_front_panel_info;

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

void ui_build_sync_modal_layout(const UiSyncFeedback *feedback, UiSyncModalLayout *layout) {
  if ((feedback == NULL) || (layout == NULL)) {
    return;
  }

  memset(layout, 0, sizeof(*layout));
  layout->panel_x = 48.0f;
  layout->panel_y = 34.0f;
  layout->panel_w = 864.0f;
  layout->panel_h = 478.0f;
  layout->content_x = layout->panel_x + 28.0f;
  layout->content_w = layout->panel_w - 56.0f;

  float cursor_y = layout->panel_y + 42.0f;

  int title_lines = ui_wrap_text_lines(
      has_text(feedback->title) ? feedback->title : "Synchronization",
      0.98f,
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
  cursor_y += (ui_estimate_text_height(0.98f) + 6.0f) * (float)title_lines;

  int context_lines = 0;
  if (has_text(feedback->context)) {
    cursor_y += 4.0f;
    layout->context_y = cursor_y;
    context_lines = ui_wrap_text_lines(feedback->context, 0.78f, layout->content_w, NULL, 2, 1);
    if (context_lines < 1) {
      context_lines = 1;
    }
    if (context_lines > 2) {
      context_lines = 2;
    }
    cursor_y += (ui_estimate_text_height(0.78f) + 4.0f) * (float)context_lines;
  } else {
    layout->context_y = cursor_y;
  }

  cursor_y += 16.0f;
  layout->message_y = cursor_y;
  cursor_y += (ui_estimate_text_height(0.82f) + 4.0f) * 2.0f;
  cursor_y += 12.0f;
  layout->progress_y = cursor_y;
  cursor_y += 28.0f;
  layout->progress_text_y = cursor_y;
  cursor_y += 28.0f;
  layout->log_label_y = cursor_y;
  cursor_y += 16.0f;

  layout->log_x = layout->content_x;
  layout->log_y = cursor_y;
  layout->log_w = layout->content_w;

  float footer_height = ui_estimate_text_height(0.68f);
  float scroll_hint_height = ui_estimate_text_height(0.62f);
  layout->footer_y = layout->panel_y + layout->panel_h - 22.0f;
  layout->scroll_hint_y = layout->footer_y - footer_height - 10.0f - scroll_hint_height;
  layout->log_h = layout->scroll_hint_y - 12.0f - layout->log_y;
  if (layout->log_h < 110.0f) {
    layout->log_h = 110.0f;
    layout->scroll_hint_y = layout->log_y + layout->log_h + 14.0f;
  }
}

int ui_sync_modal_max_scroll(const UiSyncModalLayout *layout) {
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

void ui_sync_modal_scroll_by(UiSyncFeedback *feedback, const UiSyncModalLayout *layout, int delta_lines) {
  if ((feedback == NULL) || (layout == NULL)) {
    return;
  }

  int max_scroll = ui_sync_modal_max_scroll(layout);
  int next_scroll = clamp_int(feedback->modal_log_scroll + delta_lines, 0, max_scroll);
  feedback->modal_log_scroll = next_scroll;
  feedback->modal_auto_scroll = (next_scroll >= max_scroll) ? 1 : 0;
}

void ui_sync_modal_reset_touch(UiSyncFeedback *feedback) {
  if (feedback == NULL) {
    return;
  }

  feedback->modal_touch_active = 0;
  feedback->modal_touch_id = -1;
  feedback->modal_touch_last_y = 0.0f;
  feedback->modal_touch_scroll_remainder = 0.0f;
}

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

void ui_sync_modal_handle_input(UiAppState *state) {
  if (state == NULL) {
    return;
  }

  UiSyncModalLayout layout;
  ui_build_sync_modal_layout(&state->sync_feedback, &layout);

  ui_sync_modal_handle_controller_scroll(&state->sync_feedback, &layout, ui_poll_buttons());
  ui_sync_modal_handle_touch_scroll(&state->sync_feedback, &layout);
}

void ui_render_sync_modal(UiAppState *state) {
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
  vita2d_draw_rectangle(layout.panel_x, layout.panel_y, layout.panel_w, 3.0f, UI_COLOR_ACCENT);
  ui_draw_wrapped_text_block(
      layout.content_x,
      layout.title_y,
      layout.content_w,
      UI_COLOR_TEXT,
      0.98f,
      6.0f,
      2,
      has_text(feedback->title) ? feedback->title : "Synchronization");
  if (has_text(feedback->context)) {
    ui_draw_wrapped_text_block(
        layout.content_x,
        layout.context_y,
        layout.content_w,
        UI_COLOR_TEXT_MUTED,
        0.78f,
        4.0f,
        2,
        feedback->context);
  }

  unsigned int result_color = UI_COLOR_TEXT_MUTED;
  if (feedback->running) {
    result_color = UI_COLOR_ACCENT;
  } else if (feedback->success) {
    result_color = UI_COLOR_SUCCESS;
  } else if (feedback->warning) {
    result_color = UI_COLOR_WARNING;
  } else {
    result_color = UI_COLOR_DANGER;
  }

  ui_draw_wrapped_text_block(
      layout.content_x,
      layout.message_y,
      layout.content_w,
      result_color,
      0.82f,
      4.0f,
      2,
      has_text(feedback->message) ? feedback->message : "");

  ui_draw_progress_bar(layout.content_x, layout.progress_y, layout.content_w, 18.0f, ratio);
  ui_draw_text(
      layout.content_x,
      layout.progress_text_y,
      UI_COLOR_TEXT_MUTED,
      0.78f,
      "%d%% (%d/%d)",
      (int)(ratio * 100.0f),
      feedback->completed_units,
      feedback->total_units);

  vita2d_draw_rectangle(layout.content_x, layout.log_label_y - 19.0f, 3.0f, 22.0f, UI_COLOR_TEXT_DIM);
  ui_draw_text(layout.content_x + 14.0f, layout.log_label_y, UI_COLOR_TEXT, 0.82f, "Live logs");
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

  ui_draw_text(
      layout.content_x,
      layout.footer_y,
      UI_COLOR_TEXT_DIM,
      0.68f,
      feedback->running ? "Synchronization is running..." : "CIRCLE, CROSS, or START: close");
  ui_end_frame();
}

void ui_sync_render_live(UiAppState *state) {
  if (state == NULL) {
    return;
  }

  ui_pump_app_events();
  if (state->sync_feedback.trigger == UI_SYNC_TRIGGER_MANUAL) {
    ui_sync_modal_handle_input(state);
    ui_render_sync_modal(state);
  } else {
    ui_render_active_screen(state);
  }
}
