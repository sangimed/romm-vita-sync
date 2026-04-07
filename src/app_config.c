#include "app_config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backup_manager.h"

typedef enum AppConfigSection {
  APP_CONFIG_SECTION_NONE = 0,
  APP_CONFIG_SECTION_ROMM = 1,
  APP_CONFIG_SECTION_DEVICE = 2,
  APP_CONFIG_SECTION_SYNC = 3,
  APP_CONFIG_SECTION_LOG = 4
} AppConfigSection;

/*
 * Returns non-zero when a string is non-null and non-empty.
 */
static int has_text(const char *value) {
  return (value != NULL) && (value[0] != '\0');
}

/*
 * Case-insensitive ASCII equality helper for keys and section names.
 */
static int string_ieq(const char *lhs, const char *rhs) {
  if (lhs == rhs) {
    return 1;
  }
  if ((lhs == NULL) || (rhs == NULL)) {
    return 0;
  }

  while ((*lhs != '\0') && (*rhs != '\0')) {
    unsigned char l = (unsigned char)*lhs;
    unsigned char r = (unsigned char)*rhs;
    if (tolower(l) != tolower(r)) {
      return 0;
    }
    lhs++;
    rhs++;
  }

  return (*lhs == '\0') && (*rhs == '\0');
}

/*
 * Copies text safely into a fixed-size destination buffer.
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
 * Removes trailing CR/LF characters from an in-place line buffer.
 */
static void trim_line_endings(char *line) {
  if (line == NULL) {
    return;
  }

  size_t length = strlen(line);
  while (length > 0U) {
    char c = line[length - 1U];
    if ((c != '\n') && (c != '\r')) {
      break;
    }

    line[length - 1U] = '\0';
    length--;
  }
}

/*
 * Trims leading and trailing whitespace from an in-place string.
 * Returns the pointer to the first non-space character.
 */
static char *trim_whitespace(char *text) {
  if (text == NULL) {
    return NULL;
  }

  while ((*text != '\0') && isspace((unsigned char)*text)) {
    text++;
  }

  size_t length = strlen(text);
  while (length > 0U) {
    char c = text[length - 1U];
    if (!isspace((unsigned char)c)) {
      break;
    }
    text[length - 1U] = '\0';
    length--;
  }

  return text;
}

/*
 * Parses common boolean spellings and returns default_value on unknown input.
 */
static int parse_bool_value(const char *text, int default_value) {
  if (!has_text(text)) {
    return default_value;
  }

  if (string_ieq(text, "1") || string_ieq(text, "true") || string_ieq(text, "yes") || string_ieq(text, "on")) {
    return 1;
  }

  if (string_ieq(text, "0") || string_ieq(text, "false") || string_ieq(text, "no") || string_ieq(text, "off")) {
    return 0;
  }

  return default_value;
}

/*
 * Parses a positive integer and returns fallback_value if parsing fails.
 */
static int parse_positive_int(const char *text, int fallback_value) {
  if (!has_text(text)) {
    return fallback_value;
  }

  char *end = NULL;
  long value = strtol(text, &end, 10);
  if ((end == text) || ((end != NULL) && (*end != '\0')) || (value <= 0L)) {
    return fallback_value;
  }

  if (value > 3600L) {
    return 3600;
  }

  return (int)value;
}

/*
 * Parses log level from number or text.
 */
static int parse_log_level_value(const char *text, int fallback_value) {
  if (!has_text(text)) {
    return fallback_value;
  }

  if (string_ieq(text, "error")) {
    return APP_CONFIG_LOG_LEVEL_ERROR;
  }
  if (string_ieq(text, "warn") || string_ieq(text, "warning")) {
    return APP_CONFIG_LOG_LEVEL_WARN;
  }
  if (string_ieq(text, "info")) {
    return APP_CONFIG_LOG_LEVEL_INFO;
  }
  if (string_ieq(text, "debug") || string_ieq(text, "verbose")) {
    return APP_CONFIG_LOG_LEVEL_DEBUG;
  }

  char *end = NULL;
  long numeric = strtol(text, &end, 10);
  if ((end == text) || ((end != NULL) && (*end != '\0'))) {
    return fallback_value;
  }

  if (numeric < APP_CONFIG_LOG_LEVEL_ERROR) {
    return APP_CONFIG_LOG_LEVEL_ERROR;
  }
  if (numeric > APP_CONFIG_LOG_LEVEL_DEBUG) {
    return APP_CONFIG_LOG_LEVEL_DEBUG;
  }
  return numeric;
}

