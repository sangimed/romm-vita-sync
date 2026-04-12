#ifndef UI_SYNC_ORCHESTRATOR_H
#define UI_SYNC_ORCHESTRATOR_H

#include "ui_common.h"

int ui_refresh_local_inventory(UiAppState *state);
int ui_run_sync_pipeline(UiAppState *state, SyncSaveDescriptor *work_items, int work_item_count, UiSyncTrigger trigger, const char *title, const char *context);
void ui_run_sync_for_selected_games(UiAppState *state);
void ui_run_sync_all_saves(UiAppState *state);
void ui_run_pending_auto_sync(UiAppState *state);

#endif
