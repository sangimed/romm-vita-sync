#ifndef UI_COMMON_H
#define UI_COMMON_H

#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <vita2d.h>

#include "app_config.h"
#include "app_log.h"
#include "romm_client.h"
#include "save_scanner.h"
#include "sync_types.h"

#define UI_SELECT_SERVER_URL 0
#define UI_SELECT_USERNAME 1
#define UI_SELECT_PASSWORD 2
#define UI_SELECT_PLATFORM 3
#define UI_SELECT_DRY_RUN 4
#define UI_SELECT_SYNC_PRIMARY 5
#define UI_SELECT_SYNC_ALL 6
#define UI_SELECT_RESCAN 7
#define UI_SELECT_GAME_BASE 8

#define UI_SCREEN_WIDTH 960.0f
#define UI_SCREEN_HEIGHT 544.0f

#define UI_GAME_LIST_VISIBLE 4
#define UI_GAME_ROW_HEIGHT 28.0f
#define UI_LOG_TOP_PADDING 18.0f
#define UI_LOG_BOTTOM_PADDING 10.0f
#define UI_LOG_LINE_HEIGHT 18.0f
#define UI_LOG_TEXT_SCALE 0.66f
#define UI_WRAP_BUFFER_LEN 384
#define UI_WRAP_MAX_LINES_PER_BLOCK 48
#define UI_STATUS_LINE_LEN 192
#define UI_EDITOR_BUFFER_LEN 512
#define UI_SCROLL_REPEAT_DELAY_FRAMES 18
#define UI_SCROLL_REPEAT_INTERVAL_FRAMES 3
#define UI_TOUCH_DRAG_DEADZONE 4.0f
#define APP_RUNTIME_DATA_DIRECTORY "ux0:data/romm-vita-sync"
#define APP_RUNTIME_LOG_FILE_PATH "ux0:data/romm-vita-sync/romm-vita-sync.log"

#define UI_COLOR_BACKGROUND RGBA8(11, 15, 22, 255)
#define UI_COLOR_BACKGROUND_ALT RGBA8(16, 22, 32, 255)
#define UI_COLOR_HEADER RGBA8(18, 24, 34, 255)
#define UI_COLOR_FOOTER RGBA8(14, 19, 27, 255)
#define UI_COLOR_PANEL RGBA8(24, 32, 45, 236)
#define UI_COLOR_PANEL_ALT RGBA8(20, 28, 39, 236)
#define UI_COLOR_PANEL_BORDER RGBA8(84, 100, 122, 148)
#define UI_COLOR_PANEL_BORDER_ACTIVE RGBA8(94, 155, 255, 220)
#define UI_COLOR_FIELD RGBA8(28, 37, 51, 255)
#define UI_COLOR_FIELD_ACTIVE RGBA8(37, 52, 74, 255)
#define UI_COLOR_BUTTON RGBA8(60, 122, 218, 255)
#define UI_COLOR_BUTTON_ACTIVE RGBA8(84, 146, 244, 255)
#define UI_COLOR_BUTTON_DISABLED RGBA8(52, 60, 72, 255)
#define UI_COLOR_BUTTON_BORDER RGBA8(182, 214, 255, 96)
#define UI_COLOR_TEXT RGBA8(248, 250, 252, 255)
#define UI_COLOR_TEXT_MUTED RGBA8(191, 200, 214, 255)
#define UI_COLOR_TEXT_DIM RGBA8(130, 142, 160, 255)
#define UI_COLOR_STATUS RGBA8(224, 231, 241, 255)
#define UI_COLOR_ACCENT RGBA8(94, 155, 255, 255)
#define UI_COLOR_ACCENT_SOFT RGBA8(94, 155, 255, 56)
#define UI_COLOR_SUCCESS RGBA8(138, 214, 167, 255)
#define UI_COLOR_WARNING RGBA8(255, 194, 119, 255)
#define UI_COLOR_DANGER RGBA8(255, 140, 140, 255)

#define UI_TEXT_SCALE_BOOST 1.14f
#define UI_TEXT_SCALE_MIN 0.82f
#define UI_TEXT_SCALE_MAX 1.46f
#define UI_TEXT_SCALE_STEP 0.125f
#define UI_TEXT_SHADOW_OFFSET_X 1.0f
#define UI_TEXT_SHADOW_OFFSET_Y 1.0f
#define UI_TEXT_SHADOW_ALPHA_MIN 32U
#define UI_TEXT_SHADOW_ALPHA_MAX 96U

#define UI_NAV_UP 0
#define UI_NAV_DOWN 1
#define UI_NAV_LEFT 2
#define UI_NAV_RIGHT 3
#define UI_NAV_NONE -1

