#include "ui_screens.h"

#include <stdio.h>
#include <string.h>

#include "ui_common.h"
#include "ui_navigation.h"
#include "ui_render.h"
#include "ui_sync_modal.h"

extern int g_common_dialog_active;

void ui_render_header(const UiAppState *state) {
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
      "Configure the RoMM server, check one or more PS1 games, then start a manual or startup synchronization.");

  ui_draw_text_right(928.0f, 28.0f, UI_COLOR_TEXT_DIM, 0.72f, "Status");
  ui_draw_truncated_text_right(928.0f, 52.0f, 240.0f, status_color, 0.76f, status_text);
}

void ui_render_connection_panel(const UiAppState *state) {
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

void ui_render_sync_panel(const UiAppState *state) {
  if (state == NULL) {
    return;
  }

  UiMainLayout layout;
  ui_build_main_layout(&layout);

  const UiGameEntry *first_selected_game = ui_first_selected_game(state);
  int selected_games = ui_selected_game_count(state);
  char title_display[ROMM_GAME_TITLE_LEN + 16];
  char detail[128];
  char readiness[UI_STATUS_LINE_LEN];
  unsigned int readiness_color = UI_COLOR_SUCCESS;
  int sync_enabled = ui_sync_action_enabled(state);
  int sync_all_enabled = ui_sync_all_action_enabled(state);

  if (selected_games > 0) {
    int selected_targets = 0;
    for (int i = 0; i < state->game_count; ++i) {
      if (state->games[i].selected_for_sync) {
        selected_targets += state->games[i].save_count;
      }
    }

    if ((selected_games == 1) && (first_selected_game != NULL)) {
      const char *resolved_title = has_text(first_selected_game->title)
                                       ? first_selected_game->title
                                       : first_selected_game->game_id;
      snprintf(title_display, sizeof(title_display), "%s", resolved_title);
      snprintf(
          detail,
          sizeof(detail),
          "1 game checked | %d local target%s",
          selected_targets,
          (selected_targets == 1) ? "" : "s");
    } else {
      snprintf(title_display, sizeof(title_display), "%d games checked", selected_games);
      snprintf(
          detail,
          sizeof(detail),
          "%d local target%s currently selected",
          selected_targets,
          (selected_targets == 1) ? "" : "s");
    }
  } else {
    snprintf(title_display, sizeof(title_display), "No game checked");
    snprintf(detail, sizeof(detail), "Open Detected PS1 Games and check one or more entries to sync.");
  }

  if (state->sync_feedback.running) {
    snprintf(readiness, sizeof(readiness), "%s", state->sync_feedback.message);
    readiness_color = UI_COLOR_ACCENT;
  } else if (state->sync_feedback.completed) {
    snprintf(readiness, sizeof(readiness), "%s", state->sync_feedback.message);
    readiness_color = state->sync_feedback.success ? UI_COLOR_SUCCESS : UI_COLOR_DANGER;
  } else if (state->game_count <= 0) {
    snprintf(readiness, sizeof(readiness), "No local PS1 saves detected yet.");
    readiness_color = UI_COLOR_WARNING;
  } else if (selected_games <= 0) {
    snprintf(readiness, sizeof(readiness), "Check one or more PS1 games in the list to enable sync.");
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
      selected_games > 0 ? UI_COLOR_TEXT : UI_COLOR_TEXT_MUTED,
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
      "Synchronize Selected Games");
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

void ui_render_game_panel(const UiAppState *state) {
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

  char summary[96];
  snprintf(
      summary,
      sizeof(summary),
      "Showing %d-%d of %d | Checked %d",
      start + 1,
      end,
      state->game_count,
      ui_selected_game_count(state));
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
        state->games[i].selected_for_sync,
        full_title,
        game->card_count);
    row_y += UI_GAME_ROW_HEIGHT;
  }
}

void ui_render_footer(const UiAppState *state) {
  if (state == NULL) {
    return;
  }

  UiMainLayout layout;
  ui_build_main_layout(&layout);

  char controls_hint[96];
  snprintf(
      controls_hint,
      sizeof(controls_hint),
      "D-Pad/Left Stick move   %s select/toggle   START quit",
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

void ui_render_main_screen(UiAppState *state) {
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

void ui_render_dialog_background_frame(void *user_data) {
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

void ui_render_busy_screen(const char *title, const char *subtitle) {
  ui_begin_frame();
  ui_draw_panel(168.0f, 194.0f, 624.0f, 146.0f, UI_COLOR_PANEL, UI_COLOR_PANEL_BORDER);
  ui_draw_text_center(UI_SCREEN_WIDTH * 0.5f, 244.0f, UI_COLOR_TEXT, 1.00f, has_text(title) ? title : "Please wait");
  if (has_text(subtitle)) {
    ui_draw_text_center(UI_SCREEN_WIDTH * 0.5f, 272.0f, UI_COLOR_TEXT_MUTED, 0.82f, subtitle);
  }
  ui_end_frame();
}

void ui_render_exit_screen(void) {
  ui_begin_frame();
  ui_draw_panel(220.0f, 214.0f, 520.0f, 112.0f, UI_COLOR_PANEL, UI_COLOR_PANEL_BORDER);
  ui_draw_text_center(UI_SCREEN_WIDTH * 0.5f, 274.0f, UI_COLOR_TEXT, 0.95f, "Exiting RomM Vita Sync...");
  ui_end_frame();
}
