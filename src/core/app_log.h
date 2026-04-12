#ifndef APP_LOG_H
#define APP_LOG_H

typedef enum AppLogLevel {
  APP_LOG_LEVEL_ERROR = 0,
  APP_LOG_LEVEL_WARN = 1,
  APP_LOG_LEVEL_INFO = 2,
  APP_LOG_LEVEL_DEBUG = 3
} AppLogLevel;

/*
 * Maximum number of log lines kept in memory for UI rendering.
 */
#define APP_LOG_HISTORY_CAPACITY 256

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
 * Writes one structured log line to in-memory history when enabled.
 */
void app_log_write(AppLogLevel level, const char *tag, const char *format, ...);

/*
 * Clears all in-memory log history lines.
 */
void app_log_clear_history(void);

/*
 * Returns the number of lines currently kept in memory.
 */
int app_log_history_count(void);

/*
 * Returns one log line from history by chronological index.
 * Index 0 is the oldest retained line.
 */
const char *app_log_history_line(int index);

/*
 * Returns short textual name for a log level.
 */
const char *app_log_level_str(AppLogLevel level);

/*
 * Enables/disables file logging and sets the destination path.
 * When disabled, in-memory history logging is unchanged.
 */
void app_log_set_file_output(int enabled, const char *log_path);

#endif