/*
 * Returns text representation for persisted log level.
 */
static const char *log_level_to_text(int log_level) {
  switch (log_level) {
    case APP_CONFIG_LOG_LEVEL_ERROR:
      return "error";
    case APP_CONFIG_LOG_LEVEL_WARN:
      return "warn";
    case APP_CONFIG_LOG_LEVEL_DEBUG:
      return "debug";
    case APP_CONFIG_LOG_LEVEL_INFO:
    default:
      return "info";
  }
}

/*
 * Removes surrounding single/double quotes when present.
 */
static char *strip_optional_quotes(char *text) {
  if (!has_text(text)) {
    return text;
  }

  size_t length = strlen(text);
  if (length < 2U) {
    return text;
  }

  char first = text[0];
  char last = text[length - 1U];
  if (((first == '"') && (last == '"')) || ((first == '\'') && (last == '\''))) {
    text[length - 1U] = '\0';
    return text + 1;
  }

  return text;
}

/*
 * Converts an ini section name to an internal section enum.
 */
static AppConfigSection parse_section_name(const char *name) {
  if (!has_text(name)) {
    return APP_CONFIG_SECTION_NONE;
  }

  if (string_ieq(name, "RomM")) {
    return APP_CONFIG_SECTION_ROMM;
  }

  if (string_ieq(name, "Device")) {
    return APP_CONFIG_SECTION_DEVICE;
  }

  if (string_ieq(name, "Sync")) {
    return APP_CONFIG_SECTION_SYNC;
  }

  if (string_ieq(name, "Log")) {
    return APP_CONFIG_SECTION_LOG;
  }

  return APP_CONFIG_SECTION_NONE;
}

/*
 * Ensures the parent directory of a config path exists.
 */
static int ensure_parent_directory(const char *path) {
  if (!has_text(path)) {
    return APP_CONFIG_ERR_INVALID_ARGUMENT;
  }

  const char *last_forward = strrchr(path, '/');
  const char *last_backward = strrchr(path, '\\');
  const char *last_separator = last_forward;
  if ((last_backward != NULL) && ((last_separator == NULL) || (last_backward > last_separator))) {
    last_separator = last_backward;
  }

  if (last_separator == NULL) {
    return APP_CONFIG_OK;
  }

  size_t parent_length = (size_t)(last_separator - path);
  if (parent_length == 0U) {
    return APP_CONFIG_OK;
  }

  char parent_path[ROMM_MAX_PATH_LEN];
  if (parent_length >= sizeof(parent_path)) {
    return APP_CONFIG_ERR_INVALID_ARGUMENT;
  }

  memcpy(parent_path, path, parent_length);
  parent_path[parent_length] = '\0';

  int status = backup_manager_ensure_directory(parent_path);
  if (status != BACKUP_MANAGER_OK) {
    return APP_CONFIG_ERR_CREATE_DIRECTORY;
  }

  return APP_CONFIG_OK;
}

/*
 * Applies one parsed key/value pair onto the destination config struct.
 */
