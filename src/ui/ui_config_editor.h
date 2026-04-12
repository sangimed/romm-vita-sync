#ifndef UI_CONFIG_EDITOR_H
#define UI_CONFIG_EDITOR_H

#include "ui_common.h"

void ui_apply_logging_preferences(const AppConfig *config);
int ui_save_config(UiAppState *state, const char *success_message);
int ensure_device_registration(AppConfig *config, const RommClient *romm_client);
int ui_dialog_runtime_init(void);
void ui_dialog_runtime_term(void);
void ui_touch_init(void);
void ui_touch_term(void);
void ui_activate_selection(UiAppState *state);

#endif
