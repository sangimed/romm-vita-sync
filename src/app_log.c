#include "app_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static AppLogLevel g_log_level = APP_LOG_LEVEL_INFO;
static char g_log_history[APP_LOG_HISTORY_CAPACITY][320];
static int g_log_history_start = 0;
static int g_log_history_count = 0;
static int g_file_output_enabled = 0;
static char g_file_output_path[256];

#define APP_LOG_FILE_MAX_SIZE_BYTES (10U * 1024U * 1024U)
#define APP_LOG_FILE_MAX_FILES 3

/*
 * Returns non-zero when text is non-null and non-empty.
 */
static int has_text(const char *text) {
  return (text != NULL) && (text[0] != '\0');
}

/*
 * Safely copies one optional string into a fixed-size destination.
 */
static void safe_copy(char *destination, size_t destination_size, const char *source) {
  if ((destination == NULL) || (destination_size == 0U)) {
    return;
  }

  if (source == NULL) {
    destination[0] = '\0';
    return;
  }

  snprintf(destination, destination_size, "%s", source);
}

/*
 * Builds one dated timestamp suitable for log file output.
 */
static void build_timestamp(char *out, size_t out_size) {
  if ((out == NULL) || (out_size == 0U)) {
    return;
  }

  time_t now = time(NULL);
  if (now <= 0) {
    snprintf(out, out_size, "0000-00-00 00:00:00");
    return;
  }

  struct tm *local = localtime(&now);
  if (local == NULL) {
    snprintf(out, out_size, "0000-00-00 00:00:00");
    return;
  }

  snprintf(
      out,
      out_size,
      "%04d-%02d-%02d %02d:%02d:%02d",
      local->tm_year + 1900,
      local->tm_mon + 1,
      local->tm_mday,
      local->tm_hour,
      local->tm_min,
      local->tm_sec);
}

/*
 * Returns one file size in bytes, or -1 when file is not readable.
 */
static long file_size_bytes(const char *path) {
  if (!has_text(path)) {
    return -1;
  }

  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    return -1;
  }

  if (fseek(file, 0L, SEEK_END) != 0) {
    fclose(file);
    return -1;
  }

  long size = ftell(file);
  fclose(file);
  return size;
}

/*
 * Resolves one rotated log path from base path and index.
 * Index 0 means the active log file.
 */
static void build_rotated_path(const char *base_path, int index, char *out_path, size_t out_size) {
  if ((out_path == NULL) || (out_size == 0U)) {
    return;
  }

  if (!has_text(base_path)) {
    out_path[0] = '\0';
    return;
  }

  if (index <= 0) {
    safe_copy(out_path, out_size, base_path);
    return;
  }

  snprintf(out_path, out_size, "%s.%d", base_path, index);
}

/*
 * Rotates log files when active file reached size limit.
 * Keeps at most APP_LOG_FILE_MAX_FILES files in total.
 */
static void rotate_log_files_if_needed(size_t incoming_line_bytes) {
  if (!g_file_output_enabled || !has_text(g_file_output_path)) {
    return;
  }

  long size = file_size_bytes(g_file_output_path);
  if (size < 0) {
    return;
  }

  if ((unsigned long long)size + (unsigned long long)incoming_line_bytes <= APP_LOG_FILE_MAX_SIZE_BYTES) {
    return;
  }

  char source[320];
  char target[320];

  for (int index = APP_LOG_FILE_MAX_FILES; index < 32; ++index) {
    build_rotated_path(g_file_output_path, index, target, sizeof(target));
    remove(target);
  }

  build_rotated_path(g_file_output_path, APP_LOG_FILE_MAX_FILES - 1, target, sizeof(target));
  remove(target);

  for (int index = APP_LOG_FILE_MAX_FILES - 2; index >= 0; --index) {
    build_rotated_path(g_file_output_path, index, source, sizeof(source));
    build_rotated_path(g_file_output_path, index + 1, target, sizeof(target));
    remove(target);
    rename(source, target);
  }
}

/*
 * Appends one line to the file log when enabled.
 */
static void append_file_line(const char *line) {
  if (!g_file_output_enabled || !has_text(g_file_output_path) || !has_text(line)) {
    return;
  }

  size_t line_len = strlen(line);
  rotate_log_files_if_needed(line_len + 1U);

  FILE *file = fopen(g_file_output_path, "ab");
  if (file == NULL) {
    return;
  }

  fwrite(line, 1U, line_len, file);
  fputc('\n', file);
  fflush(file);
  fclose(file);
}

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
 * Enables/disables file logging and sets the destination path.
 */
void app_log_set_file_output(int enabled, const char *log_path) {
  g_file_output_enabled = enabled ? 1 : 0;
  safe_copy(g_file_output_path, sizeof(g_file_output_path), log_path);
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
 * Writes one structured log line to the in-memory history when enabled.
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

  char timestamp[32];
  build_timestamp(timestamp, sizeof(timestamp));

  char file_line[420];
  snprintf(file_line, sizeof(file_line), "[%s] %s", timestamp, rendered);
  append_file_line(file_line);
}
