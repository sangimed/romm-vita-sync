#ifndef UI_SYNC_MODAL_H
#define UI_SYNC_MODAL_H

#include "ui_common.h"

void ui_build_sync_modal_layout(const UiSyncFeedback *feedback, UiSyncModalLayout *layout);
int ui_sync_modal_max_scroll(const UiSyncModalLayout *layout);
void ui_sync_modal_scroll_by(UiSyncFeedback *feedback, const UiSyncModalLayout *layout, int delta_lines);
void ui_sync_modal_reset_touch(UiSyncFeedback *feedback);
void ui_sync_modal_handle_input(UiAppState *state);
void ui_render_sync_modal(UiAppState *state);
void ui_sync_render_live(UiAppState *state);

#endif
