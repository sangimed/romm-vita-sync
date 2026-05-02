#ifndef UI_SCREENS_H
#define UI_SCREENS_H

#include "ui_common.h"

void ui_render_header(const UiAppState *state);
void ui_render_settings_panel(const UiAppState *state);
void ui_render_sync_panel(const UiAppState *state);
void ui_render_game_panel(const UiAppState *state);
void ui_render_footer(const UiAppState *state);
void ui_render_main_screen(UiAppState *state);
void ui_render_settings_screen(UiAppState *state);
void ui_render_active_screen(UiAppState *state);
void ui_render_dialog_background_frame(void *user_data);
void ui_render_busy_screen(const char *title, const char *subtitle);
void ui_render_exit_screen(void);

#endif