#define UI_ANALOG_CENTER 127
#define UI_ANALOG_DEADZONE 40
#define UI_NAV_REPEAT_DELAY_FRAMES 14
#define UI_NAV_REPEAT_INTERVAL_FRAMES 4

typedef struct UiControllerState {
  unsigned int buttons;
  unsigned char left_x;
  unsigned char left_y;
} UiControllerState;

typedef struct UiGameEntry {
  char key[ROMM_GAME_ID_LEN];
  char game_id[ROMM_GAME_ID_LEN];
  char title[ROMM_GAME_TITLE_LEN];
  int save_count;
  int card_count;
  int selected_for_sync;
} UiGameEntry;

typedef enum UiSyncTrigger {
  UI_SYNC_TRIGGER_MANUAL = 0,
  UI_SYNC_TRIGGER_AUTOMATIC = 1
} UiSyncTrigger;

typedef struct UiSyncFeedback {
  int running;
  int completed;
  int success;
  int sync_status;
  int completed_units;
  int total_units;
  int modal_log_scroll;
  int modal_auto_scroll;
  int modal_scroll_hold_direction;
  int modal_scroll_hold_frames;
  int modal_touch_active;
  int modal_touch_id;
  float modal_touch_last_y;
  float modal_touch_scroll_remainder;
  UiSyncTrigger trigger;
  char title[64];
  char message[UI_STATUS_LINE_LEN];
  char context[UI_STATUS_LINE_LEN];
} UiSyncFeedback;

typedef struct UiMainLayout {
  float connection_x;
  float connection_y;
  float connection_w;
  float connection_h;
  float connection_row_x;
  float connection_row_w;
  float connection_row_h;
  float connection_row_gap;
  float connection_first_row_y;

  float sync_x;
  float sync_y;
  float sync_w;
  float sync_h;
  float sync_content_x;
  float sync_content_w;
  float sync_button_x;
  float sync_button_w;
  float sync_button_h;
  float sync_button_gap;
  float sync_first_button_y;

  float game_x;
  float game_y;
  float game_w;
  float game_h;
  float game_row_x;
  float game_row_w;
  float game_first_row_y;

  float footer_status_x;
  float footer_status_w;
  float footer_hint_right_x;
  float footer_hint_w;
} UiMainLayout;

typedef struct UiSyncModalLayout {
  float panel_x;
  float panel_y;
  float panel_w;
  float panel_h;
  float content_x;
  float content_w;
  float title_y;
  float context_y;
  float progress_y;
  float progress_text_y;
  float log_label_y;
  float log_x;
  float log_y;
  float log_w;
  float log_h;
  float scroll_hint_y;
  float message_y;
  float footer_y;
} UiSyncModalLayout;

typedef struct UiAppState {
  AppConfig config;
  int config_status;
  RommClient romm_client;

  ScanResult scan_result;
  SyncSaveDescriptor local_items[ROMM_SYNC_MAX_ITEMS];
  SyncSaveDescriptor sync_work_items[ROMM_SYNC_MAX_ITEMS];
  SyncRunReport sync_report;
  int local_count;
  SyncSavePlatform selected_save_platform;

  UiGameEntry games[ROMM_SYNC_MAX_ITEMS];
  int game_count;

  int selected_index;
  int active_game_index;
  int game_scroll;
  int nav_hold_direction;
  int nav_hold_frames;
  char status_line[UI_STATUS_LINE_LEN];
  UiSyncFeedback sync_feedback;
  int pending_auto_sync;
} UiAppState;

typedef struct UiSyncProgressBridge {
  UiAppState *state;
  int base_completed_units;
  int overall_total_units;
} UiSyncProgressBridge;

typedef struct UiSyncConflictResolutionContext {
  UiAppState *state;
  UiSyncTrigger trigger;
  int dry_run;
  int auto_apply_conflicts;
} UiSyncConflictResolutionContext;

int has_text(const char *value);
int clamp_int(int value, int min_value, int max_value);

void ui_set_status(UiAppState *state, const char *format, ...);
void ui_pump_app_events(void);
void ui_build_main_layout(UiMainLayout *layout);

const char *ui_dialog_confirm_button_label(void);
const char *ui_dialog_decline_button_label(void);
unsigned int ui_primary_action_button(void);

void ui_sync_feedback_reset(UiSyncFeedback *feedback, UiSyncTrigger trigger, const char *title, const char *context);
void ui_sync_feedback_set_message(UiSyncFeedback *feedback, const char *message);
void ui_sync_feedback_set_progress(UiSyncFeedback *feedback, int completed_units, int total_units);
void ui_sync_log_write(AppLogLevel level, const char *format, ...);

#endif
