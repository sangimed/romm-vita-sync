#include "romm_http_client.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/libssl.h>
#include <psp2/net/http.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include "app_config.h"
#include "app_log.h"
#include "game_matcher.h"
#include "romm_client.h"

#define ROMM_NET_POOL_SIZE (1024 * 1024)
#define ROMM_HTTP_POOL_SIZE (256 * 1024)
#define ROMM_SSL_POOL_SIZE (256 * 1024)
#define ROMM_HTTP_MAX_BODY_SIZE (128 * 1024)
#define ROMM_HTTP_SMALL_BODY_SIZE 4096
#define ROMM_HTTP_MAX_ROMS 4096
#define ROMM_HTTP_PAGE_LIMIT 200
#define ROMM_HTTP_MAX_UPLOAD_SIZE (2 * 1024 * 1024)
#define ROMM_HTTP_MAX_FILENAME 128
#define ROMM_HTTP_MAX_PLATFORM_SLUG 64
#define ROMM_HTTP_FALLBACK_URL_BUFFER_SIZE (APP_CONFIG_MAX_URL_LEN + 256)
#define ROMM_HTTP_SSL_TRANSPORT_ERROR 0x80431075u
#define ROMM_HTTP_SAVE_EMULATOR "pcsx_rearmed"
/*
 * Keep verify-tls override compatible with retail firmware behavior.
 * Some Vita builds reject multi-flag disable masks with 0x8043506B.
 * Disabling SERVER_VERIFY is sufficient to skip certificate checks.
 */
#define ROMM_HTTPS_VERIFY_OVERRIDE_FLAG (SCE_HTTPS_FLAG_SERVER_VERIFY)

typedef struct HttpRuntimeState {
  void *net_memory;
  int net_inited;
  int netctl_inited;
  int http_inited;
  int ssl_inited;
  int tls_verify_disabled;
  int module_net_loaded;
  int module_http_loaded;
  int module_ssl_loaded;
  int module_https_loaded;
} HttpRuntimeState;

typedef GameMatcherRomCandidate RomCatalogEntry;

static void http_runtime_term(HttpRuntimeState *state);

/*
 * Returns non-zero when a string is non-null and non-empty.
 */
static int has_text(const char *value) {
  return (value != NULL) && (value[0] != '\0');
}

/*
 * Returns non-zero when url starts with "https://".
 */
static int url_is_https(const char *url) {
  if (!has_text(url)) {
    return 0;
  }

  return strncmp(url, "https://", 8) == 0;
}

/*
 * Builds an HTTP URL from an HTTPS URL by swapping the scheme.
 */
static int build_http_fallback_url(const char *https_url, char *out_url, size_t out_url_size) {
  if (!url_is_https(https_url) || (out_url == NULL) || (out_url_size == 0U)) {
    return -1;
  }

  int written = snprintf(out_url, out_url_size, "http://%s", https_url + 8);
  if ((written <= 0) || ((size_t)written >= out_url_size)) {
    return -1;
  }

  return 0;
}

/*
 * Loads a user sysmodule, treating "already loaded" as success.
 */
static int load_user_module(int module_id, int *out_loaded_now) {
  if (out_loaded_now != NULL) {
    *out_loaded_now = 0;
  }

  int already_loaded = (sceSysmoduleIsLoaded(module_id) == 0);
  int status = sceSysmoduleLoadModule(module_id);
  if (status < 0) {
    return status;
  }

  if (out_loaded_now != NULL) {
    *out_loaded_now = already_loaded ? 0 : 1;
  }
  return 0;
}

/*
 * Unloads a user sysmodule only when this code loaded it.
 */
static void unload_user_module_if_loaded(int module_id, int loaded_now) {
  if (!loaded_now) {
    return;
  }

  int status = sceSysmoduleUnloadModule(module_id);
  if ((status < 0) && (status != SCE_SYSMODULE_ERROR_UNLOADED)) {
    app_log_write(APP_LOG_LEVEL_WARN, "http", "sceSysmoduleUnloadModule(%d) failed: 0x%08X", module_id, (unsigned int)status);
  }
}

/*
 * Builds an API URL from base URL and endpoint path.
 */
static int build_api_url(const char *base_url, const char *path, char *out_url, size_t out_url_size) {
  if (!has_text(base_url) || !has_text(path) || (out_url == NULL) || (out_url_size == 0U)) {
    return -1;
  }

  size_t base_len = strlen(base_url);
  size_t path_offset = 0U;
  if ((base_len > 0U) && (base_url[base_len - 1U] == '/') && (path[0] == '/')) {
    path_offset = 1U;
  }

  int written = snprintf(out_url, out_url_size, "%s%s", base_url, path + path_offset);
  if ((written <= 0) || ((size_t)written >= out_url_size)) {
    return -1;
  }

  return 0;
}

/*
 * Escapes a string for safe JSON string insertion.
 */
static int json_escape(const char *input, char *output, size_t output_size) {
  if ((input == NULL) || (output == NULL) || (output_size == 0U)) {
    return -1;
  }

  size_t out = 0U;
  for (const unsigned char *cursor = (const unsigned char *)input; *cursor != '\0'; ++cursor) {
    char sequence[3];
    size_t sequence_len = 0U;

    switch (*cursor) {
      case '\"':
        sequence[0] = '\\';
        sequence[1] = '\"';
        sequence_len = 2U;
        break;
      case '\\':
        sequence[0] = '\\';
        sequence[1] = '\\';
        sequence_len = 2U;
        break;
      case '\n':
        sequence[0] = '\\';
        sequence[1] = 'n';
        sequence_len = 2U;
        break;
      case '\r':
        sequence[0] = '\\';
        sequence[1] = 'r';
        sequence_len = 2U;
        break;
      case '\t':
        sequence[0] = '\\';
        sequence[1] = 't';
        sequence_len = 2U;
        break;
      default:
        if (*cursor < 0x20U) {
          sequence[0] = '?';
          sequence_len = 1U;
        } else {
          sequence[0] = (char)(*cursor);
          sequence_len = 1U;
        }
        break;
    }

    if (out + sequence_len >= output_size) {
      output[0] = '\0';
      return -1;
    }

    for (size_t i = 0U; i < sequence_len; ++i) {
      output[out++] = sequence[i];
    }
  }

  output[out] = '\0';
  return 0;
}

/*
 * Base64-encodes input for HTTP Basic authentication.
 */
static int base64_encode(const unsigned char *input, size_t input_size, char *output, size_t output_size) {
  static const char kBase64Table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  if ((input == NULL) || (output == NULL) || (output_size == 0U)) {
    return -1;
  }

  size_t required = 4U * ((input_size + 2U) / 3U);
  if (required + 1U > output_size) {
    return -1;
  }

  size_t in = 0U;
  size_t out = 0U;
  while ((in + 2U) < input_size) {
    unsigned int a = input[in++];
    unsigned int b = input[in++];
    unsigned int c = input[in++];
    unsigned int triple = (a << 16) | (b << 8) | c;

    output[out++] = kBase64Table[(triple >> 18) & 0x3F];
    output[out++] = kBase64Table[(triple >> 12) & 0x3F];
    output[out++] = kBase64Table[(triple >> 6) & 0x3F];
    output[out++] = kBase64Table[triple & 0x3F];
  }

  size_t remainder = input_size - in;
  if (remainder == 1U) {
    unsigned int triple = ((unsigned int)input[in]) << 16;
    output[out++] = kBase64Table[(triple >> 18) & 0x3F];
    output[out++] = kBase64Table[(triple >> 12) & 0x3F];
    output[out++] = '=';
    output[out++] = '=';
  } else if (remainder == 2U) {
    unsigned int triple = (((unsigned int)input[in]) << 16) | (((unsigned int)input[in + 1U]) << 8);
    output[out++] = kBase64Table[(triple >> 18) & 0x3F];
    output[out++] = kBase64Table[(triple >> 12) & 0x3F];
    output[out++] = kBase64Table[(triple >> 6) & 0x3F];
    output[out++] = '=';
  }

  output[out] = '\0';
  return 0;
}

/*
 * Builds Authorization header value from token or username/password config.
 */
