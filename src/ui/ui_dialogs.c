#include "ui_dialogs.h"

#include <psp2/common_dialog.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/message_dialog.h>

#include <string.h>
#include <vita2d.h>

#include "app_log.h"

static int g_dialog_initialized = 0;
static UiDialogFrameCallback g_frame_callback = NULL;
static void *g_frame_callback_user_data = NULL;

int ui_dialog_init(void) {
  g_dialog_initialized = 1;
  return 0;
}

void ui_dialog_set_frame_callback(UiDialogFrameCallback callback, void *user_data) {
  g_frame_callback = callback;
  g_frame_callback_user_data = user_data;
}

/*
 * Pumps one vita2d frame so sceMsgDialog renders on top of the app content.
 */
static void pump_frame(int delay_microseconds) {
  if (g_frame_callback != NULL) {
    g_frame_callback(g_frame_callback_user_data);
  } else {
    vita2d_start_drawing();
    vita2d_end_drawing();
    vita2d_common_dialog_update();
    vita2d_swap_buffers();
  }
  if (delay_microseconds > 0) {
    sceKernelDelayThread((unsigned int)delay_microseconds);
  }
}

/*
 * Spins until sceMsgDialog reaches FINISHED state, pumping frames each tick.
 */
static void wait_dialog_finished(void) {
  while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
    pump_frame(16 * 1000);
  }
}

/*
 * Shows a user-message dialog and returns the pressed button.
 */
static int show_user_msg(const char *message, SceMsgDialogButtonType buttons) {
  if (message == NULL) {
    return -1;
  }
  if (!g_dialog_initialized) {
    app_log_write(APP_LOG_LEVEL_ERROR, "dialog", "ui_dialog_init not called");
    return -1;
  }

  SceMsgDialogParam param;
  sceMsgDialogParamInit(&param);
  param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;

  SceMsgDialogUserMessageParam msg;
  memset(&msg, 0, sizeof(msg));
  msg.buttonType = buttons;
  msg.msg = (const SceChar8 *)message;
  param.userMsgParam = &msg;

  int status = sceMsgDialogInit(&param);
  if (status < 0) {
    app_log_write(APP_LOG_LEVEL_ERROR, "dialog", "sceMsgDialogInit failed: 0x%08X", (unsigned int)status);
    return -1;
  }

  SceMsgDialogResult result;
  memset(&result, 0, sizeof(result));
  result.buttonId = SCE_MSG_DIALOG_BUTTON_ID_INVALID;

  int close_requested = 0;
  for (;;) {
    int dialog_status = sceMsgDialogGetStatus();
    if (dialog_status == SCE_COMMON_DIALOG_STATUS_FINISHED) {
      break;
    }

    if (!close_requested && (dialog_status == SCE_COMMON_DIALOG_STATUS_RUNNING)) {
      SceMsgDialogResult live_result;
      memset(&live_result, 0, sizeof(live_result));
      sceMsgDialogGetResult(&live_result);
      if (live_result.buttonId != SCE_MSG_DIALOG_BUTTON_ID_INVALID) {
        result = live_result;
        sceMsgDialogClose();
        close_requested = 1;
      }
    }
    pump_frame(close_requested ? 0 : (16 * 1000));
  }

  if (result.buttonId == SCE_MSG_DIALOG_BUTTON_ID_INVALID) {
    sceMsgDialogGetResult(&result);
  }
  sceMsgDialogTerm();

  return (int)result.buttonId;
}

int ui_dialog_confirm(const char *message) {
  int button = show_user_msg(message, SCE_MSG_DIALOG_BUTTON_TYPE_YESNO);
  if (button < 0) {
    return button;
  }
  return (button == SCE_MSG_DIALOG_BUTTON_ID_YES) ? 1 : 0;
}

int ui_dialog_info(const char *message) {
  int button = show_user_msg(message, SCE_MSG_DIALOG_BUTTON_TYPE_OK);
  return (button < 0) ? button : 0;
}

int ui_dialog_error(const char *message) {
  return ui_dialog_info(message);
}

int ui_dialog_progress_start(const char *message) {
  if (!g_dialog_initialized) {
    app_log_write(APP_LOG_LEVEL_ERROR, "dialog", "ui_dialog_init not called");
    return -1;
  }

  SceMsgDialogParam param;
  sceMsgDialogParamInit(&param);
  param.mode = SCE_MSG_DIALOG_MODE_PROGRESS_BAR;

  SceMsgDialogProgressBarParam bar;
  memset(&bar, 0, sizeof(bar));
  bar.barType = SCE_MSG_DIALOG_PROGRESSBAR_TYPE_PERCENTAGE;
  bar.msg = (const SceChar8 *)message;
  param.progBarParam = &bar;

  int status = sceMsgDialogInit(&param);
  if (status < 0) {
    app_log_write(APP_LOG_LEVEL_ERROR, "dialog", "sceMsgDialogInit (progress) failed: 0x%08X", (unsigned int)status);
    return -1;
  }

  pump_frame(16 * 1000);
  return 0;
}

int ui_dialog_progress_update(int percent) {
  if (percent < 0) {
    percent = 0;
  }
  if (percent > 100) {
    percent = 100;
  }

  int status = sceMsgDialogProgressBarSetValue(SCE_MSG_DIALOG_PROGRESSBAR_TARGET_BAR_DEFAULT, (SceUInt32)percent);
  if (status < 0) {
    return status;
  }

  pump_frame(16 * 1000);
  return 0;
}

int ui_dialog_progress_finish(void) {
  sceMsgDialogProgressBarSetValue(SCE_MSG_DIALOG_PROGRESSBAR_TARGET_BAR_DEFAULT, 100);
  pump_frame(16 * 1000);
  sceMsgDialogClose();
  wait_dialog_finished();

  SceMsgDialogResult result;
  memset(&result, 0, sizeof(result));
  sceMsgDialogGetResult(&result);
  sceMsgDialogTerm();
  return 0;
}
