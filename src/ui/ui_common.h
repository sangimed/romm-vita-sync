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

#define UI_SELECT_GAME_SEARCH 0
#define UI_SELECT_SYNC_PRIMARY 1
#define UI_SELECT_SYNC_ALL 2
#define UI_SELECT_RESCAN 3
#define UI_SELECT_OPEN_SETTINGS 4
#define UI_SELECT_GAME_BASE 5

#define UI_SELECT_SERVER_URL 0
#define UI_SELECT_API_TOKEN 1
#define UI_SELECT_USERNAME 2
#define UI_SELECT_PASSWORD 3
#define UI_SELECT_PLATFORM 4
#define UI_SELECT_DRY_RUN 5
#define UI_SELECT_SETTINGS_BACK 6

#define UI_SCREEN_WIDTH 960.0f
#define UI_SCREEN_HEIGHT 544.0f

#define UI_GAME_LIST_VISIBLE 3
#define UI_GAME_ROW_HEIGHT 42.0f
#define UI_GAME_SEARCH_QUERY_LEN 96
#define UI_LOG_TOP_PADDING 18.0f
#define UI_LOG_BOTTOM_PADDING 10.0f
#define UI_LOG_LINE_HEIGHT 23.0f
#define UI_LOG_TEXT_SCALE 0.82f
#define UI_WRAP_BUFFER_LEN 384
#define UI_WRAP_MAX_LINES_PER_BLOCK 48
#define UI_STATUS_LINE_LEN 192
#define UI_EDITOR_BUFFER_LEN 512
#define UI_SCROLL_REPEAT_DELAY_FRAMES 18
#define UI_SCROLL_REPEAT_INTERVAL_FRAMES 3
#define UI_TOUCH_DRAG_DEADZONE 4.0f
#define APP_RUNTIME_DATA_DIRECTORY "ux0:data/romm-vita-sync"
#define APP_RUNTIME_LOG_FILE_PATH "ux0:data/romm-vita-sync/romm-vita-sync.log"

#define UI_COLOR_BACKGROUND RGBA8(5, 11, 18, 255)
#define UI_COLOR_BACKGROUND_ALT RGBA8(7, 18, 30, 255)
#define UI_COLOR_HEADER RGBA8(7, 16, 27, 255)
#define UI_COLOR_FOOTER RGBA8(6, 13, 22, 255)
#define UI_COLOR_PANEL RGBA8(9, 22, 36, 250)
#define UI_COLOR_PANEL_ALT RGBA8(7, 17, 28, 250)
#define UI_COLOR_PANEL_SHADOW RGBA8(1, 4, 8, 82)
#define UI_COLOR_PANEL_HIGHLIGHT RGBA8(255, 255, 255, 16)
#define UI_COLOR_PANEL_BORDER RGBA8(70, 96, 125, 132)
#define UI_COLOR_PANEL_BORDER_ACTIVE RGBA8(35, 230, 220, 236)
#define UI_COLOR_FIELD RGBA8(12, 27, 43, 255)
#define UI_COLOR_FIELD_ACTIVE RGBA8(18, 42, 63, 255)
#define UI_COLOR_BUTTON RGBA8(12, 172, 168, 255)
#define UI_COLOR_BUTTON_ACTIVE RGBA8(31, 214, 205, 255)
#define UI_COLOR_BUTTON_DISABLED RGBA8(43, 55, 67, 255)
#define UI_COLOR_BUTTON_BORDER RGBA8(82, 245, 230, 126)
#define UI_COLOR_TEXT RGBA8(248, 250, 253, 255)
#define UI_COLOR_TEXT_MUTED RGBA8(189, 200, 216, 255)
#define UI_COLOR_TEXT_DIM RGBA8(122, 139, 158, 255)
#define UI_COLOR_STATUS RGBA8(225, 234, 244, 255)
#define UI_COLOR_ACCENT RGBA8(45, 231, 222, 255)
#define UI_COLOR_ACCENT_SOFT RGBA8(45, 231, 222, 50)
#define UI_COLOR_GOLD RGBA8(255, 203, 89, 255)
#define UI_COLOR_GOLD_SOFT RGBA8(255, 203, 89, 58)
#define UI_COLOR_MAGENTA RGBA8(139, 167, 255, 255)
#define UI_COLOR_MAGENTA_SOFT RGBA8(139, 167, 255, 34)
#define UI_COLOR_SUCCESS RGBA8(88, 230, 139, 255)
#define UI_COLOR_WARNING RGBA8(255, 192, 98, 255)
#define UI_COLOR_DANGER RGBA8(255, 117, 127, 255)

#define UI_TEXT_SCALE_BOOST 1.15f
#define UI_TEXT_SCALE_MIN 0.875f
#define UI_TEXT_SCALE_MAX 1.50f
#define UI_TEXT_SCALE_STEP 0.125f
#define UI_TEXT_SHADOW_OFFSET_X 1.0f
#define UI_TEXT_SHADOW_OFFSET_Y 1.0f
#define UI_TEXT_SHADOW_ALPHA_MIN 18U
#define UI_TEXT_SHADOW_ALPHA_MAX 54U
#define UI_TEXT_SHADOW_MIN_SCALE 1.125f

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

typedef enum UiActiveScreen {
  UI_ACTIVE_SCREEN_MAIN = 0,
  UI_ACTIVE_SCREEN_SETTINGS = 1
} UiActiveScreen;

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

  float settings_x;
  float settings_y;
  float settings_w;
  float settings_h;
  float settings_button_x;
  float settings_button_y;
  float settings_button_w;
  float settings_button_h;

  float settings_options_x;
  float settings_options_y;
  float settings_options_w;
  float settings_options_h;
  float settings_options_row_x;
  float settings_options_row_w;
  float settings_options_row_h;
  float settings_options_row_gap;
  float settings_options_first_row_y;
  float settings_back_button_x;
  float settings_back_button_y;
  float settings_back_button_w;
  float settings_back_button_h;

  float search_row_x;
  float search_row_y;
  float search_row_w;
  float search_row_h;

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
  int filtered_game_indices[ROMM_SYNC_MAX_ITEMS];
  int filtered_game_count;
  char game_search_query[UI_GAME_SEARCH_QUERY_LEN];

  UiActiveScreen active_screen;
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