static int build_auth_value(const AppConfig *config, char *out_value, size_t out_value_size) {
  if ((config == NULL) || (out_value == NULL) || (out_value_size == 0U)) {
    return -1;
  }

  out_value[0] = '\0';
  if (has_text(config->romm_token)) {
    int written = snprintf(out_value, out_value_size, "Bearer %s", config->romm_token);
    return ((written > 0) && ((size_t)written < out_value_size)) ? 0 : -1;
  }

  if (has_text(config->romm_username) && has_text(config->romm_password)) {
    char credentials[APP_CONFIG_MAX_USERNAME_LEN + APP_CONFIG_MAX_PASSWORD_LEN + 2];
    int credentials_written = snprintf(credentials, sizeof(credentials), "%s:%s", config->romm_username, config->romm_password);
    if ((credentials_written <= 0) || ((size_t)credentials_written >= sizeof(credentials))) {
      return -1;
    }

    char encoded[512];
    if (base64_encode((const unsigned char *)credentials, strlen(credentials), encoded, sizeof(encoded)) < 0) {
      return -1;
    }

    int written = snprintf(out_value, out_value_size, "Basic %s", encoded);
    return ((written > 0) && ((size_t)written < out_value_size)) ? 0 : -1;
  }

  return -1;
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
 * Returns lowercase alnum-only view of identifiers for fuzzy matching.
 */
static void normalize_identifier(const char *input, char *output, size_t output_size) {
  if ((output == NULL) || (output_size == 0U)) {
    return;
  }

  output[0] = '\0';
  if (!has_text(input)) {
    return;
  }

  size_t out = 0U;
  for (const unsigned char *cursor = (const unsigned char *)input;
       (*cursor != '\0') && ((out + 1U) < output_size);
       ++cursor) {
    unsigned char c = *cursor;
    if (isalnum(c)) {
      output[out++] = (char)tolower(c);
    }
  }
  output[out] = '\0';
}

/*
 * Parses a decimal scalar from text to int with range checks.
 */
static int parse_int_value(const char *text, int *out_value) {
  if (!has_text(text) || (out_value == NULL)) {
    return -1;
  }

  char *end = NULL;
  long value = strtol(text, &end, 10);
  if ((end == text) || ((end != NULL) && (*end != '\0')) || (value < INT_MIN) || (value > INT_MAX)) {
    return -1;
  }

  *out_value = (int)value;
  return 0;
}

/*
 * Parses decimal scalar to uint64.
 */
static int parse_uint64_value(const char *text, uint64_t *out_value) {
  if (!has_text(text) || (out_value == NULL)) {
    return -1;
  }

  char *end = NULL;
  unsigned long long value = strtoull(text, &end, 10);
  if ((end == text) || ((end != NULL) && (*end != '\0'))) {
    return -1;
  }

  *out_value = (uint64_t)value;
  return 0;
}

/*
 * Converts ISO-like timestamp to deterministic unix timestamp (seconds precision).
 */
static int parse_iso_timestamp(const char *timestamp, int64_t *out_unix) {
  if (!has_text(timestamp) || (out_unix == NULL)) {
    return -1;
  }

  if (strlen(timestamp) < 19U) {
    return -1;
  }

  char normalized[20];
  memcpy(normalized, timestamp, 19U);
  normalized[19] = '\0';
  if (normalized[10] == 'T') {
    normalized[10] = ' ';
  }

  return sync_parse_local_timestamp(normalized, out_unix);
}

/*
 * Maps server slot values to Vita slot enum.
 */
static SyncSlot parse_slot_value(const char *slot_text, const char *filename) {
  if (has_text(slot_text)) {
    if (sync_string_ieq(slot_text, "slot0") || sync_string_ieq(slot_text, "0")) {
      return SYNC_SLOT_0;
    }
    if (sync_string_ieq(slot_text, "slot1") || sync_string_ieq(slot_text, "1")) {
      return SYNC_SLOT_1;
    }
  }

  SyncSlot parsed = SYNC_SLOT_UNKNOWN;
  if (has_text(filename) && (sync_slot_from_filename(filename, &parsed) == 0)) {
    return parsed;
  }

  return SYNC_SLOT_UNKNOWN;
}

/*
 * Returns canonical slot query value for upload endpoint.
 */
static const char *slot_to_query_value(SyncSlot slot) {
  switch (slot) {
    case SYNC_SLOT_0:
      return "slot0";
    case SYNC_SLOT_1:
      return "slot1";
    case SYNC_SLOT_UNKNOWN:
    default:
      return NULL;
  }
}

/*
 * Builds deterministic upload filename to keep remote entries readable.
 */
static void build_upload_filename(const SyncSaveDescriptor *local_item, char *out_filename, size_t out_size) {
  if ((out_filename == NULL) || (out_size == 0U)) {
    return;
  }

  out_filename[0] = '\0';
  if (local_item == NULL) {
    safe_copy(out_filename, out_size, "save.srm");
    return;
  }

  char normalized_game_id[ROMM_GAME_ID_LEN];
  normalize_identifier(local_item->game_id, normalized_game_id, sizeof(normalized_game_id));
  if (!has_text(normalized_game_id)) {
    safe_copy(normalized_game_id, sizeof(normalized_game_id), "ps1save");
  }

  const char *slot = "unknown";
  if (local_item->slot == SYNC_SLOT_0) {
    slot = "slot0";
  } else if (local_item->slot == SYNC_SLOT_1) {
    slot = "slot1";
  }

  snprintf(out_filename, out_size, "%s_%s.srm", normalized_game_id, slot);
}

/*
 * Reads a local file entirely into memory, bounded for upload safety.
 */
static int read_file_to_memory(const char *path, unsigned char **out_data, size_t *out_size) {
  if (!has_text(path) || (out_data == NULL) || (out_size == NULL)) {
    return -1;
  }

  *out_data = NULL;
  *out_size = 0U;

  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    return -1;
  }

  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return -1;
  }
  long length = ftell(file);
  if ((length < 0L) || ((size_t)length > ROMM_HTTP_MAX_UPLOAD_SIZE)) {
    fclose(file);
    return -1;
  }
  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return -1;
  }

  unsigned char *buffer = (unsigned char *)malloc((size_t)length);
  if ((buffer == NULL) && (length > 0L)) {
    fclose(file);
    return -1;
  }

  size_t read_size = 0U;
  if (length > 0L) {
    read_size = fread(buffer, 1U, (size_t)length, file);
    if (read_size != (size_t)length) {
      free(buffer);
      fclose(file);
      return -1;
    }
  }

  fclose(file);
  *out_data = buffer;
  *out_size = read_size;
  return 0;
}

/*
 * Extracts a string field value from a small JSON payload.
 */
static int extract_json_string_field(
    const char *json,
    const char *field_name,
    char *out_value,
    size_t out_value_size) {
  if (!has_text(json) || !has_text(field_name) || (out_value == NULL) || (out_value_size == 0U)) {
    return -1;
  }

  out_value[0] = '\0';

  char pattern[80];
  int pattern_written = snprintf(pattern, sizeof(pattern), "\"%s\"", field_name);
  if ((pattern_written <= 0) || ((size_t)pattern_written >= sizeof(pattern))) {
    return -1;
  }

  const char *field = strstr(json, pattern);
  if (field == NULL) {
    return -1;
  }

  const char *colon = strchr(field + pattern_written, ':');
  if (colon == NULL) {
    return -1;
  }

  const char *cursor = colon + 1;
  while ((*cursor != '\0') && isspace((unsigned char)*cursor)) {
    cursor++;
  }
  if (*cursor != '\"') {
    return -1;
  }
  cursor++;

  size_t out = 0U;
  while ((*cursor != '\0') && (*cursor != '\"')) {
    if ((out + 1U) >= out_value_size) {
      return -1;
    }

    if ((*cursor == '\\') && (cursor[1] != '\0')) {
      cursor++;
      switch (*cursor) {
        case 'n':
          out_value[out++] = '\n';
          break;
        case 'r':
          out_value[out++] = '\r';
          break;
        case 't':
          out_value[out++] = '\t';
          break;
        default:
          out_value[out++] = *cursor;
          break;
      }
    } else {
      out_value[out++] = *cursor;
    }
    cursor++;
  }

  if (*cursor != '\"') {
    return -1;
  }

  out_value[out] = '\0';
  return 0;
}

/*
 * Extracts an unquoted scalar field from a small JSON payload.
 */
static int extract_json_scalar_field(
    const char *json,
    const char *field_name,
    char *out_value,
    size_t out_value_size) {
  if (!has_text(json) || !has_text(field_name) || (out_value == NULL) || (out_value_size == 0U)) {
    return -1;
  }

  out_value[0] = '\0';

  char pattern[80];
  int pattern_written = snprintf(pattern, sizeof(pattern), "\"%s\"", field_name);
  if ((pattern_written <= 0) || ((size_t)pattern_written >= sizeof(pattern))) {
    return -1;
  }

  const char *field = strstr(json, pattern);
  if (field == NULL) {
    return -1;
  }

  const char *colon = strchr(field + pattern_written, ':');
  if (colon == NULL) {
    return -1;
  }

  const char *cursor = colon + 1;
  while ((*cursor != '\0') && isspace((unsigned char)*cursor)) {
    cursor++;
  }
  if ((*cursor == '\"') || (*cursor == '{') || (*cursor == '[')) {
    return -1;
  }

  size_t out = 0U;
  while ((*cursor != '\0') && (*cursor != ',') && (*cursor != '}') && !isspace((unsigned char)*cursor)) {
    if ((out + 1U) >= out_value_size) {
      return -1;
    }
    out_value[out++] = *cursor;
    cursor++;
  }

  if (out == 0U) {
    return -1;
  }

  out_value[out] = '\0';
  return 0;
}

/*
 * Reads HTTP response body into a bounded buffer.
 */
static int read_response_body(int request_id, char *out_body, size_t out_body_size) {
  if ((request_id < 0) || (out_body == NULL) || (out_body_size == 0U)) {
    return -1;
  }

  size_t offset = 0U;
  while (offset + 1U < out_body_size) {
    unsigned int chunk = (unsigned int)(out_body_size - 1U - offset);
    int read = sceHttpReadData(request_id, out_body + offset, chunk);
    if (read < 0) {
      out_body[offset] = '\0';
      return read;
    }
    if (read == 0) {
      break;
    }
    offset += (size_t)read;
  }

  out_body[offset] = '\0';
  return 0;
}

/*
 * Finds the matching closing bracket while skipping string literals.
 */
static const char *find_matching_bracket(const char *start, char open_char, char close_char) {
  if (start == NULL) {
    return NULL;
  }

  int depth = 0;
  int in_string = 0;
  int escaped = 0;
  for (const char *cursor = start; *cursor != '\0'; ++cursor) {
    char c = *cursor;
    if (in_string) {
      if (escaped) {
        escaped = 0;
      } else if (c == '\\') {
        escaped = 1;
      } else if (c == '\"') {
        in_string = 0;
      }
      continue;
    }

    if (c == '\"') {
      in_string = 1;
      continue;
    }

    if (c == open_char) {
      depth++;
    } else if (c == close_char) {
      depth--;
      if (depth == 0) {
        return cursor;
      }
    }
  }

  return NULL;
}