static void apply_key_value(
    AppConfig *config,
    AppConfigSection section,
    const char *key,
    const char *value) {
  if ((config == NULL) || !has_text(key) || (value == NULL)) {
    return;
  }

  if (section == APP_CONFIG_SECTION_ROMM) {
    if (string_ieq(key, "url") || string_ieq(key, "base_url")) {
      safe_copy(config->romm_url, sizeof(config->romm_url), value);
    } else if (string_ieq(key, "username")) {
      safe_copy(config->romm_username, sizeof(config->romm_username), value);
    } else if (string_ieq(key, "password")) {
      safe_copy(config->romm_password, sizeof(config->romm_password), value);
    } else if (string_ieq(key, "platform") || string_ieq(key, "platform_filter") || string_ieq(key, "roms_platform")) {
      safe_copy(config->romm_platform_filter, sizeof(config->romm_platform_filter), value);
    } else if (string_ieq(key, "emulator") || string_ieq(key, "save_emulator")) {
      safe_copy(config->romm_save_emulator, sizeof(config->romm_save_emulator), value);
    } else if (string_ieq(key, "verify_tls")) {
      config->romm_verify_tls = parse_bool_value(value, config->romm_verify_tls);
    } else if (string_ieq(key, "timeout_seconds")) {
      config->romm_timeout_seconds = parse_positive_int(value, config->romm_timeout_seconds);
    }
    return;
  }

  if (section == APP_CONFIG_SECTION_DEVICE) {
    if (string_ieq(key, "device_id")) {
      safe_copy(config->device_id, sizeof(config->device_id), value);
    } else if (string_ieq(key, "device_name")) {
      safe_copy(config->device_name, sizeof(config->device_name), value);
    } else if (string_ieq(key, "device_platform")) {
      safe_copy(config->device_platform, sizeof(config->device_platform), value);
    } else if (string_ieq(key, "client")) {
      safe_copy(config->device_client, sizeof(config->device_client), value);
    } else if (string_ieq(key, "client_version")) {
      safe_copy(config->device_client_version, sizeof(config->device_client_version), value);
    }
    return;
  }

  if (section == APP_CONFIG_SECTION_SYNC) {
    if (string_ieq(key, "state_store_path")) {
      safe_copy(config->sync_state_store_path, sizeof(config->sync_state_store_path), value);
    } else if (string_ieq(key, "backup_directory")) {
      safe_copy(config->sync_backup_directory, sizeof(config->sync_backup_directory), value);
    } else if (string_ieq(key, "dry_run")) {
      config->sync_dry_run = parse_bool_value(value, config->sync_dry_run);
    } else if (string_ieq(key, "auto_apply_conflicts") || string_ieq(key, "auto_resolve_conflicts")) {
      config->sync_auto_apply_conflicts =
          parse_bool_value(value, config->sync_auto_apply_conflicts);
    } else if (string_ieq(key, "auto_sync_on_startup") || string_ieq(key, "auto_sync")) {
      config->sync_auto_on_startup = parse_bool_value(value, config->sync_auto_on_startup);
    }
    return;
  }

  if (section == APP_CONFIG_SECTION_LOG) {
    if (string_ieq(key, "level")) {
      config->log_level = parse_log_level_value(value, config->log_level);
    } else if (string_ieq(key, "file_enabled") || string_ieq(key, "file_output_enabled") || string_ieq(key, "file_logging")) {
      config->log_file_enabled = parse_bool_value(value, config->log_file_enabled);
    } else if (string_ieq(key, "verbose")) {
      int verbose_enabled = parse_bool_value(value, config->log_level >= APP_CONFIG_LOG_LEVEL_DEBUG);
      config->log_level = verbose_enabled ? APP_CONFIG_LOG_LEVEL_DEBUG : APP_CONFIG_LOG_LEVEL_INFO;
    } else if (string_ieq(key, "scan_verbose")) {
      config->log_scan_verbose = parse_bool_value(value, config->log_scan_verbose);
    }
  }
}

/*
 * Initializes settings with safe defaults.
 */
void app_config_init_defaults(AppConfig *config) {
  if (config == NULL) {
    return;
  }

  memset(config, 0, sizeof(*config));

  safe_copy(config->romm_platform_filter, sizeof(config->romm_platform_filter), "psx");
  safe_copy(config->romm_save_emulator, sizeof(config->romm_save_emulator), "pcsx_rearmed");
  config->romm_verify_tls = 1;
  config->romm_timeout_seconds = 30;

  safe_copy(config->device_name, sizeof(config->device_name), "PS Vita");
  safe_copy(config->device_platform, sizeof(config->device_platform), "PSVita");
  safe_copy(config->device_client, sizeof(config->device_client), "romm-vita-sync");
  safe_copy(config->device_client_version, sizeof(config->device_client_version), "0.1");

  safe_copy(config->sync_state_store_path, sizeof(config->sync_state_store_path), APP_CONFIG_DEFAULT_STATE_STORE_PATH);
  safe_copy(config->sync_backup_directory, sizeof(config->sync_backup_directory), APP_CONFIG_DEFAULT_BACKUP_DIRECTORY);
  config->sync_dry_run = 1;
  config->sync_auto_apply_conflicts = 0;
  config->sync_auto_on_startup = 0;

  config->log_level = APP_CONFIG_LOG_LEVEL_DEBUG;
  config->log_file_enabled = 0;
  config->log_scan_verbose = 0;
}

/*
 * Loads settings from an ini-like file.
 */
