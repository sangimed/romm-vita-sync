#include "app_log.h"

#include <stdarg.h>
#include <stdio.h>

#include "debugScreen.h"

#define printf psvDebugScreenPrintf

static AppLogLevel g_log_level = APP_LOG_LEVEL_INFO;

/*
 * Returns short textual name for a log level.
 */
const char *app_log_level_str(AppLogLevel level) {
  switch (level) {
    case APP_LOG_LEVEL_ERROR:
      return "ERROR";
    case APP_LOG_LEVEL_WARN:
      return "WARN";
    case APP_LOG_LEVEL_DEBUG:
      return "DEBUG";
    case APP_LOG_LEVEL_INFO:
    default:
      return "INFO";
  }
}

/*
 * Sets the global log level threshold.
 */
void app_log_set_level(AppLogLevel level) {
  if (level < APP_LOG_LEVEL_ERROR) {
    level = APP_LOG_LEVEL_ERROR;
  }
  if (level > APP_LOG_LEVEL_DEBUG) {
    level = APP_LOG_LEVEL_DEBUG;
  }
  g_log_level = level;
}

/*
 * Returns the currently active log level threshold.
 */
AppLogLevel app_log_get_level(void) {
  return g_log_level;
}

/*
 * Returns non-zero when a message at level should be emitted.
 */
int app_log_is_enabled(AppLogLevel level) {
  if (level < APP_LOG_LEVEL_ERROR) {
    return 0;
  }
  return level <= g_log_level;
}

/*
 * Writes one structured log line to the debug screen when enabled.
 */
void app_log_write(AppLogLevel level, const char *tag, const char *format, ...) {
  if (!app_log_is_enabled(level) || (format == NULL)) {
    return;
  }

  char message[320];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);

  if ((tag != NULL) && (tag[0] != '\0')) {
    printf("[%s][%s] %s\n", app_log_level_str(level), tag, message);
  } else {
    printf("[%s] %s\n", app_log_level_str(level), message);
  }
}
