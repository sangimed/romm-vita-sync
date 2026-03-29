#include "app_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "debugScreen.h"

#define printf psvDebugScreenPrintf

static AppLogLevel g_log_level = APP_LOG_LEVEL_INFO;
static char g_log_history[APP_LOG_HISTORY_CAPACITY][320];
static int g_log_history_start = 0;
static int g_log_history_count = 0;

/*
 * Appends one rendered log line to the in-memory ring buffer.
 */
static void append_history_line(const char *line) {
  if ((line == NULL) || (line[0] == '\0')) {
    return;
  }

  int write_index = (g_log_history_start + g_log_history_count) % APP_LOG_HISTORY_CAPACITY;
  snprintf(g_log_history[write_index], sizeof(g_log_history[write_index]), "%s", line);

  if (g_log_history_count < APP_LOG_HISTORY_CAPACITY) {
    g_log_history_count += 1;
    return;
  }

  g_log_history_start = (g_log_history_start + 1) % APP_LOG_HISTORY_CAPACITY;
}

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
 * Clears all in-memory log history lines.
 */
void app_log_clear_history(void) {
  memset(g_log_history, 0, sizeof(g_log_history));
  g_log_history_start = 0;
  g_log_history_count = 0;
}

/*
 * Returns the number of lines currently kept in memory.
 */
int app_log_history_count(void) {
  return g_log_history_count;
}

/*
 * Returns one log line from history by chronological index.
 */
const char *app_log_history_line(int index) {
  if ((index < 0) || (index >= g_log_history_count)) {
    return NULL;
  }

  int slot = (g_log_history_start + index) % APP_LOG_HISTORY_CAPACITY;
  return g_log_history[slot];
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

  char rendered[360];
  if ((tag != NULL) && (tag[0] != '\0')) {
    snprintf(rendered, sizeof(rendered), "[%s][%s] %s", app_log_level_str(level), tag, message);
  } else {
    snprintf(rendered, sizeof(rendered), "[%s] %s", app_log_level_str(level), message);
  }

  append_history_line(rendered);
  printf("%s\n", rendered);
}
