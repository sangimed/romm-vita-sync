#ifndef APP_LOG_H
#define APP_LOG_H

typedef enum AppLogLevel {
  APP_LOG_LEVEL_ERROR = 0,
  APP_LOG_LEVEL_WARN = 1,
  APP_LOG_LEVEL_INFO = 2,
  APP_LOG_LEVEL_DEBUG = 3
} AppLogLevel;

/*
 * Sets the global log level threshold.
 */
void app_log_set_level(AppLogLevel level);

/*
 * Returns the currently active log level threshold.
 */
AppLogLevel app_log_get_level(void);

/*
 * Returns non-zero when a message at level should be emitted.
 */
int app_log_is_enabled(AppLogLevel level);

/*
 * Writes one structured log line to the debug screen when enabled.
 */
void app_log_write(AppLogLevel level, const char *tag, const char *format, ...);

/*
 * Returns short textual name for a log level.
 */
const char *app_log_level_str(AppLogLevel level);

#endif