/*
 * Locates an array range from a JSON field ("items") or root array.
 */
static int find_object_array_range(
    const char *json,
    const char *field_name,
    const char **out_array_start,
    const char **out_array_end) {
  if ((json == NULL) || (out_array_start == NULL) || (out_array_end == NULL)) {
    return -1;
  }

  *out_array_start = NULL;
  *out_array_end = NULL;

  const char *array_start = NULL;
  if (has_text(field_name)) {
    char pattern[80];
    int pattern_written = snprintf(pattern, sizeof(pattern), "\"%s\"", field_name);
    if ((pattern_written > 0) && ((size_t)pattern_written < sizeof(pattern))) {
      const char *field = strstr(json, pattern);
      if (field != NULL) {
        array_start = strchr(field + pattern_written, '[');
      }
    }
  }

  if (array_start == NULL) {
    const char *cursor = json;
    while ((*cursor != '\0') && isspace((unsigned char)*cursor)) {
      cursor++;
    }
    if (*cursor == '[') {
      array_start = cursor;
    }
  }

  if (array_start == NULL) {
    return -1;
  }

  const char *array_end = find_matching_bracket(array_start, '[', ']');
  if (array_end == NULL) {
    return -1;
  }

  *out_array_start = array_start;
  *out_array_end = array_end;
  return 0;
}

/*
 * Iterates to the next JSON object in an array range.
 */
static int next_object_in_array(
    const char *cursor,
    const char *array_end,
    const char **out_object_start,
    const char **out_object_end,
    const char **out_next_cursor) {
  if ((cursor == NULL) || (array_end == NULL) ||
      (out_object_start == NULL) || (out_object_end == NULL) || (out_next_cursor == NULL)) {
    return -1;
  }

  const char *scan = cursor;
  while ((scan < array_end) && (isspace((unsigned char)*scan) || (*scan == ','))) {
    scan++;
  }
  if ((scan >= array_end) || (*scan != '{')) {
    return -1;
  }

  const char *object_end = find_matching_bracket(scan, '{', '}');
  if ((object_end == NULL) || (object_end > array_end)) {
    return -1;
  }

  *out_object_start = scan;
  *out_object_end = object_end + 1;
  *out_next_cursor = object_end + 1;
  return 0;
}

/*
 * Extracts a JSON string field from a [start,end) range.
 */
static int extract_json_string_field_in_range(
    const char *start,
    const char *end,
    const char *field_name,
    char *out_value,
    size_t out_value_size) {
  if ((start == NULL) || (end == NULL) || (start >= end) ||
      !has_text(field_name) || (out_value == NULL) || (out_value_size == 0U)) {
    return -1;
  }

  out_value[0] = '\0';
  char pattern[80];
  int pattern_written = snprintf(pattern, sizeof(pattern), "\"%s\"", field_name);
  if ((pattern_written <= 0) || ((size_t)pattern_written >= sizeof(pattern))) {
    return -1;
  }

  const char *field = start;
  while ((field = strstr(field, pattern)) != NULL) {
    if (field >= end) {
      return -1;
    }

    const char *colon = strchr(field + pattern_written, ':');
    if ((colon == NULL) || (colon >= end)) {
      field += pattern_written;
      continue;
    }

    const char *cursor = colon + 1;
    while ((cursor < end) && isspace((unsigned char)*cursor)) {
      cursor++;
    }
    if ((cursor >= end) || (*cursor != '\"')) {
      field += pattern_written;
      continue;
    }
    cursor++;

    size_t out = 0U;
    while ((cursor < end) && (*cursor != '\"')) {
      if ((out + 1U) >= out_value_size) {
        return -1;
      }
      if ((*cursor == '\\') && ((cursor + 1) < end)) {
        cursor++;
        switch (*cursor) {
          case 'n':
            out_value[out++] = '\n';
            break;
          case 'r':
            out_value[out++] = '\r';
            break;
          case 't':
            out_value[out++] = '\t';
            break;
          default:
            out_value[out++] = *cursor;
            break;
        }
      } else {
        out_value[out++] = *cursor;
      }
      cursor++;
    }

    if ((cursor < end) && (*cursor == '\"')) {
      out_value[out] = '\0';
      return 0;
    }

    return -1;
  }

  return -1;
}

/*
 * Extracts an unquoted JSON scalar from a [start,end) range.
 */
static int extract_json_scalar_field_in_range(
    const char *start,
    const char *end,
    const char *field_name,
    char *out_value,
    size_t out_value_size) {
  if ((start == NULL) || (end == NULL) || (start >= end) ||
      !has_text(field_name) || (out_value == NULL) || (out_value_size == 0U)) {
    return -1;
  }

  out_value[0] = '\0';
  char pattern[80];
  int pattern_written = snprintf(pattern, sizeof(pattern), "\"%s\"", field_name);
  if ((pattern_written <= 0) || ((size_t)pattern_written >= sizeof(pattern))) {
    return -1;
  }

  const char *field = start;
  while ((field = strstr(field, pattern)) != NULL) {
    if (field >= end) {
      return -1;
    }

    const char *colon = strchr(field + pattern_written, ':');
    if ((colon == NULL) || (colon >= end)) {
      field += pattern_written;
      continue;
    }

    const char *cursor = colon + 1;
    while ((cursor < end) && isspace((unsigned char)*cursor)) {
      cursor++;
    }
    if ((cursor >= end) || (*cursor == '\"') || (*cursor == '{') || (*cursor == '[')) {
      field += pattern_written;
      continue;
    }

    size_t out = 0U;
    while ((cursor < end) && (*cursor != ',') && (*cursor != '}') && (*cursor != ']') &&
           !isspace((unsigned char)*cursor)) {
      if ((out + 1U) >= out_value_size) {
        return -1;
      }
      out_value[out++] = *cursor;
      cursor++;
    }

    if (out == 0U) {
      return -1;
    }
    out_value[out] = '\0';
    return 0;
  }

  return -1;
}

/*
 * Appends one value to a pipe-delimited list while avoiding duplicates.
 */
static int append_pipe_list_unique(char *io_list, size_t io_list_size, const char *value) {
  if ((io_list == NULL) || (io_list_size == 0U) || (value == NULL)) {
    return -1;
  }

  const char *begin = value;
  while ((*begin != '\0') && isspace((unsigned char)*begin)) {
    begin++;
  }
  const char *end = begin + strlen(begin);
  while ((end > begin) && isspace((unsigned char)*(end - 1))) {
    end--;
  }
  if (begin == end) {
    return -1;
  }

  char trimmed[GAME_MATCHER_MAX_SERIAL_LIST_LEN];
  size_t trimmed_len = (size_t)(end - begin);
  if (trimmed_len >= sizeof(trimmed)) {
    trimmed_len = sizeof(trimmed) - 1U;
  }
  memcpy(trimmed, begin, trimmed_len);
  trimmed[trimmed_len] = '\0';

  char current[GAME_MATCHER_MAX_SERIAL_LIST_LEN];
  size_t current_len = 0U;
  for (const char *cursor = io_list;; ++cursor) {
    char c = *cursor;
    if ((c != '|') && (c != '\0') && ((current_len + 1U) < sizeof(current))) {
      current[current_len++] = c;
      continue;
    }

    if (current_len > 0U) {
      current[current_len] = '\0';
      if (sync_string_ieq(current, trimmed)) {
        return 0;
      }
      current_len = 0U;
    }

    if (c == '\0') {
      break;
    }
  }

  if (io_list[0] != '\0') {
    size_t used = strlen(io_list);
    if (used + 1U >= io_list_size) {
      return -1;
    }
    io_list[used] = '|';
    io_list[used + 1U] = '\0';
  }

  size_t required = strlen(io_list) + strlen(trimmed) + 1U;
  if (required > io_list_size) {
    return -1;
  }
  strncat(io_list, trimmed, io_list_size - strlen(io_list) - 1U);
  return 0;
}

/*
 * Extracts a JSON string array field and returns pipe-delimited values.
 */
