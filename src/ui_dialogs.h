#ifndef UI_DIALOGS_H
#define UI_DIALOGS_H

#define UI_DIALOG_MSG_MAX_LEN 512

/*
 * Initializes the sceMsgDialog subsystem.
 * Must be called after sceCommonDialogSetConfigParam.
 * Returns 0 on success, negative on failure.
 */
int ui_dialog_init(void);

/*
 * Displays a Yes/No confirmation dialog using sceMsgDialog.
 * Blocks until user responds. Pumps vita2d frames while waiting.
 * Returns 1 when user selects Yes, 0 when No or canceled, negative on error.
 */
int ui_dialog_confirm(const char *message);

/*
 * Displays an informational OK dialog using sceMsgDialog.
 * Blocks until user dismisses. Pumps vita2d frames while waiting.
 * Returns 0 on success, negative on error.
 */
int ui_dialog_info(const char *message);

/*
 * Displays an error-style OK dialog using sceMsgDialog.
 * Blocks until user dismisses. Pumps vita2d frames while waiting.
 * Returns 0 on success, negative on error.
 */
int ui_dialog_error(const char *message);

/*
 * Opens a sceMsgDialog progress bar.
 * Blocks rendering until ui_dialog_progress_update/finish are called.
 * Returns 0 on success, negative on error.
 */
int ui_dialog_progress_start(const char *message);

/*
 * Updates the progress bar percentage (0-100).
 * Must be called between progress_start and progress_finish.
 * Pumps one vita2d frame per call.
 * Returns 0 on success, negative on error.
 */
int ui_dialog_progress_update(int percent);

/*
 * Closes the running progress bar dialog.
 * Returns 0 on success, negative on error.
 */
int ui_dialog_progress_finish(void);

#endif