int app_config_load(const char *path, AppConfig *out_config) {
  if ((path == NULL) || (out_config == NULL) || (path[0] == '\0')) {
    return APP_CONFIG_ERR_INVALID_ARGUMENT;
  }

  app_config_init_defaults(out_config);

  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    if (errno == ENOENT) {
      return APP_CONFIG_ERR_NOT_FOUND;
    }
    return APP_CONFIG_ERR_OPEN;
  }

  char line[1024];
  AppConfigSection section = APP_CONFIG_SECTION_NONE;

  while (fgets(line, sizeof(line), file) != NULL) {
    trim_line_endings(line);

    char *cursor = trim_whitespace(line);
    if ((cursor == NULL) || (cursor[0] == '\0')) {
      continue;
    }

    if ((cursor[0] == '#') || (cursor[0] == ';')) {
      continue;
    }

    if (cursor[0] == '[') {
      char *end = strchr(cursor, ']');
      if (end == NULL) {
        fclose(file);
        return APP_CONFIG_ERR_FORMAT;
      }

      *end = '\0';
      char *name = trim_whitespace(cursor + 1);
      section = parse_section_name(name);
      continue;
    }

    char *equals = strchr(cursor, '=');
    if (equals == NULL) {
      fclose(file);
      return APP_CONFIG_ERR_FORMAT;
    }

    *equals = '\0';
    char *key = trim_whitespace(cursor);
    char *value = trim_whitespace(equals + 1);
    value = strip_optional_quotes(value);

    apply_key_value(out_config, section, key, value);
  }

  if (ferror(file)) {
    fclose(file);
    return APP_CONFIG_ERR_READ;
  }

  fclose(file);
  return APP_CONFIG_OK;
}

/*
 * Updates device_id in memory with basic validation.
 */
int app_config_set_device_id(AppConfig *config, const char *device_id) {
  if ((config == NULL) || !has_text(device_id)) {
    return APP_CONFIG_ERR_INVALID_ARGUMENT;
  }

  safe_copy(config->device_id, sizeof(config->device_id), device_id);
  return APP_CONFIG_OK;
}

/*
 * Saves settings to an ini-like file.
 */