static int extract_json_string_array_field_in_range(
    const char *start,
    const char *end,
    const char *field_name,
    char *out_value,
    size_t out_value_size) {
  if ((start == NULL) || (end == NULL) || (start >= end) ||
      !has_text(field_name) || (out_value == NULL) || (out_value_size == 0U)) {
    return -1;
  }

  out_value[0] = '\0';
  char pattern[80];
  int pattern_written = snprintf(pattern, sizeof(pattern), "\"%s\"", field_name);
  if ((pattern_written <= 0) || ((size_t)pattern_written >= sizeof(pattern))) {
    return -1;
  }

  const char *field = start;
  while ((field = strstr(field, pattern)) != NULL) {
    if (field >= end) {
      break;
    }

    const char *colon = strchr(field + pattern_written, ':');
    if ((colon == NULL) || (colon >= end)) {
      field += pattern_written;
      continue;
    }

    const char *cursor = colon + 1;
    while ((cursor < end) && isspace((unsigned char)*cursor)) {
      cursor++;
    }
    if ((cursor >= end) || (*cursor != '[')) {
      field += pattern_written;
      continue;
    }

    const char *array_end = find_matching_bracket(cursor, '[', ']');
    if ((array_end == NULL) || (array_end >= end)) {
      field += pattern_written;
      continue;
    }

    const char *item_cursor = cursor + 1;
    while (item_cursor < array_end) {
      while ((item_cursor < array_end) &&
             (isspace((unsigned char)*item_cursor) || (*item_cursor == ','))) {
        item_cursor++;
      }
      if ((item_cursor >= array_end) || (*item_cursor != '\"')) {
        item_cursor++;
        continue;
      }

      item_cursor++;
      char token[GAME_MATCHER_MAX_SERIAL_LIST_LEN];
      size_t token_len = 0U;
      while ((item_cursor < array_end) && (*item_cursor != '\"')) {
        if ((*item_cursor == '\\') && ((item_cursor + 1) < array_end)) {
          item_cursor++;
        }
        if ((token_len + 1U) < sizeof(token)) {
          token[token_len++] = *item_cursor;
        }
        item_cursor++;
      }

      token[token_len] = '\0';
      append_pipe_list_unique(out_value, out_value_size, token);

      if ((item_cursor < array_end) && (*item_cursor == '\"')) {
        item_cursor++;
      }
    }

    if (has_text(out_value)) {
      return 0;
    }

    field = array_end + 1;
  }

  return -1;
}

/*
 * Enriches one ROM catalog entry with serial metadata fields when present.
 */
static void extract_serial_metadata(
    const char *object_start,
    const char *object_end,
    RomCatalogEntry *entry) {
  if ((object_start == NULL) || (object_end == NULL) ||
      (object_start >= object_end) || (entry == NULL)) {
    return;
  }

  const char *const primary_keys[] = {
      "serial",
      "serials",
      "serial_list",
      "serial_number",
      "serialNumber",
      "product_code",
      "productCode",
      "disc_id",
      "discId",
      "game_id",
      "gameId"};
  const char *const list_keys[] = {
      "serials",
      "serial_list",
      "serialNumbers",
      "product_codes",
      "productCodes"};

  for (size_t i = 0U; i < (sizeof(primary_keys) / sizeof(primary_keys[0])); ++i) {
    char value[GAME_MATCHER_MAX_SERIAL_LIST_LEN];
    if (extract_json_string_field_in_range(object_start, object_end, primary_keys[i], value, sizeof(value)) == 0) {
      if (!has_text(entry->serial)) {
        safe_copy(entry->serial, sizeof(entry->serial), value);
      }
      append_pipe_list_unique(entry->serials, sizeof(entry->serials), value);
    }
  }

  for (size_t i = 0U; i < (sizeof(list_keys) / sizeof(list_keys[0])); ++i) {
    char list_values[GAME_MATCHER_MAX_SERIAL_LIST_LEN];
    if (extract_json_string_array_field_in_range(object_start, object_end, list_keys[i], list_values, sizeof(list_values)) == 0) {
      char token[GAME_MATCHER_MAX_SERIAL_LIST_LEN];
      size_t token_len = 0U;
      for (const char *cursor = list_values;; ++cursor) {
        char c = *cursor;
        if ((c != '|') && (c != '\0') && ((token_len + 1U) < sizeof(token))) {
          token[token_len++] = c;
          continue;
        }

        if (token_len > 0U) {
          token[token_len] = '\0';
          if (!has_text(entry->serial)) {
            safe_copy(entry->serial, sizeof(entry->serial), token);
          }
          append_pipe_list_unique(entry->serials, sizeof(entry->serials), token);
          token_len = 0U;
        }

        if (c == '\0') {
          break;
        }
      }
    }
  }
}

/*
 * Initializes Vita networking + HTTP stack for one HTTP transaction.
 */
static int http_runtime_init(const AppConfig *config, const char *url, HttpRuntimeState *state) {
  if ((config == NULL) || (state == NULL) || !has_text(url)) {
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }

  memset(state, 0, sizeof(*state));

  int status = load_user_module(SCE_SYSMODULE_NET, &state->module_net_loaded);
  if (status < 0) {
    app_log_write(APP_LOG_LEVEL_ERROR, "http", "sceSysmoduleLoadModule(NET) failed: 0x%08X", (unsigned int)status);
    return ROMM_CLIENT_ERR_NETWORK;
  }

  state->net_memory = malloc(ROMM_NET_POOL_SIZE);
  if (state->net_memory == NULL) {
    app_log_write(APP_LOG_LEVEL_ERROR, "http", "Out of memory for SceNet pool");
    goto fail;
  }

  SceNetInitParam net_init_param;
  memset(&net_init_param, 0, sizeof(net_init_param));
  net_init_param.memory = state->net_memory;
  net_init_param.size = ROMM_NET_POOL_SIZE;
  net_init_param.flags = 0;

  status = sceNetInit(&net_init_param);
  if (status < 0) {
    app_log_write(APP_LOG_LEVEL_ERROR, "http", "sceNetInit failed: 0x%08X", (unsigned int)status);
    goto fail;
  }
  state->net_inited = 1;

  status = sceNetCtlInit();
  if (status < 0) {
    app_log_write(APP_LOG_LEVEL_ERROR, "http", "sceNetCtlInit failed: 0x%08X", (unsigned int)status);
    goto fail;
  }
  state->netctl_inited = 1;

  status = load_user_module(SCE_SYSMODULE_HTTP, &state->module_http_loaded);
  if (status < 0) {
    app_log_write(APP_LOG_LEVEL_ERROR, "http", "sceSysmoduleLoadModule(HTTP) failed: 0x%08X", (unsigned int)status);
    goto fail;
  }

  status = sceHttpInit(ROMM_HTTP_POOL_SIZE);
  if (status < 0) {
    app_log_write(APP_LOG_LEVEL_ERROR, "http", "sceHttpInit failed: 0x%08X", (unsigned int)status);
    goto fail;
  }
  state->http_inited = 1;

  if (url_is_https(url)) {
    status = load_user_module(SCE_SYSMODULE_SSL, &state->module_ssl_loaded);
    if (status < 0) {
      app_log_write(APP_LOG_LEVEL_ERROR, "http", "sceSysmoduleLoadModule(SSL) failed: 0x%08X", (unsigned int)status);
      goto fail;
    }

    status = load_user_module(SCE_SYSMODULE_HTTPS, &state->module_https_loaded);
    if (status < 0) {
      app_log_write(APP_LOG_LEVEL_ERROR, "http", "sceSysmoduleLoadModule(HTTPS) failed: 0x%08X", (unsigned int)status);
      goto fail;
    }

    status = sceSslInit(ROMM_SSL_POOL_SIZE);
    if (status < 0) {
      app_log_write(APP_LOG_LEVEL_ERROR, "http", "sceSslInit failed: 0x%08X", (unsigned int)status);
      goto fail;
    }
    state->ssl_inited = 1;

    if (!config->romm_verify_tls) {
      int disable_status = sceHttpsDisableOption(ROMM_HTTPS_VERIFY_OVERRIDE_FLAG);
      if (disable_status >= 0) {
        state->tls_verify_disabled = 1;
        app_log_write(APP_LOG_LEVEL_WARN, "http", "TLS verification disabled for this request");
      } else {
        app_log_write(APP_LOG_LEVEL_WARN, "http", "Failed to disable TLS verification: 0x%08X", (unsigned int)disable_status);
      }
    }
  }

  return ROMM_CLIENT_OK;

fail:
  http_runtime_term(state);
  return ROMM_CLIENT_ERR_NETWORK;
}

/*
 * Tears down the stack initialized by http_runtime_init().
 */
static void http_runtime_term(HttpRuntimeState *state) {
  if (state == NULL) {
    return;
  }

  if (state->tls_verify_disabled) {
    int status = sceHttpsEnableOption(ROMM_HTTPS_VERIFY_OVERRIDE_FLAG);
    if (status < 0) {
      app_log_write(APP_LOG_LEVEL_WARN, "http", "sceHttpsEnableOption failed: 0x%08X", (unsigned int)status);
    }
  }

  if (state->ssl_inited) {
    int status = sceSslTerm();
    if (status < 0) {
      app_log_write(APP_LOG_LEVEL_WARN, "http", "sceSslTerm failed: 0x%08X", (unsigned int)status);
    }
  }

  if (state->http_inited) {
    int status = sceHttpTerm();
    if (status < 0) {
      app_log_write(APP_LOG_LEVEL_WARN, "http", "sceHttpTerm failed: 0x%08X", (unsigned int)status);
    }
  }

  if (state->netctl_inited) {
    /*
     * VitaSDK headers may declare sceNetCtlTerm() as void on some versions.
     * Keep teardown best-effort and avoid assuming an int return status.
     */
    (void)sceNetCtlTerm();
  }

  if (state->net_inited) {
    int status = sceNetTerm();
    if (status < 0) {
      app_log_write(APP_LOG_LEVEL_WARN, "http", "sceNetTerm failed: 0x%08X", (unsigned int)status);
    }
  }

  if (state->net_memory != NULL) {
    free(state->net_memory);
    state->net_memory = NULL;
  }

  unload_user_module_if_loaded(SCE_SYSMODULE_HTTPS, state->module_https_loaded);
  unload_user_module_if_loaded(SCE_SYSMODULE_SSL, state->module_ssl_loaded);
  unload_user_module_if_loaded(SCE_SYSMODULE_HTTP, state->module_http_loaded);
  unload_user_module_if_loaded(SCE_SYSMODULE_NET, state->module_net_loaded);
}

/*
 * Sends an authenticated HTTP request.
 * If response_file_path is non-null and status is 200, body is streamed to file.
 */
