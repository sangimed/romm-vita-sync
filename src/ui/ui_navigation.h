#ifndef UI_NAVIGATION_H
#define UI_NAVIGATION_H

#include "ui_common.h"

UiControllerState ui_poll_controller_state(void);
unsigned int ui_poll_buttons(void);
unsigned int ui_compute_pressed(unsigned int buttons, unsigned int *io_previous_buttons);
unsigned int ui_poll_pressed(unsigned int *io_previous_buttons);

int ui_total_selectable_entries(const UiAppState *state);
int ui_is_sync_button_index(int index);
int ui_get_selection_anchor(const UiAppState *state, int index, float *out_x, float *out_y);

void ui_handle_navigation_input(UiAppState *state, unsigned int buttons, unsigned char left_x, unsigned char left_y);
void ui_clamp_active_game(UiAppState *state);
void ui_clamp_selection(UiAppState *state);
void ui_update_game_scroll(UiAppState *state);
void ui_open_settings_screen(UiAppState *state);
void ui_close_settings_screen(UiAppState *state);
void ui_refresh_game_filter(UiAppState *state);
int ui_visible_game_count(const UiAppState *state);
int ui_game_index_for_visible_row(const UiAppState *state, int visible_index);
int ui_visible_row_for_game_index(const UiAppState *state, int game_index);
void ui_sync_active_game_from_selection(UiAppState *state);

const UiGameEntry *ui_active_game(const UiAppState *state);
const UiGameEntry *ui_visible_game(const UiAppState *state, int visible_index);
int ui_selected_game_count(const UiAppState *state);
const UiGameEntry *ui_first_selected_game(const UiAppState *state);
int ui_sync_action_enabled(const UiAppState *state);
int ui_sync_all_action_enabled(const UiAppState *state);

void ui_build_game_key(const SyncSaveDescriptor *item, char *out_key, size_t out_key_size);
int ui_find_game_entry(const UiGameEntry *games, int game_count, const char *key);
int ui_build_game_entries(const SyncSaveDescriptor *items, int item_count, UiGameEntry *out_games, int max_games);

#endif