int app_config_save(const char *path, const AppConfig *config) {
  if ((path == NULL) || (config == NULL) || (path[0] == '\0')) {
    return APP_CONFIG_ERR_INVALID_ARGUMENT;
  }

  int ensure_status = ensure_parent_directory(path);
  if (ensure_status != APP_CONFIG_OK) {
    return ensure_status;
  }

  FILE *file = fopen(path, "wb");
  if (file == NULL) {
    return APP_CONFIG_ERR_OPEN;
  }

  int status = APP_CONFIG_OK;
  if (fprintf(file, "; Generated by romm-vita-sync\n\n") < 0) {
    status = APP_CONFIG_ERR_WRITE;
  }

  if ((status == APP_CONFIG_OK) && (fprintf(file, "[RomM]\n") < 0)) {
    status = APP_CONFIG_ERR_WRITE;
  }
  if ((status == APP_CONFIG_OK) && (fprintf(file, "url = %s\n", config->romm_url) < 0)) {
    status = APP_CONFIG_ERR_WRITE;
  }
  if ((status == APP_CONFIG_OK) && (fprintf(file, "username = %s\n", config->romm_username) < 0)) {
    status = APP_CONFIG_ERR_WRITE;
  }
  if ((status == APP_CONFIG_OK) && (fprintf(file, "password = %s\n", config->romm_password) < 0)) {
    status = APP_CONFIG_ERR_WRITE;
  }
  if ((status == APP_CONFIG_OK) && (fprintf(file, "platform = %s\n", config->romm_platform_filter) < 0)) {
    status = APP_CONFIG_ERR_WRITE;
  }
  if ((status == APP_CONFIG_OK) && (fprintf(file, "emulator = %s\n", config->romm_save_emulator) < 0)) {
    status = APP_CONFIG_ERR_WRITE;
  }
  if ((status == APP_CONFIG_OK) && (fprintf(file, "verify_tls = %s\n", config->romm_verify_tls ? "true" : "false") < 0)) {
    status = APP_CONFIG_ERR_WRITE;
  }
  if ((status == APP_CONFIG_OK) && (fprintf(file, "timeout_seconds = %d\n\n", config->romm_timeout_seconds) < 0)) {
    status = APP_CONFIG_ERR_WRITE;
  }

  if ((status == APP_CONFIG_OK) && (fprintf(file, "[Device]\n") < 0)) {
    status = APP_CONFIG_ERR_WRITE;
  }
  if ((status == APP_CONFIG_OK) && (fprintf(file, "device_id = %s\n", config->device_id) < 0)) {
    status = APP_CONFIG_ERR_WRITE;
  }
  if ((status == APP_CONFIG_OK) && (fprintf(file, "device_name = %s\n", config->device_name) < 0)) {
    status = APP_CONFIG_ERR_WRITE;
  }
  if ((status == APP_CONFIG_OK) && (fprintf(file, "device_platform = %s\n", config->device_platform) < 0)) {
    status = APP_CONFIG_ERR_WRITE;
  }
  if ((status == APP_CONFIG_OK) && (fprintf(file, "client = %s\n", config->device_client) < 0)) {
    status = APP_CONFIG_ERR_WRITE;
  }
  if ((status == APP_CONFIG_OK) && (fprintf(file, "client_version = %s\n\n", config->device_client_version) < 0)) {
    status = APP_CONFIG_ERR_WRITE;
  }

  if ((status == APP_CONFIG_OK) && (fprintf(file, "[Sync]\n") < 0)) {
    status = APP_CONFIG_ERR_WRITE;
  }
  if ((status == APP_CONFIG_OK) && (fprintf(file, "state_store_path = %s\n", config->sync_state_store_path) < 0)) {
    status = APP_CONFIG_ERR_WRITE;
  }
  if ((status == APP_CONFIG_OK) && (fprintf(file, "backup_directory = %s\n", config->sync_backup_directory) < 0)) {
    status = APP_CONFIG_ERR_WRITE;
  }
  if ((status == APP_CONFIG_OK) && (fprintf(file, "dry_run = %s\n", config->sync_dry_run ? "true" : "false") < 0)) {
    status = APP_CONFIG_ERR_WRITE;
  }
  if ((status == APP_CONFIG_OK) &&
      (fprintf(
           file,
           "auto_apply_conflicts = %s\n",
           config->sync_auto_apply_conflicts ? "true" : "false") < 0)) {
    status = APP_CONFIG_ERR_WRITE;
  }
  if ((status == APP_CONFIG_OK) && (fprintf(file, "auto_sync_on_startup = %s\n", config->sync_auto_on_startup ? "true" : "false") < 0)) {
    status = APP_CONFIG_ERR_WRITE;
  }
  if ((status == APP_CONFIG_OK) && (fprintf(file, "\n[Log]\n") < 0)) {
    status = APP_CONFIG_ERR_WRITE;
  }
  if ((status == APP_CONFIG_OK) && (fprintf(file, "level = %s\n", log_level_to_text(config->log_level)) < 0)) {
    status = APP_CONFIG_ERR_WRITE;
  }
  if ((status == APP_CONFIG_OK) && (fprintf(file, "file_enabled = %s\n", config->log_file_enabled ? "true" : "false") < 0)) {
    status = APP_CONFIG_ERR_WRITE;
  }
  if ((status == APP_CONFIG_OK) && (fprintf(file, "scan_verbose = %s\n", config->log_scan_verbose ? "true" : "false") < 0)) {
    status = APP_CONFIG_ERR_WRITE;
  }

  if ((status == APP_CONFIG_OK) && fflush(file) != 0) {
    status = APP_CONFIG_ERR_WRITE;
  }

  fclose(file);
  return status;
}

/*
 * Returns non-zero when a server URL is configured.
 */
int app_config_has_server_url(const AppConfig *config) {
  if (config == NULL) {
    return 0;
  }

  return has_text(config->romm_url);
}

/*
 * Returns non-zero when username/password authentication is configured.
 */
int app_config_has_auth(const AppConfig *config) {
  if (config == NULL) {
    return 0;
  }

  return has_text(config->romm_username) && has_text(config->romm_password);
}

/*
 * Returns a readable message for a configuration status code.
 */
const char *app_config_status_str(int status) {
  switch (status) {
    case APP_CONFIG_OK:
      return "ok";
    case APP_CONFIG_ERR_INVALID_ARGUMENT:
      return "invalid argument";
    case APP_CONFIG_ERR_NOT_FOUND:
      return "file not found";
    case APP_CONFIG_ERR_OPEN:
      return "file open failed";
    case APP_CONFIG_ERR_READ:
      return "file read failed";
    case APP_CONFIG_ERR_FORMAT:
      return "invalid format";
    case APP_CONFIG_ERR_CREATE_DIRECTORY:
      return "directory creation failed";
    case APP_CONFIG_ERR_WRITE:
      return "file write failed";
    default:
      return "unknown error";
  }
}