static int http_send_request(
    const AppConfig *config,
    int method,
    const char *url,
    const char *content_type,
    const void *payload,
    size_t payload_size,
    int *out_status_code,
    char *out_body,
    size_t out_body_size,
    const char *response_file_path) {
  if ((config == NULL) || !has_text(url) || (out_status_code == NULL)) {
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }
  if ((payload == NULL) && (payload_size > 0U)) {
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }
  if ((response_file_path == NULL) && (out_body == NULL)) {
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }
  if ((response_file_path == NULL) && (out_body_size == 0U)) {
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }
  if (payload_size > UINT_MAX) {
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }

  *out_status_code = 0;
  if ((out_body != NULL) && (out_body_size > 0U)) {
    out_body[0] = '\0';
  }

  HttpRuntimeState runtime_state;
  int runtime_status = http_runtime_init(config, url, &runtime_state);
  if (runtime_status < 0) {
    return runtime_status;
  }

  int template_id = -1;
  int connection_id = -1;
  int request_id = -1;
  int result = ROMM_CLIENT_ERR_NETWORK;
  int retry_over_http = 0;
  int transport_error = 0;
  char fallback_url[ROMM_HTTP_FALLBACK_URL_BUFFER_SIZE];
  fallback_url[0] = '\0';

  unsigned int timeout_seconds = (config->romm_timeout_seconds > 0)
                                     ? (unsigned int)config->romm_timeout_seconds
                                     : 30U;
  unsigned long long timeout_usec_ull = ((unsigned long long)timeout_seconds) * 1000ULL * 1000ULL;
  if (timeout_usec_ull > UINT_MAX) {
    timeout_usec_ull = UINT_MAX;
  }
  unsigned int timeout_usec_u32 = (unsigned int)timeout_usec_ull;

  char user_agent[APP_CONFIG_MAX_CLIENT_NAME_LEN + APP_CONFIG_MAX_CLIENT_VERSION_LEN + 2];
  int user_agent_written = snprintf(
      user_agent,
      sizeof(user_agent),
      "%s/%s",
      has_text(config->device_client) ? config->device_client : "romm-vita-sync",
      has_text(config->device_client_version) ? config->device_client_version : "0.1");
  if ((user_agent_written <= 0) || ((size_t)user_agent_written >= sizeof(user_agent))) {
    user_agent[0] = '\0';
  }

  template_id = sceHttpCreateTemplate(
      has_text(user_agent) ? user_agent : "romm-vita-sync/0.1",
      SCE_HTTP_VERSION_1_1,
      SCE_HTTP_PROXY_AUTO);
  if (template_id < 0) {
    app_log_write(APP_LOG_LEVEL_ERROR, "http", "sceHttpCreateTemplate failed: 0x%08X", (unsigned int)template_id);
    goto cleanup;
  }

  sceHttpSetConnectTimeOut(template_id, timeout_usec_u32);
  sceHttpSetSendTimeOut(template_id, timeout_usec_u32);
  sceHttpSetRecvTimeOut(template_id, timeout_usec_u32);

  connection_id = sceHttpCreateConnectionWithURL(template_id, url, 0);
  if (connection_id < 0) {
    transport_error = connection_id;
    if (!config->romm_verify_tls &&
        ((unsigned int)transport_error == ROMM_HTTP_SSL_TRANSPORT_ERROR) &&
        (build_http_fallback_url(url, fallback_url, sizeof(fallback_url)) == 0)) {
      retry_over_http = 1;
    }
    app_log_write(APP_LOG_LEVEL_ERROR, "http", "sceHttpCreateConnectionWithURL failed: 0x%08X", (unsigned int)connection_id);
    goto cleanup;
  }

  request_id = sceHttpCreateRequestWithURL(connection_id, method, url, (unsigned long long)payload_size);
  if (request_id < 0) {
    transport_error = request_id;
    if (!config->romm_verify_tls &&
        ((unsigned int)transport_error == ROMM_HTTP_SSL_TRANSPORT_ERROR) &&
        (build_http_fallback_url(url, fallback_url, sizeof(fallback_url)) == 0)) {
      retry_over_http = 1;
    }
    app_log_write(APP_LOG_LEVEL_ERROR, "http", "sceHttpCreateRequestWithURL failed: 0x%08X", (unsigned int)request_id);
    goto cleanup;
  }

  if (has_text(content_type)) {
    sceHttpAddRequestHeader(request_id, "Content-Type", content_type, SCE_HTTP_HEADER_OVERWRITE);
  }
  sceHttpAddRequestHeader(request_id, "Accept", "application/json", SCE_HTTP_HEADER_OVERWRITE);

  char auth_value[640];
  if (build_auth_value(config, auth_value, sizeof(auth_value)) == 0) {
    sceHttpAddRequestHeader(request_id, "Authorization", auth_value, SCE_HTTP_HEADER_OVERWRITE);
  } else {
    result = ROMM_CLIENT_ERR_AUTH;
    goto cleanup;
  }

  int http_status = sceHttpSendRequest(request_id, payload, (unsigned int)payload_size);
  if (http_status < 0) {
    transport_error = http_status;
    if (!config->romm_verify_tls &&
        ((unsigned int)transport_error == ROMM_HTTP_SSL_TRANSPORT_ERROR) &&
        (build_http_fallback_url(url, fallback_url, sizeof(fallback_url)) == 0)) {
      retry_over_http = 1;
    }
    app_log_write(APP_LOG_LEVEL_ERROR, "http", "sceHttpSendRequest failed: 0x%08X", (unsigned int)http_status);
    goto cleanup;
  }

  http_status = sceHttpGetStatusCode(request_id, out_status_code);
  if (http_status < 0) {
    app_log_write(APP_LOG_LEVEL_ERROR, "http", "sceHttpGetStatusCode failed: 0x%08X", (unsigned int)http_status);
    goto cleanup;
  }

  if (response_file_path != NULL) {
    if (*out_status_code != 200) {
      if ((out_body != NULL) && (out_body_size > 0U)) {
        if (read_response_body(request_id, out_body, out_body_size) < 0) {
          out_body[0] = '\0';
        }
      }
      result = ROMM_CLIENT_OK;
      goto cleanup;
    }

    FILE *destination = fopen(response_file_path, "wb");
    if (destination == NULL) {
      app_log_write(APP_LOG_LEVEL_ERROR, "http", "Cannot open destination file: %s", response_file_path);
      result = ROMM_CLIENT_ERR_NETWORK;
      goto cleanup;
    }

    unsigned char chunk[8192];
    int stream_error = 0;
    for (;;) {
      int read = sceHttpReadData(request_id, chunk, (unsigned int)sizeof(chunk));
      if (read < 0) {
        stream_error = 1;
        break;
      }
      if (read == 0) {
        break;
      }
      if (fwrite(chunk, 1U, (size_t)read, destination) != (size_t)read) {
        stream_error = 1;
        break;
      }
    }

    fclose(destination);

    if (stream_error) {
      remove(response_file_path);
      result = ROMM_CLIENT_ERR_NETWORK;
      goto cleanup;
    }
  } else {
    if (read_response_body(request_id, out_body, out_body_size) < 0) {
      app_log_write(APP_LOG_LEVEL_WARN, "http", "Failed to read full response body");
    }
  }

  result = ROMM_CLIENT_OK;

cleanup:
  if (request_id >= 0) {
    sceHttpDeleteRequest(request_id);
  }
  if (connection_id >= 0) {
    sceHttpDeleteConnection(connection_id);
  }
  if (template_id >= 0) {
    sceHttpDeleteTemplate(template_id);
  }

  http_runtime_term(&runtime_state);

  if (retry_over_http) {
    app_log_write(
        APP_LOG_LEVEL_WARN,
        "http",
        "Retrying request over HTTP after HTTPS SSL transport failure (verify_tls=false): %s",
        fallback_url);
    return http_send_request(
        config,
        method,
        fallback_url,
        content_type,
        payload,
        payload_size,
        out_status_code,
        out_body,
        out_body_size,
        response_file_path);
  }

  return result;
}

/*
 * Sends a POST JSON request and returns HTTP status + response payload.
 */
static int http_post_json(
    const AppConfig *config,
    const char *url,
    const char *payload,
    int *out_status_code,
    char *out_body,
    size_t out_body_size) {
  if ((config == NULL) || !has_text(url) || (payload == NULL) ||
      (out_status_code == NULL) || (out_body == NULL) || (out_body_size == 0U)) {
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }

  return http_send_request(
      config,
      SCE_HTTP_METHOD_POST,
      url,
      "application/json",
      payload,
      strlen(payload),
      out_status_code,
      out_body,
      out_body_size,
      NULL);
}

/*
 * Sends a GET request and captures text response.
 */
static int http_get_text(
    const AppConfig *config,
    const char *url,
    int *out_status_code,
    char *out_body,
    size_t out_body_size) {
  return http_send_request(
      config,
      SCE_HTTP_METHOD_GET,
      url,
      NULL,
      NULL,
      0U,
      out_status_code,
      out_body,
      out_body_size,
      NULL);
}

/*
 * Sends a GET request and streams body into file.
 */
static int http_get_file(
    const AppConfig *config,
    const char *url,
    int *out_status_code,
    char *out_body,
    size_t out_body_size,
    const char *destination_path) {
  return http_send_request(
      config,
      SCE_HTTP_METHOD_GET,
      url,
      NULL,
      NULL,
      0U,
      out_status_code,
      out_body,
      out_body_size,
      destination_path);
}

