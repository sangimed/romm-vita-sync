#include "ui_navigation.h"

#include <psp2/ctrl.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "ui_common.h"

UiControllerState ui_poll_controller_state(void) {
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

unsigned int ui_poll_buttons(void) {
  return ui_poll_controller_state().buttons;
}

unsigned int ui_compute_pressed(unsigned int buttons, unsigned int *io_previous_buttons) {
  if (io_previous_buttons == NULL) {
    return 0U;
  }

  unsigned int pressed = buttons & (~(*io_previous_buttons));
  *io_previous_buttons = buttons;
  return pressed;
}

unsigned int ui_poll_pressed(unsigned int *io_previous_buttons) {
  return ui_compute_pressed(ui_poll_buttons(), io_previous_buttons);
}

int ui_total_selectable_entries(const UiAppState *state) {
  if (state == NULL) {
    return UI_SELECT_GAME_BASE;
  }
  return UI_SELECT_GAME_BASE + state->game_count;
}

int ui_is_sync_button_index(int index) {
  return (index >= UI_SELECT_SYNC_PRIMARY) && (index <= UI_SELECT_RESCAN);
}

static int ui_try_move_selection_shortcut(UiAppState *state, int direction) {
  if (state == NULL) {
    return 0;
  }

  if ((direction == UI_NAV_RIGHT) &&
      ((state->selected_index == UI_SELECT_SERVER_URL) ||
       (state->selected_index == UI_SELECT_USERNAME) ||
       (state->selected_index == UI_SELECT_PASSWORD) ||
       (state->selected_index == UI_SELECT_PLATFORM) ||
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

int ui_get_selection_anchor(const UiAppState *state, int index, float *out_x, float *out_y) {
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
  if (index == UI_SELECT_PLATFORM) {
    *out_x = layout.connection_row_x + (layout.connection_row_w * 0.5f);
    *out_y = layout.connection_first_row_y + ((layout.connection_row_h + layout.connection_row_gap) * 3.0f) +
             (layout.connection_row_h * 0.5f);
    return 0;
  }
  if (index == UI_SELECT_DRY_RUN) {
    *out_x = layout.connection_row_x + (layout.connection_row_w * 0.5f);
    *out_y = layout.connection_first_row_y + ((layout.connection_row_h + layout.connection_row_gap) * 4.0f) +
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

void ui_handle_navigation_input(UiAppState *state, unsigned int buttons, unsigned char left_x, unsigned char left_y) {
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

void ui_clamp_active_game(UiAppState *state) {
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

void ui_clamp_selection(UiAppState *state) {
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

void ui_update_game_scroll(UiAppState *state) {
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

const UiGameEntry *ui_active_game(const UiAppState *state) {
  if ((state == NULL) || (state->game_count <= 0)) {
    return NULL;
  }

  if ((state->active_game_index < 0) || (state->active_game_index >= state->game_count)) {
    return NULL;
  }

  return &state->games[state->active_game_index];
}

int ui_selected_game_count(const UiAppState *state) {
  if ((state == NULL) || (state->game_count <= 0)) {
    return 0;
  }

  int count = 0;
  for (int i = 0; i < state->game_count; ++i) {
    if (state->games[i].selected_for_sync) {
      count += 1;
    }
  }
  return count;
}

const UiGameEntry *ui_first_selected_game(const UiAppState *state) {
  if ((state == NULL) || (state->game_count <= 0)) {
    return NULL;
  }

  for (int i = 0; i < state->game_count; ++i) {
    if (state->games[i].selected_for_sync) {
      return &state->games[i];
    }
  }

  return NULL;
}

int ui_sync_action_enabled(const UiAppState *state) {
  if (ui_selected_game_count(state) <= 0) {
    return 0;
  }

  if ((state == NULL) || !app_config_has_server_url(&state->config) || !app_config_has_auth(&state->config)) {
    return 0;
  }

  return 1;
}

int ui_sync_all_action_enabled(const UiAppState *state) {
  if ((state == NULL) || (state->local_count <= 0)) {
    return 0;
  }

  if (!app_config_has_server_url(&state->config) || !app_config_has_auth(&state->config)) {
    return 0;
  }

  return 1;
}

void ui_build_game_key(const SyncSaveDescriptor *item, char *out_key, size_t out_key_size) {
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

int ui_find_game_entry(const UiGameEntry *games, int game_count, const char *key) {
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

static int ui_item_has_local_memory_card(const SyncSaveDescriptor *item) {
  if (item == NULL) {
    return 0;
  }

  return item->size_bytes > 0U;
}

int ui_build_game_entries(
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