/*
 * Sends a multipart/form-data upload request for one file field.
 */
static int http_post_multipart_file(
    const AppConfig *config,
    const char *url,
    const char *field_name,
    const char *upload_filename,
    const char *source_path,
    int *out_status_code,
    char *out_body,
    size_t out_body_size) {
  if ((config == NULL) || !has_text(url) || !has_text(field_name) || !has_text(upload_filename) ||
      !has_text(source_path) || (out_status_code == NULL) || (out_body == NULL) || (out_body_size == 0U)) {
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }

  unsigned char *file_data = NULL;
  size_t file_size = 0U;
  if (read_file_to_memory(source_path, &file_data, &file_size) < 0) {
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }

  const char *boundary = "----romm-vita-sync-boundary";
  char content_type[128];
  snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);

  char preamble[512];
  int preamble_written = snprintf(
      preamble,
      sizeof(preamble),
      "--%s\r\n"
      "Content-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n"
      "Content-Type: application/octet-stream\r\n\r\n",
      boundary,
      field_name,
      upload_filename);
  if ((preamble_written <= 0) || ((size_t)preamble_written >= sizeof(preamble))) {
    free(file_data);
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }

  char trailer[64];
  int trailer_written = snprintf(trailer, sizeof(trailer), "\r\n--%s--\r\n", boundary);
  if ((trailer_written <= 0) || ((size_t)trailer_written >= sizeof(trailer))) {
    free(file_data);
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }

  size_t payload_size = (size_t)preamble_written + file_size + (size_t)trailer_written;
  unsigned char *payload = (unsigned char *)malloc(payload_size);
  if (payload == NULL) {
    free(file_data);
    return ROMM_CLIENT_ERR_NETWORK;
  }

  memcpy(payload, preamble, (size_t)preamble_written);
  if (file_size > 0U) {
    memcpy(payload + (size_t)preamble_written, file_data, file_size);
  }
  memcpy(payload + (size_t)preamble_written + file_size, trailer, (size_t)trailer_written);

  free(file_data);
  int status = http_send_request(
      config,
      SCE_HTTP_METHOD_POST,
      url,
      content_type,
      payload,
      payload_size,
      out_status_code,
      out_body,
      out_body_size,
      NULL);
  free(payload);
  return status;
}

/*
 * Parses one /api/roms page into simple catalog entries.
 */
static int parse_rom_catalog_entries(
    const char *json,
    RomCatalogEntry *out_entries,
    int max_entries,
    int *out_total) {
  if ((json == NULL) || (out_entries == NULL) || (max_entries <= 0) || (out_total == NULL)) {
    return -1;
  }

  *out_total = -1;
  char total_text[32];
  if (extract_json_scalar_field(json, "total", total_text, sizeof(total_text)) == 0) {
    parse_int_value(total_text, out_total);
  } else if (extract_json_scalar_field(json, "count", total_text, sizeof(total_text)) == 0) {
    parse_int_value(total_text, out_total);
  }

  const char *array_start = NULL;
  const char *array_end = NULL;
  if (find_object_array_range(json, "items", &array_start, &array_end) < 0) {
    if (find_object_array_range(json, "data", &array_start, &array_end) < 0) {
      if (find_object_array_range(json, "results", &array_start, &array_end) < 0) {
        if (find_object_array_range(json, "roms", &array_start, &array_end) < 0) {
          if (find_object_array_range(json, NULL, &array_start, &array_end) < 0) {
            return 0;
          }
        }
      }
    }
  }

  int count = 0;
  const char *cursor = array_start + 1;
  while (count < max_entries) {
    const char *object_start = NULL;
    const char *object_end = NULL;
    const char *next_cursor = NULL;
    if (next_object_in_array(cursor, array_end, &object_start, &object_end, &next_cursor) < 0) {
      break;
    }

    char id_text[32];
    int rom_id = -1;
    int id_found = 0;
    if ((extract_json_scalar_field_in_range(object_start, object_end, "id", id_text, sizeof(id_text)) == 0) &&
        (parse_int_value(id_text, &rom_id) == 0)) {
      id_found = 1;
    } else if ((extract_json_string_field_in_range(object_start, object_end, "id", id_text, sizeof(id_text)) == 0) &&
               (parse_int_value(id_text, &rom_id) == 0)) {
      id_found = 1;
    }

    if (id_found &&
        (rom_id > 0)) {
      RomCatalogEntry *entry = &out_entries[count];
      memset(entry, 0, sizeof(*entry));
      entry->rom_id = rom_id;
      extract_json_string_field_in_range(object_start, object_end, "name", entry->name, sizeof(entry->name));
      extract_json_string_field_in_range(object_start, object_end, "fs_name", entry->fs_name, sizeof(entry->fs_name));
      extract_json_string_field_in_range(object_start, object_end, "fs_name_no_ext", entry->fs_name_no_ext, sizeof(entry->fs_name_no_ext));
      extract_json_string_field_in_range(object_start, object_end, "platform_slug", entry->platform_slug, sizeof(entry->platform_slug));
      extract_serial_metadata(object_start, object_end, entry);
      count++;
    }

    cursor = next_cursor;
  }

  return count;
}

/*
 * Parses one /api/saves page into descriptors.
 */
static int parse_remote_save_entries(
    const char *json,
    SyncSaveDescriptor *out_items,
    int max_items,
    int *out_total) {
  if ((json == NULL) || (out_items == NULL) || (max_items <= 0) || (out_total == NULL)) {
    return -1;
  }

  *out_total = -1;
  char total_text[32];
  if (extract_json_scalar_field(json, "total", total_text, sizeof(total_text)) == 0) {
    parse_int_value(total_text, out_total);
  }

  const char *array_start = NULL;
  const char *array_end = NULL;
  if (find_object_array_range(json, "items", &array_start, &array_end) < 0) {
    if (find_object_array_range(json, NULL, &array_start, &array_end) < 0) {
      return 0;
    }
  }

  int count = 0;
  const char *cursor = array_start + 1;
  while (count < max_items) {
    const char *object_start = NULL;
    const char *object_end = NULL;
    const char *next_cursor = NULL;
    if (next_object_in_array(cursor, array_end, &object_start, &object_end, &next_cursor) < 0) {
      break;
    }

    SyncSaveDescriptor *item = &out_items[count];
    sync_save_descriptor_init(item);

    char scalar[64];
    if ((extract_json_scalar_field_in_range(object_start, object_end, "id", scalar, sizeof(scalar)) == 0) &&
        (parse_int_value(scalar, &item->remote_id) < 0)) {
      item->remote_id = -1;
    }
    if ((extract_json_scalar_field_in_range(object_start, object_end, "rom_id", scalar, sizeof(scalar)) == 0) &&
        (parse_int_value(scalar, &item->rom_id) < 0)) {
      item->rom_id = -1;
    }
    if ((extract_json_scalar_field_in_range(object_start, object_end, "file_size_bytes", scalar, sizeof(scalar)) == 0) ||
        (extract_json_scalar_field_in_range(object_start, object_end, "size_bytes", scalar, sizeof(scalar)) == 0)) {
      uint64_t parsed_size = 0;
      if (parse_uint64_value(scalar, &parsed_size) == 0) {
        item->size_bytes = parsed_size;
      }
    }

    extract_json_string_field_in_range(object_start, object_end, "file_name", item->filename, sizeof(item->filename));
    extract_json_string_field_in_range(object_start, object_end, "download_path", item->remote_path, sizeof(item->remote_path));
    extract_json_string_field_in_range(object_start, object_end, "hash", item->hash, sizeof(item->hash));
    extract_json_string_field_in_range(object_start, object_end, "device_id", item->origin_device, sizeof(item->origin_device));
    if (!has_text(item->origin_device)) {
      extract_json_string_field_in_range(object_start, object_end, "origin_device", item->origin_device, sizeof(item->origin_device));
    }

    char updated_at[64];
    if (extract_json_string_field_in_range(object_start, object_end, "updated_at", updated_at, sizeof(updated_at)) == 0) {
      if (parse_iso_timestamp(updated_at, &item->timestamp_unix) < 0) {
        item->timestamp_unix = 0;
      }
    }

    char slot_text[32];
    if (extract_json_string_field_in_range(object_start, object_end, "slot", slot_text, sizeof(slot_text)) == 0) {
      item->slot = parse_slot_value(slot_text, item->filename);
    } else {
      item->slot = parse_slot_value(NULL, item->filename);
    }

    char is_current_text[16];
    if (extract_json_scalar_field_in_range(object_start, object_end, "device_is_current", is_current_text, sizeof(is_current_text)) == 0) {
      item->device_is_current = sync_string_ieq(is_current_text, "true") || sync_string_ieq(is_current_text, "1");
    } else {
      item->device_is_current = 0;
    }

    count++;
    cursor = next_cursor;
  }

  return count;
}

/*
 * Returns non-zero if candidate is newer than current saved revision.
 */
static int remote_candidate_is_newer(
    const SyncSaveDescriptor *current_item,
    const SyncSaveDescriptor *candidate_item) {
  if ((current_item == NULL) || (candidate_item == NULL)) {
    return 0;
  }

  if ((candidate_item->timestamp_unix > 0) && (current_item->timestamp_unix > 0)) {
    if (candidate_item->timestamp_unix != current_item->timestamp_unix) {
      return candidate_item->timestamp_unix > current_item->timestamp_unix;
    }
  } else if (candidate_item->timestamp_unix > current_item->timestamp_unix) {
    return 1;
  }

  return candidate_item->remote_id > current_item->remote_id;
}

/*
 * Searches existing remote entry with same identity key.
 */
static int find_existing_remote_index(
    const SyncSaveDescriptor *items,
    int count,
    const SyncSaveDescriptor *candidate) {
  if ((items == NULL) || (candidate == NULL) || (count <= 0)) {
    return -1;
  }

  for (int i = 0; i < count; ++i) {
    const SyncSaveDescriptor *entry = &items[i];
    if ((entry->rom_id > 0) && (candidate->rom_id > 0) &&
        (entry->rom_id == candidate->rom_id) &&
        (entry->slot == candidate->slot)) {
      return i;
    }
  }

  for (int i = 0; i < count; ++i) {
    const SyncSaveDescriptor *entry = &items[i];
    if (has_text(entry->filename) && has_text(candidate->filename) &&
        sync_string_ieq(entry->filename, candidate->filename) &&
        (entry->slot == candidate->slot)) {
      return i;
    }
  }

  return -1;
}

/*
 * Fetches RomM catalog and resolves best rom_id for each local item.
 */
int romm_http_resolve_rom_ids(
    const void *context,
    SyncSaveDescriptor *items,
    int item_count) {
  if ((context == NULL) || (items == NULL) || (item_count < 0)) {
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }

  if (item_count == 0) {
    return 0;
  }

  const AppConfig *config = (const AppConfig *)context;
  if (!app_config_has_server_url(config) || !app_config_has_auth(config)) {
    return ROMM_CLIENT_ERR_AUTH;
  }

  RomCatalogEntry *catalog = (RomCatalogEntry *)malloc(sizeof(RomCatalogEntry) * ROMM_HTTP_MAX_ROMS);
  if (catalog == NULL) {
    return ROMM_CLIENT_ERR_NETWORK;
  }
  char *body = (char *)malloc(ROMM_HTTP_MAX_BODY_SIZE);
  if (body == NULL) {
    free(catalog);
    return ROMM_CLIENT_ERR_NETWORK;
  }

  int catalog_count = 0;
  int total = INT_MAX;
  for (int offset = 0; (offset < total) && (catalog_count < ROMM_HTTP_MAX_ROMS); offset += ROMM_HTTP_PAGE_LIMIT) {
    char path[320];
    snprintf(
        path,
        sizeof(path),
        "/api/roms?limit=%d&offset=%d&fields=id,name,fs_name,fs_name_no_ext,platform_slug,serial,serials,serial_number,product_code,disc_id,game_id",
        ROMM_HTTP_PAGE_LIMIT,
        offset);

    char url[APP_CONFIG_MAX_URL_LEN + sizeof(path)];
    if (build_api_url(config->romm_url, path, url, sizeof(url)) < 0) {
      free(body);
      free(catalog);
      return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
    }

    int status_code = 0;
    int transport_status = http_get_text(config, url, &status_code, body, ROMM_HTTP_MAX_BODY_SIZE);
    if (transport_status < 0) {
      free(body);
      free(catalog);
      return transport_status;
    }
    if ((status_code == 401) || (status_code == 403)) {
      free(body);
      free(catalog);
      return ROMM_CLIENT_ERR_AUTH;
    }
    if (status_code != 200) {
      app_log_write(APP_LOG_LEVEL_WARN, "http", "rom catalog fetch failed status=%d offset=%d", status_code, offset);
      free(body);
      free(catalog);
      return ROMM_CLIENT_ERR_NETWORK;
    }

    int page_total = -1;
    int remaining = ROMM_HTTP_MAX_ROMS - catalog_count;
    int parsed = parse_rom_catalog_entries(body, &catalog[catalog_count], remaining, &page_total);
    if (parsed < 0) {
      free(body);
      free(catalog);
      return ROMM_CLIENT_ERR_NETWORK;
    }

    if (page_total > 0) {
      total = page_total;
    }
    if (parsed == 0) {
      break;
    }

    app_log_write(
        APP_LOG_LEVEL_DEBUG,
        "http",
        "rom catalog page parsed offset=%d parsed=%d total_hint=%d",
        offset,
        parsed,
        page_total);

    catalog_count += parsed;
  }

  app_log_write(
      APP_LOG_LEVEL_INFO,
      "http",
      "rom catalog loaded entries=%d",
      catalog_count);
  if (catalog_count <= 0) {
    app_log_write(
        APP_LOG_LEVEL_WARN,
        "http",
        "rom catalog is empty or unsupported response format");
  }

  int resolved_count = 0;
  for (int i = 0; i < item_count; ++i) {
    SyncSaveDescriptor *local_item = &items[i];
    if (local_item->rom_id > 0) {
      resolved_count++;
      continue;
    }

    int resolved_rom_id = game_matcher_resolve_rom_id(local_item, catalog, catalog_count);
    if (resolved_rom_id > 0) {
      local_item->rom_id = resolved_rom_id;
      resolved_count++;
      app_log_write(
          APP_LOG_LEVEL_DEBUG,
          "http",
          "rom_id mapped game=%s title=%s -> rom_id=%d",
          local_item->game_id,
          local_item->title,
          resolved_rom_id);
    } else {
      if (resolved_rom_id == GAME_MATCHER_AMBIGUOUS) {
        app_log_write(
            APP_LOG_LEVEL_WARN,
            "http",
            "rom_id ambiguous game=%s title=%s",
            local_item->game_id,
            local_item->title);
      } else {
        app_log_write(
            APP_LOG_LEVEL_WARN,
            "http",
            "rom_id unresolved game=%s title=%s",
            local_item->game_id,
            local_item->title);
      }
    }
  }

  free(body);
  free(catalog);
  return resolved_count;
}

/*
 * Real RomM save-list callback backed by GET /api/saves.
 */
int romm_http_list_remote_saves_callback(
    void *context,
    SyncSaveDescriptor *out_items,
    int max_items) {
  if ((context == NULL) || (out_items == NULL) || (max_items <= 0)) {
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }

  const AppConfig *config = (const AppConfig *)context;
  if (!app_config_has_server_url(config) || !app_config_has_auth(config)) {
    return ROMM_CLIENT_ERR_AUTH;
  }

  for (int i = 0; i < max_items; ++i) {
    sync_save_descriptor_init(&out_items[i]);
  }

  char *body = (char *)malloc(ROMM_HTTP_MAX_BODY_SIZE);
  SyncSaveDescriptor *page_items = (SyncSaveDescriptor *)malloc(sizeof(SyncSaveDescriptor) * ROMM_HTTP_PAGE_LIMIT);
  if ((body == NULL) || (page_items == NULL)) {
    free(body);
    free(page_items);
    return ROMM_CLIENT_ERR_NETWORK;
  }

  int count = 0;
  int total = INT_MAX;
  for (int offset = 0; (offset < total); offset += ROMM_HTTP_PAGE_LIMIT) {
    char path[192];
    snprintf(
        path,
        sizeof(path),
        "/api/saves?limit=%d&offset=%d&emulator=%s",
        ROMM_HTTP_PAGE_LIMIT,
        offset,
        ROMM_HTTP_SAVE_EMULATOR);

    char url[APP_CONFIG_MAX_URL_LEN + sizeof(path)];
    if (build_api_url(config->romm_url, path, url, sizeof(url)) < 0) {
      free(body);
      free(page_items);
      return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
    }

    int status_code = 0;
    int transport_status = http_get_text(config, url, &status_code, body, ROMM_HTTP_MAX_BODY_SIZE);
    if (transport_status < 0) {
      free(body);
      free(page_items);
      return transport_status;
    }
    if ((status_code == 401) || (status_code == 403)) {
      free(body);
      free(page_items);
      return ROMM_CLIENT_ERR_AUTH;
    }
    if (status_code != 200) {
      app_log_write(APP_LOG_LEVEL_WARN, "http", "remote saves fetch failed status=%d offset=%d", status_code, offset);
      free(body);
      free(page_items);
      return ROMM_CLIENT_ERR_NETWORK;
    }

    int page_total = -1;
    int page_count = parse_remote_save_entries(body, page_items, ROMM_HTTP_PAGE_LIMIT, &page_total);
    if (page_count < 0) {
      free(body);
      free(page_items);
      return ROMM_CLIENT_ERR_NETWORK;
    }
    if (page_total > 0) {
      total = page_total;
    }
    if (page_count == 0) {
      break;
    }

    for (int i = 0; i < page_count; ++i) {
      SyncSaveDescriptor *candidate = &page_items[i];
      if (candidate->remote_id <= 0) {
        continue;
      }

      int existing_index = find_existing_remote_index(out_items, count, candidate);
      if (existing_index >= 0) {
        if (remote_candidate_is_newer(&out_items[existing_index], candidate)) {
          memcpy(&out_items[existing_index], candidate, sizeof(*candidate));
        }
        continue;
      }

      if (count < max_items) {
        memcpy(&out_items[count], candidate, sizeof(*candidate));
        count++;
      }
    }

    if ((count >= max_items) || (page_count < ROMM_HTTP_PAGE_LIMIT)) {
      break;
    }
  }

  free(body);
  free(page_items);
  app_log_write(APP_LOG_LEVEL_INFO, "http", "remote saves listed=%d", count);
  return count;
}

/*
 * Real RomM upload callback backed by POST /api/saves.
 */
int romm_http_upload_save_callback(
    void *context,
    const SyncSaveDescriptor *local_item) {
  if ((context == NULL) || (local_item == NULL) || !has_text(local_item->path)) {
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }

  const AppConfig *config = (const AppConfig *)context;
  if (!app_config_has_server_url(config) || !app_config_has_auth(config)) {
    return ROMM_CLIENT_ERR_AUTH;
  }

  int rom_id = local_item->rom_id;
  if (rom_id <= 0) {
    SyncSaveDescriptor single;
    memcpy(&single, local_item, sizeof(single));
    int resolved = romm_http_resolve_rom_ids(config, &single, 1);
    if (resolved > 0) {
      rom_id = single.rom_id;
    }
  }
  if (rom_id <= 0) {
    app_log_write(APP_LOG_LEVEL_WARN, "http", "upload skipped: rom_id unresolved game=%s", local_item->game_id);
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }

  const char *slot_query = slot_to_query_value(local_item->slot);
  char path[320];
  if (has_text(config->device_id) && has_text(slot_query)) {
    snprintf(
        path,
        sizeof(path),
        "/api/saves?rom_id=%d&emulator=%s&device_id=%s&slot=%s",
        rom_id,
        ROMM_HTTP_SAVE_EMULATOR,
        config->device_id,
        slot_query);
  } else if (has_text(config->device_id)) {
    snprintf(
        path,
        sizeof(path),
        "/api/saves?rom_id=%d&emulator=%s&device_id=%s",
        rom_id,
        ROMM_HTTP_SAVE_EMULATOR,
        config->device_id);
  } else if (has_text(slot_query)) {
    snprintf(
        path,
        sizeof(path),
        "/api/saves?rom_id=%d&emulator=%s&slot=%s",
        rom_id,
        ROMM_HTTP_SAVE_EMULATOR,
        slot_query);
  } else {
    snprintf(
        path,
        sizeof(path),
        "/api/saves?rom_id=%d&emulator=%s",
        rom_id,
        ROMM_HTTP_SAVE_EMULATOR);
  }

  char url[APP_CONFIG_MAX_URL_LEN + sizeof(path)];
  if (build_api_url(config->romm_url, path, url, sizeof(url)) < 0) {
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }

  char upload_filename[ROMM_HTTP_MAX_FILENAME];
  build_upload_filename(local_item, upload_filename, sizeof(upload_filename));

  char response_body[ROMM_HTTP_SMALL_BODY_SIZE];
  int status_code = 0;
  int transport_status = http_post_multipart_file(
      config,
      url,
      "saveFile",
      upload_filename,
      local_item->path,
      &status_code,
      response_body,
      sizeof(response_body));
  if (transport_status < 0) {
    return transport_status;
  }

  app_log_write(
      APP_LOG_LEVEL_DEBUG,
      "http",
      "upload save status=%d game=%s rom_id=%d body=%s",
      status_code,
      local_item->game_id,
      rom_id,
      response_body);

  if ((status_code == 401) || (status_code == 403)) {
    return ROMM_CLIENT_ERR_AUTH;
  }
  if (status_code == 409) {
    return ROMM_CLIENT_ERR_CONFLICT;
  }
  if ((status_code == 200) || (status_code == 201) || (status_code == 204)) {
    return ROMM_CLIENT_OK;
  }
  return ROMM_CLIENT_ERR_NETWORK;
}

/*
 * Real RomM download callback backed by GET /api/saves/{id}/content.
 */
int romm_http_download_save_callback(
    void *context,
    const SyncSaveDescriptor *remote_item,
    const char *destination_path) {
  if ((context == NULL) || (remote_item == NULL) || !has_text(destination_path) || (remote_item->remote_id <= 0)) {
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }

  const AppConfig *config = (const AppConfig *)context;
  if (!app_config_has_server_url(config) || !app_config_has_auth(config)) {
    return ROMM_CLIENT_ERR_AUTH;
  }

  char path[192];
  int has_device_id = has_text(config->device_id);
  if (has_device_id) {
    snprintf(path, sizeof(path), "/api/saves/%d/content?device_id=%s&optimistic=true", remote_item->remote_id, config->device_id);
  } else {
    snprintf(path, sizeof(path), "/api/saves/%d/content", remote_item->remote_id);
  }

  char url[APP_CONFIG_MAX_URL_LEN + sizeof(path)];
  if (build_api_url(config->romm_url, path, url, sizeof(url)) < 0) {
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }

  char response_body[ROMM_HTTP_SMALL_BODY_SIZE];
  int status_code = 0;
  int transport_status = http_get_file(
      config,
      url,
      &status_code,
      response_body,
      sizeof(response_body),
      destination_path);
  if (transport_status < 0) {
    return transport_status;
  }

  if ((status_code == 404) && has_device_id) {
    snprintf(path, sizeof(path), "/api/saves/%d/content", remote_item->remote_id);
    if (build_api_url(config->romm_url, path, url, sizeof(url)) < 0) {
      return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
    }

    transport_status = http_get_file(
        config,
        url,
        &status_code,
        response_body,
        sizeof(response_body),
        destination_path);
    if (transport_status < 0) {
      return transport_status;
    }
  }

  app_log_write(
      APP_LOG_LEVEL_DEBUG,
      "http",
      "download save status=%d remote_id=%d body=%s",
      status_code,
      remote_item->remote_id,
      response_body);

  if ((status_code == 401) || (status_code == 403)) {
    return ROMM_CLIENT_ERR_AUTH;
  }
  if (status_code == 409) {
    return ROMM_CLIENT_ERR_CONFLICT;
  }
  if (status_code == 200) {
    return ROMM_CLIENT_OK;
  }

  remove(destination_path);
  return ROMM_CLIENT_ERR_NETWORK;
}

/*
 * Real RomM register-device callback backed by POST /api/devices.
 */
int romm_http_register_device_callback(
    void *context,
    const char *device_name,
    const char *device_platform,
    const char *client_name,
    const char *client_version,
    char *out_device_id,
    size_t out_device_id_size) {
  if ((context == NULL) || !has_text(device_name) || !has_text(device_platform) ||
      !has_text(client_name) || !has_text(client_version) ||
      (out_device_id == NULL) || (out_device_id_size == 0U)) {
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }

  const AppConfig *config = (const AppConfig *)context;
  if (!app_config_has_server_url(config) || !app_config_has_auth(config)) {
    return ROMM_CLIENT_ERR_AUTH;
  }

  out_device_id[0] = '\0';

  char url[APP_CONFIG_MAX_URL_LEN + 32];
  if (build_api_url(config->romm_url, "/api/devices", url, sizeof(url)) < 0) {
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }

  char escaped_name[APP_CONFIG_MAX_DEVICE_NAME_LEN * 2];
  char escaped_platform[APP_CONFIG_MAX_DEVICE_PLATFORM_LEN * 2];
  char escaped_client[APP_CONFIG_MAX_CLIENT_NAME_LEN * 2];
  char escaped_client_version[APP_CONFIG_MAX_CLIENT_VERSION_LEN * 2];
  char escaped_hostname[APP_CONFIG_MAX_DEVICE_NAME_LEN * 2];
  const char *hostname = has_text(config->device_name) ? config->device_name : device_name;
  if ((json_escape(device_name, escaped_name, sizeof(escaped_name)) < 0) ||
      (json_escape(device_platform, escaped_platform, sizeof(escaped_platform)) < 0) ||
      (json_escape(client_name, escaped_client, sizeof(escaped_client)) < 0) ||
      (json_escape(client_version, escaped_client_version, sizeof(escaped_client_version)) < 0) ||
      (json_escape(hostname, escaped_hostname, sizeof(escaped_hostname)) < 0)) {
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }

  char payload[1024];
  int payload_written = snprintf(
      payload,
      sizeof(payload),
      "{\"name\":\"%s\",\"platform\":\"%s\",\"client\":\"%s\",\"client_version\":\"%s\","
      "\"hostname\":\"%s\",\"allow_existing\":true,\"allow_duplicate\":false}",
      escaped_name,
      escaped_platform,
      escaped_client,
      escaped_client_version,
      escaped_hostname);
  if ((payload_written <= 0) || ((size_t)payload_written >= sizeof(payload))) {
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }

  int http_status_code = 0;
  char response_body[ROMM_HTTP_SMALL_BODY_SIZE];
  int transport_status = http_post_json(
      config,
      url,
      payload,
      &http_status_code,
      response_body,
      sizeof(response_body));
  if (transport_status < 0) {
    return transport_status;
  }

  app_log_write(APP_LOG_LEVEL_DEBUG, "http", "register_device status=%d body=%s", http_status_code, response_body);

  if ((http_status_code == 401) || (http_status_code == 403)) {
    return ROMM_CLIENT_ERR_AUTH;
  }
  if (http_status_code == 409) {
    return ROMM_CLIENT_ERR_CONFLICT;
  }
  if ((http_status_code != 200) && (http_status_code != 201)) {
    return ROMM_CLIENT_ERR_NETWORK;
  }

  if (extract_json_string_field(response_body, "device_id", out_device_id, out_device_id_size) == 0) {
    return ROMM_CLIENT_OK;
  }
  if (extract_json_scalar_field(response_body, "device_id", out_device_id, out_device_id_size) == 0) {
    return ROMM_CLIENT_OK;
  }
  if (extract_json_string_field(response_body, "id", out_device_id, out_device_id_size) == 0) {
    return ROMM_CLIENT_OK;
  }
  if (extract_json_scalar_field(response_body, "id", out_device_id, out_device_id_size) == 0) {
    return ROMM_CLIENT_OK;
  }

  app_log_write(APP_LOG_LEVEL_ERROR, "http", "register_device response missing device_id");
  return ROMM_CLIENT_ERR_NETWORK;
}
