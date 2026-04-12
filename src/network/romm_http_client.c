#include "romm_http_client.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
#define ROMM_HTTP_PLATFORM_BODY_SIZE (1024 * 1024)
#define ROMM_HTTP_SMALL_BODY_SIZE 4096
#define ROMM_HTTP_MAX_ROMS 4096
#define ROMM_HTTP_MAX_SEARCH_ROMS 256
#define ROMM_HTTP_PAGE_LIMIT 200
#define ROMM_HTTP_ROM_PAGE_LIMIT 10
#define ROMM_HTTP_MAX_UPLOAD_SIZE (2 * 1024 * 1024)
#define ROMM_HTTP_MAX_FILENAME 128
#define ROMM_HTTP_MAX_PLATFORM_SLUG 64
#define ROMM_HTTP_MAX_SEARCH_TERM ROMM_GAME_TITLE_LEN
#define ROMM_HTTP_MAX_SEARCH_VARIANTS 6
#define ROMM_HTTP_LOG_BODY_PREVIEW 160
#define ROMM_HTTP_LOG_BODY_DEBUG 224
#define ROMM_HTTP_FALLBACK_URL_BUFFER_SIZE (APP_CONFIG_MAX_URL_LEN + 256)
#define ROMM_HTTP_SSL_TRANSPORT_ERROR 0x80431075u
#define ROMM_HTTP_TLS_DISABLE_UNSUPPORTED 0x8043506Bu
#define ROMM_HTTP_DEFAULT_PLATFORM_FILTER "psx"
#define ROMM_HTTP_DEFAULT_SAVE_EMULATOR "pcsx_rearmed"
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
  int force_http_fallback;
  int module_net_loaded;
  int module_http_loaded;
  int module_ssl_loaded;
  int module_https_loaded;
} HttpRuntimeState;

typedef struct HttpResponseMeta {
  int has_content_length;
  unsigned long long content_length;
  char content_type[64];
  char server[64];
  char via[64];
  char location[96];
  char cf_ray[64];
} HttpResponseMeta;

typedef GameMatcherRomCandidate RomCatalogEntry;

static void http_runtime_term(HttpRuntimeState *state);
static int url_is_https(const char *url);
static void build_body_preview(const char *input, char *output, size_t output_size);
static int g_logged_https_verify_fallback_notice = 0;

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
 * Returns the URL scheme as a short label for diagnostics.
 */
static const char *url_scheme_label(const char *url) {
  if (url_is_https(url)) {
    return "https";
  }
  if (has_text(url) && (strncmp(url, "http://", 7) == 0)) {
    return "http";
  }
  return "(unknown)";
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
 * Percent-encodes one query parameter value for safe URL composition.
 */
static int url_encode_query_value(const char *input, char *output, size_t output_size) {
  static const char kHex[] = "0123456789ABCDEF";

  if ((input == NULL) || (output == NULL) || (output_size == 0U)) {
    return -1;
  }

  size_t out = 0U;
  for (const unsigned char *cursor = (const unsigned char *)input; *cursor != '\0'; ++cursor) {
    const unsigned char c = *cursor;
    const int is_unreserved =
        isalnum(c) || (c == '-') || (c == '_') || (c == '.') || (c == '~');

    if (is_unreserved) {
      if ((out + 1U) >= output_size) {
        output[0] = '\0';
        return -1;
      }
      output[out++] = (char)c;
      continue;
    }

    if ((out + 3U) >= output_size) {
      output[0] = '\0';
      return -1;
    }
    output[out++] = '%';
    output[out++] = kHex[(c >> 4) & 0x0F];
    output[out++] = kHex[c & 0x0F];
  }

  output[out] = '\0';
  return 0;
}

/*
 * Logs additional SSL details when Vita reports a transport-level HTTPS failure.
 */
static void log_ssl_error_details(int id) {
  if (id < 0) {
    return;
  }

  int ssl_error = 0;
  unsigned int ssl_detail = 0U;
  int status = sceHttpsGetSslError(id, &ssl_error, &ssl_detail);
  if (status < 0) {
    app_log_write(APP_LOG_LEVEL_WARN, "http", "sceHttpsGetSslError failed: 0x%08X", (unsigned int)status);
    return;
  }

  app_log_write(
      APP_LOG_LEVEL_WARN,
      "http",
      "SSL detail err=0x%08X detail=0x%08X",
      (unsigned int)ssl_error,
      ssl_detail);
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
 * Builds Authorization header value from username/password configuration.
 */
static int build_auth_value(const AppConfig *config, char *out_value, size_t out_value_size) {
  if ((config == NULL) || (out_value == NULL) || (out_value_size == 0U)) {
    return -1;
  }

  out_value[0] = '\0';
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
 * Copies one trimmed substring into a bounded destination.
 */
static int copy_trimmed_range(
    const char *start,
    const char *end,
    char *output,
    size_t output_size) {
  if ((start == NULL) || (end == NULL) || (output == NULL) || (output_size == 0U) || (start > end)) {
    return -1;
  }

  while ((start < end) && isspace((unsigned char)*start)) {
    start++;
  }
  while ((end > start) && isspace((unsigned char)*(end - 1))) {
    end--;
  }

  size_t length = (size_t)(end - start);
  if ((length == 0U) || (length >= output_size)) {
    output[0] = '\0';
    return -1;
  }

  memcpy(output, start, length);
  output[length] = '\0';
  return 0;
}

/*
 * Compares one header-name slice against an expected field name, case-insensitively.
 */
static int header_name_matches(const char *start, size_t length, const char *expected) {
  if ((start == NULL) || (expected == NULL)) {
    return 0;
  }

  size_t expected_length = strlen(expected);
  if (length != expected_length) {
    return 0;
  }

  for (size_t index = 0U; index < length; ++index) {
    unsigned char lhs = (unsigned char)start[index];
    unsigned char rhs = (unsigned char)expected[index];
    if (tolower(lhs) != tolower(rhs)) {
      return 0;
    }
  }

  return 1;
}

/*
 * Extracts one response header value from the raw Vita header block.
 */
static int extract_response_header_value(
    const char *headers,
    unsigned int headers_size,
    const char *field_name,
    char *output,
    size_t output_size) {
  if ((headers == NULL) || (headers_size == 0U) || !has_text(field_name) ||
      (output == NULL) || (output_size == 0U)) {
    return -1;
  }

  output[0] = '\0';

  const char *cursor = headers;
  const char *headers_end = headers + headers_size;
  while (cursor < headers_end) {
    const char *line_end = memchr(cursor, '\n', (size_t)(headers_end - cursor));
    if (line_end == NULL) {
      line_end = headers_end;
    }

    const char *trimmed_end = line_end;
    while ((trimmed_end > cursor) &&
           ((trimmed_end[-1] == '\r') || (trimmed_end[-1] == '\n'))) {
      trimmed_end--;
    }

    const char *colon = memchr(cursor, ':', (size_t)(trimmed_end - cursor));
    if ((colon != NULL) &&
        header_name_matches(cursor, (size_t)(colon - cursor), field_name) &&
        (copy_trimmed_range(colon + 1, trimmed_end, output, output_size) == 0)) {
      return 0;
    }

    cursor = line_end;
    if ((cursor < headers_end) && (*cursor == '\n')) {
      cursor++;
    }
  }

  return -1;
}

/*
 * Reads response metadata exposed by the Vita HTTP API for richer diagnostics.
 */
static void collect_http_response_meta(int request_id, HttpResponseMeta *meta) {
  if ((request_id < 0) || (meta == NULL)) {
    return;
  }

  memset(meta, 0, sizeof(*meta));

  unsigned long long content_length = 0ULL;
  if (sceHttpGetResponseContentLength(request_id, &content_length) >= 0) {
    meta->has_content_length = 1;
    meta->content_length = content_length;
  }

  char *raw_headers = NULL;
  unsigned int raw_headers_size = 0U;
  if (sceHttpGetAllResponseHeaders(request_id, &raw_headers, &raw_headers_size) < 0) {
    return;
  }

  extract_response_header_value(raw_headers, raw_headers_size, "Content-Type", meta->content_type, sizeof(meta->content_type));
  extract_response_header_value(raw_headers, raw_headers_size, "Server", meta->server, sizeof(meta->server));
  extract_response_header_value(raw_headers, raw_headers_size, "Via", meta->via, sizeof(meta->via));
  extract_response_header_value(raw_headers, raw_headers_size, "Location", meta->location, sizeof(meta->location));
  extract_response_header_value(raw_headers, raw_headers_size, "CF-Ray", meta->cf_ray, sizeof(meta->cf_ray));
}

/*
 * Logs one richer HTTP error summary with the effective URL and returned metadata.
 */
static void log_http_error_response(
    int request_id,
    const char *url,
    int status_code,
    const char *body) {
  HttpResponseMeta meta;
  collect_http_response_meta(request_id, &meta);

  char content_length[32];
  if (meta.has_content_length) {
    snprintf(content_length, sizeof(content_length), "%llu", meta.content_length);
  } else {
    safe_copy(content_length, sizeof(content_length), "(unknown)");
  }

  app_log_write(
      APP_LOG_LEVEL_WARN,
      "http",
      "response meta status=%d scheme=%s content_type=%s content_length=%s",
      status_code,
      url_scheme_label(url),
      has_text(meta.content_type) ? meta.content_type : "(unknown)",
      content_length);
  app_log_write(
      APP_LOG_LEVEL_WARN,
      "http",
      "response target status=%d url=%s",
      status_code,
      has_text(url) ? url : "(unknown)");

  if (has_text(meta.server) || has_text(meta.via) || has_text(meta.location) || has_text(meta.cf_ray)) {
    app_log_write(
        APP_LOG_LEVEL_WARN,
        "http",
        "response headers status=%d server=%s via=%s location=%s cf_ray=%s",
        status_code,
        has_text(meta.server) ? meta.server : "(none)",
        has_text(meta.via) ? meta.via : "(none)",
        has_text(meta.location) ? meta.location : "(none)",
        has_text(meta.cf_ray) ? meta.cf_ray : "(none)");
  }

  char body_preview[ROMM_HTTP_LOG_BODY_DEBUG];
  build_body_preview(body, body_preview, sizeof(body_preview));
  app_log_write(
      APP_LOG_LEVEL_WARN,
      "http",
      "response body status=%d body=%s",
      status_code,
      body_preview);
}

/*
 * Builds one compact single-line preview from a response body for INFO/WARN logs.
 */
static void build_body_preview(const char *input, char *output, size_t output_size) {
  if ((output == NULL) || (output_size == 0U)) {
    return;
  }

  output[0] = '\0';
  if (!has_text(input)) {
    safe_copy(output, output_size, "(empty)");
    return;
  }

  size_t out = 0U;
  int last_was_space = 1;
  int truncated = 0;
  for (const unsigned char *cursor = (const unsigned char *)input; *cursor != '\0'; ++cursor) {
    unsigned char c = *cursor;
    if (isspace(c)) {
      if (!last_was_space && (out > 0U) && ((out + 1U) < output_size)) {
        output[out++] = ' ';
        last_was_space = 1;
      }
      continue;
    }

    if ((out + 1U) >= output_size) {
      truncated = 1;
      break;
    }

    output[out++] = (char)c;
    last_was_space = 0;
  }

  while ((out > 0U) && (output[out - 1U] == ' ')) {
    out--;
  }

  if (truncated && (output_size >= 4U)) {
    if (out > (output_size - 4U)) {
      out = output_size - 4U;
    }
    output[out++] = '.';
    output[out++] = '.';
    output[out++] = '.';
  }

  output[out] = '\0';
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
 * Returns configured RomM platform filter used for catalog lookups.
 */
static const char *romm_catalog_platform_filter(const AppConfig *config) {
  if ((config != NULL) && has_text(config->romm_platform_filter)) {
    return config->romm_platform_filter;
  }
  return ROMM_HTTP_DEFAULT_PLATFORM_FILTER;
}

/*
 * Returns configured emulator slug used for /api/saves endpoints.
 */
static const char *romm_save_emulator(const AppConfig *config) {
  if ((config != NULL) && has_text(config->romm_save_emulator)) {
    return config->romm_save_emulator;
  }
  return ROMM_HTTP_DEFAULT_SAVE_EMULATOR;
}

/*
 * Builds a compact search term from one local text source for /api/roms.
 * Non-alnum separators collapse to single spaces.
 */
static int build_compact_search_term(
    const char *source,
    char *out_term,
    size_t out_term_size) {
  if (!has_text(source) || (out_term == NULL) || (out_term_size == 0U)) {
    return -1;
  }

  size_t out = 0U;
  int last_was_space = 1;
  for (const unsigned char *cursor = (const unsigned char *)source;
       (*cursor != '\0') && ((out + 1U) < out_term_size);
       ++cursor) {
    unsigned char c = *cursor;
    if (isalnum(c)) {
      out_term[out++] = (char)c;
      last_was_space = 0;
      continue;
    }

    if (!last_was_space && (out > 0U)) {
      out_term[out++] = ' ';
      last_was_space = 1;
    }
  }

  while ((out > 0U) && (out_term[out - 1U] == ' ')) {
    out--;
  }
  out_term[out] = '\0';

  return (out >= 4U) ? 0 : -1;
}

/*
 * Appends one deduplicated search variant when it is meaningful.
 */
static void append_search_variant(
    const char *candidate,
    const char *reason,
    char out_terms[ROMM_HTTP_MAX_SEARCH_VARIANTS][ROMM_HTTP_MAX_SEARCH_TERM],
    char out_reasons[ROMM_HTTP_MAX_SEARCH_VARIANTS][24],
    int *io_count) {
  if ((out_terms == NULL) || (out_reasons == NULL) || (io_count == NULL) ||
      (*io_count < 0) || (*io_count >= ROMM_HTTP_MAX_SEARCH_VARIANTS)) {
    return;
  }

  if (!has_text(candidate)) {
    return;
  }

  const size_t length = strlen(candidate);
  int contains_digit = 0;
  int contains_alpha = 0;
  for (const char *cursor = candidate; *cursor != '\0'; ++cursor) {
    if (isdigit((unsigned char)*cursor)) {
      contains_digit = 1;
    } else if (isalpha((unsigned char)*cursor)) {
      contains_alpha = 1;
    }
  }
  const int looks_like_alias = (contains_alpha != 0) && (contains_digit != 0) && (length <= 6U);
  if ((length < 4U) && !looks_like_alias) {
    return;
  }

  for (int i = 0; i < *io_count; ++i) {
    if (sync_string_ieq(out_terms[i], candidate)) {
      return;
    }
  }

  snprintf(out_terms[*io_count], ROMM_HTTP_MAX_SEARCH_TERM, "%s", candidate);
  snprintf(out_reasons[*io_count], 24, "%s", has_text(reason) ? reason : "variant");
  (*io_count)++;
}

/*
 * Removes short alias prefix token, e.g. "r4 ridge racer type 4" -> "ridge racer type 4".
 */
static int build_alias_stripped_variant(
    const char *normalized_title,
    char *out_term,
    size_t out_term_size) {
  if (!has_text(normalized_title) || (out_term == NULL) || (out_term_size == 0U)) {
    return -1;
  }

  const char *first_space = strchr(normalized_title, ' ');
  if (first_space == NULL) {
    return -1;
  }

  char first_token[16];
  size_t first_len = (size_t)(first_space - normalized_title);
  if ((first_len == 0U) || (first_len >= sizeof(first_token))) {
    return -1;
  }
  memcpy(first_token, normalized_title, first_len);
  first_token[first_len] = '\0';

  int has_digit = 0;
  int has_alpha = 0;
  for (size_t i = 0U; i < first_len; ++i) {
    if (isdigit((unsigned char)first_token[i])) {
      has_digit = 1;
    } else if (isalpha((unsigned char)first_token[i])) {
      has_alpha = 1;
    }
  }
  if (!has_alpha || !has_digit || (first_len > 5U)) {
    return -1;
  }

  const char *remainder = first_space + 1;
  while (*remainder == ' ') {
    remainder++;
  }
  if (strlen(remainder) < 4U) {
    return -1;
  }

  size_t copy_len = strlen(remainder);
  if (copy_len >= out_term_size) {
    copy_len = out_term_size - 1U;
  }
  memcpy(out_term, remainder, copy_len);
  out_term[copy_len] = '\0';
  return 0;
}

/*
 * Builds a compact variant from significant title tokens.
 */
static int build_significant_token_variant(
    const char *normalized_title,
    char *out_term,
    size_t out_term_size) {
  if (!has_text(normalized_title) || (out_term == NULL) || (out_term_size == 0U)) {
    return -1;
  }

  out_term[0] = '\0';

  static const char *const noise_tokens[] = {
      "the",
      "and",
      "of",
      "edition",
      "ver",
      "version",
      "disc",
      "cd",
      "usa",
      "europe",
      "japan"};

  char token[ROMM_HTTP_MAX_SEARCH_TERM];
  size_t token_len = 0U;
  size_t out = 0U;
  int kept = 0;
  for (const char *cursor = normalized_title;; ++cursor) {
    const char c = *cursor;
    if ((c != ' ') && (c != '\0')) {
      if ((token_len + 1U) < sizeof(token)) {
        token[token_len++] = c;
      }
      continue;
    }

    if (token_len > 0U) {
      token[token_len] = '\0';
      int is_noise = 0;
      for (size_t i = 0U; i < (sizeof(noise_tokens) / sizeof(noise_tokens[0])); ++i) {
        if (sync_string_ieq(token, noise_tokens[i])) {
          is_noise = 1;
          break;
        }
      }

      if (!is_noise && ((strlen(token) >= 2U) || isdigit((unsigned char)token[0]))) {
        if ((kept > 0) && ((out + 1U) < out_term_size)) {
          out_term[out++] = ' ';
        }
        for (size_t i = 0U; (i < strlen(token)) && ((out + 1U) < out_term_size); ++i) {
          out_term[out++] = token[i];
        }
        kept++;
        if (kept >= 5) {
          break;
        }
      }
      token_len = 0U;
    }

    if (c == '\0') {
      break;
    }
  }
  out_term[out] = '\0';
  return (strlen(out_term) >= 4U) ? 0 : -1;
}

/*
 * Builds ordered /api/roms search variants for one local save.
 */
static int build_rom_search_variants(
    const SyncSaveDescriptor *local_item,
    char out_terms[ROMM_HTTP_MAX_SEARCH_VARIANTS][ROMM_HTTP_MAX_SEARCH_TERM],
    char out_reasons[ROMM_HTTP_MAX_SEARCH_VARIANTS][24]) {
  if ((local_item == NULL) || (out_terms == NULL) || (out_reasons == NULL)) {
    return 0;
  }

  for (int i = 0; i < ROMM_HTTP_MAX_SEARCH_VARIANTS; ++i) {
    out_terms[i][0] = '\0';
    out_reasons[i][0] = '\0';
  }

  int count = 0;

  char game_id_term[ROMM_HTTP_MAX_SEARCH_TERM];
  if (build_compact_search_term(local_item->game_id, game_id_term, sizeof(game_id_term)) == 0) {
    append_search_variant(game_id_term, "game_id", out_terms, out_reasons, &count);
  }

  char raw_title_term[ROMM_HTTP_MAX_SEARCH_TERM];
  if (build_compact_search_term(local_item->title, raw_title_term, sizeof(raw_title_term)) == 0) {
    append_search_variant(raw_title_term, "title_raw", out_terms, out_reasons, &count);
  }

  char normalized_title[ROMM_HTTP_MAX_SEARCH_TERM];
  game_matcher_normalize_title(local_item->title, normalized_title, sizeof(normalized_title));
  append_search_variant(normalized_title, "title_normalized", out_terms, out_reasons, &count);

  char alias_trimmed[ROMM_HTTP_MAX_SEARCH_TERM];
  alias_trimmed[0] = '\0';
  if (build_alias_stripped_variant(normalized_title, alias_trimmed, sizeof(alias_trimmed)) == 0) {
    append_search_variant(alias_trimmed, "title_no_alias", out_terms, out_reasons, &count);
  }

  char significant_variant[ROMM_HTTP_MAX_SEARCH_TERM];
  if (build_significant_token_variant(
          has_text(alias_trimmed) ? alias_trimmed : normalized_title,
          significant_variant,
          sizeof(significant_variant)) == 0) {
    append_search_variant(significant_variant, "title_tokens", out_terms, out_reasons, &count);
  }

  return count;
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
 * Converts one ISO-like timestamp into the UTC-normalized deterministic sync
 * timestamp basis used by the engine (seconds precision, no libc timezone
 * dependency).
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

  int64_t parsed = 0;
  if (sync_parse_local_timestamp(normalized, &parsed) < 0) {
    return -1;
  }

  const char *suffix = timestamp + 19;
  if (*suffix == '.') {
    suffix += 1;
    while (isdigit((unsigned char)*suffix)) {
      suffix += 1;
    }
  }

  if ((*suffix == '\0') || (*suffix == ' ')) {
    *out_unix = parsed;
    return 0;
  }

  int offset_seconds = 0;
  if ((*suffix == 'Z') || (*suffix == 'z')) {
    offset_seconds = 0;
    suffix += 1;
  } else if ((*suffix == '+') || (*suffix == '-')) {
    int sign = (*suffix == '-') ? -1 : 1;
    suffix += 1;

    if (!isdigit((unsigned char)suffix[0]) || !isdigit((unsigned char)suffix[1])) {
      return -1;
    }

    int hours = ((suffix[0] - '0') * 10) + (suffix[1] - '0');
    suffix += 2;

    int minutes = 0;
    if (*suffix == ':') {
      suffix += 1;
    }
    if (*suffix != '\0') {
      if (!isdigit((unsigned char)suffix[0]) || !isdigit((unsigned char)suffix[1])) {
        return -1;
      }
      minutes = ((suffix[0] - '0') * 10) + (suffix[1] - '0');
      suffix += 2;
    }

    offset_seconds = sign * ((hours * 3600) + (minutes * 60));
  } else {
    *out_unix = parsed;
    return 0;
  }

  while (isspace((unsigned char)*suffix)) {
    suffix += 1;
  }
  if (*suffix != '\0') {
    return -1;
  }

  *out_unix = parsed - (int64_t)offset_seconds;
  return 0;
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
 * Builds one local-time timestamp token used in uploaded SRM filenames.
 * Local scan metadata is normalized on a UTC-like basis, so convert it back to
 * the device local timezone before exposing it in the human-readable filename.
 */
static void build_upload_timestamp_token(
    const SyncSaveDescriptor *local_item,
    char *out_token,
    size_t out_size) {
  if ((out_token == NULL) || (out_size == 0U)) {
    return;
  }

  out_token[0] = '\0';
  if (local_item == NULL) {
    return;
  }

  if (local_item->timestamp_unix > 0) {
    time_t raw = (time_t)local_item->timestamp_unix;
    struct tm *local = localtime(&raw);
    if (local != NULL) {
      snprintf(
          out_token,
          out_size,
          "%04d%02d%02d-%02d%02d%02d",
          local->tm_year + 1900,
          local->tm_mon + 1,
          local->tm_mday,
          local->tm_hour,
          local->tm_min,
          local->tm_sec);
      return;
    }
  }

  if (strlen(local_item->timestamp_text) >= 19U) {
    const char *text = local_item->timestamp_text;
    if ((text[4] == '-') && (text[7] == '-') && (text[10] == ' ') &&
        (text[13] == ':') && (text[16] == ':')) {
      snprintf(
          out_token,
          out_size,
          "%.4s%.2s%.2s-%.2s%.2s%.2s",
          text + 0,
          text + 5,
          text + 8,
          text + 11,
          text + 14,
          text + 17);
    }
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

  char timestamp_token[32];
  build_upload_timestamp_token(local_item, timestamp_token, sizeof(timestamp_token));

  if (has_text(timestamp_token)) {
    snprintf(out_filename, out_size, "%s_%s_%s.srm", normalized_game_id, slot, timestamp_token);
  } else {
    snprintf(out_filename, out_size, "%s_%s.srm", normalized_game_id, slot);
  }
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
 * Extracts an integer field from a small JSON payload, quoted or unquoted.
 */
static int extract_json_int_field(
    const char *json,
    const char *field_name,
    int *out_value) {
  if (!has_text(json) || !has_text(field_name) || (out_value == NULL)) {
    return -1;
  }

  char scalar[32];
  if (extract_json_scalar_field(json, field_name, scalar, sizeof(scalar)) == 0) {
    return parse_int_value(scalar, out_value);
  }
  if (extract_json_string_field(json, field_name, scalar, sizeof(scalar)) == 0) {
    return parse_int_value(scalar, out_value);
  }

  return -1;
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
 * Extracts an integer field from a [start,end) range, supporting scalar or string payloads.
 */
static int extract_json_int_field_in_range(
    const char *start,
    const char *end,
    const char *field_name,
    int *out_value) {
  if ((start == NULL) || (end == NULL) || (out_value == NULL) || !has_text(field_name)) {
    return -1;
  }

  char scalar[64];
  if ((extract_json_scalar_field_in_range(start, end, field_name, scalar, sizeof(scalar)) == 0) &&
      (parse_int_value(scalar, out_value) == 0)) {
    return 0;
  }

  if ((extract_json_string_field_in_range(start, end, field_name, scalar, sizeof(scalar)) == 0) &&
      (parse_int_value(scalar, out_value) == 0)) {
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
 * Enriches one ROM catalog entry with alternative names/aliases when present.
 */
static void extract_alternative_names_metadata(
    const char *object_start,
    const char *object_end,
    RomCatalogEntry *entry) {
  if ((object_start == NULL) || (object_end == NULL) ||
      (object_start >= object_end) || (entry == NULL)) {
    return;
  }

  const char *const list_keys[] = {
      "alternative_names",
      "alternativeNames",
      "aliases"};
  const char *const scalar_keys[] = {
      "alternative_name",
      "alternativeName",
      "alias"};

  for (size_t i = 0U; i < (sizeof(list_keys) / sizeof(list_keys[0])); ++i) {
    char list_values[GAME_MATCHER_MAX_ALTERNATIVE_NAMES_LEN];
    if (extract_json_string_array_field_in_range(object_start, object_end, list_keys[i], list_values, sizeof(list_values)) == 0) {
      char token[GAME_MATCHER_MAX_ALTERNATIVE_NAMES_LEN];
      size_t token_len = 0U;
      for (const char *cursor = list_values;; ++cursor) {
        char c = *cursor;
        if ((c != '|') && (c != '\0') && ((token_len + 1U) < sizeof(token))) {
          token[token_len++] = c;
          continue;
        }

        if (token_len > 0U) {
          token[token_len] = '\0';
          append_pipe_list_unique(entry->alternative_names, sizeof(entry->alternative_names), token);
          token_len = 0U;
        }

        if (c == '\0') {
          break;
        }
      }
    }
  }

  for (size_t i = 0U; i < (sizeof(scalar_keys) / sizeof(scalar_keys[0])); ++i) {
    char value[GAME_MATCHER_MAX_ALTERNATIVE_NAMES_LEN];
    if (extract_json_string_field_in_range(object_start, object_end, scalar_keys[i], value, sizeof(value)) == 0) {
      append_pipe_list_unique(entry->alternative_names, sizeof(entry->alternative_names), value);
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
        state->force_http_fallback = 1;
        if (((unsigned int)disable_status != ROMM_HTTP_TLS_DISABLE_UNSUPPORTED) &&
            ((unsigned int)disable_status != ROMM_HTTP_SSL_TRANSPORT_ERROR)) {
          app_log_write(
              APP_LOG_LEVEL_WARN,
              "http",
              "Failed to disable TLS verification; using HTTP fallback because verify_tls=false: 0x%08X",
              (unsigned int)disable_status);
        }
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

  char fallback_url[ROMM_HTTP_FALLBACK_URL_BUFFER_SIZE];
  fallback_url[0] = '\0';
  HttpRuntimeState runtime_state;
  int runtime_status = http_runtime_init(config, url, &runtime_state);
  if (runtime_status < 0) {
    return runtime_status;
  }

  if (runtime_state.force_http_fallback &&
      build_http_fallback_url(url, fallback_url, sizeof(fallback_url)) == 0) {
    http_runtime_term(&runtime_state);
    if (!g_logged_https_verify_fallback_notice) {
      app_log_write(
          APP_LOG_LEVEL_INFO,
          "http",
          "Using HTTP fallback because HTTPS verify override is unavailable and verify_tls=false target=%s",
          fallback_url);
      g_logged_https_verify_fallback_notice = 1;
    }
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

  int template_id = -1;
  int connection_id = -1;
  int request_id = -1;
  int result = ROMM_CLIENT_ERR_NETWORK;
  int retry_over_http = 0;
  int transport_error = 0;

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
    if ((unsigned int)transport_error == ROMM_HTTP_SSL_TRANSPORT_ERROR) {
      log_ssl_error_details(request_id);
    }
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
      if ((*out_status_code < 200) || (*out_status_code >= 400)) {
        log_http_error_response(request_id, url, *out_status_code, out_body);
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
    if ((*out_status_code < 200) || (*out_status_code >= 400)) {
      log_http_error_response(request_id, url, *out_status_code, out_body);
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
 * Sends a multipart/form-data request carrying a single file field.
 * method is typically POST for creation or PUT for in-place replacement.
 */
static int http_send_multipart_file(
    const AppConfig *config,
    int method,
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
      method,
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
      extract_json_string_field_in_range(object_start, object_end, "fs_name_no_tags", entry->fs_name_no_tags, sizeof(entry->fs_name_no_tags));
      extract_json_string_field_in_range(object_start, object_end, "fs_name_no_ext", entry->fs_name_no_ext, sizeof(entry->fs_name_no_ext));
      extract_json_string_field_in_range(object_start, object_end, "platform_slug", entry->platform_slug, sizeof(entry->platform_slug));
      extract_serial_metadata(object_start, object_end, entry);
      extract_alternative_names_metadata(object_start, object_end, entry);
      count++;
    }

    cursor = next_cursor;
  }

  return count;
}

/*
 * Resolves a RomM platform numeric identifier from /api/platforms by slug or fs_slug.
 * Returns 0 when found, 1 when not found, or -1 on parse failure.
 */
static int parse_platform_id_from_list(
    const char *json,
    const char *platform_slug,
    int *out_platform_id) {
  if ((json == NULL) || !has_text(platform_slug) || (out_platform_id == NULL)) {
    return -1;
  }

  *out_platform_id = 0;

  const char *array_start = NULL;
  const char *array_end = NULL;
  if (find_object_array_range(json, NULL, &array_start, &array_end) < 0) {
    if (find_object_array_range(json, "items", &array_start, &array_end) < 0) {
      if (find_object_array_range(json, "data", &array_start, &array_end) < 0) {
        return -1;
      }
    }
  }

  const char *cursor = array_start + 1;
  while (cursor < array_end) {
    const char *object_start = NULL;
    const char *object_end = NULL;
    const char *next_cursor = NULL;
    if (next_object_in_array(cursor, array_end, &object_start, &object_end, &next_cursor) < 0) {
      break;
    }

    int platform_id = 0;
    char slug[GAME_MATCHER_MAX_PLATFORM_SLUG_LEN];
    char fs_slug[GAME_MATCHER_MAX_PLATFORM_SLUG_LEN];
    slug[0] = '\0';
    fs_slug[0] = '\0';

    if ((extract_json_int_field_in_range(object_start, object_end, "id", &platform_id) == 0) &&
        ((extract_json_string_field_in_range(object_start, object_end, "slug", slug, sizeof(slug)) == 0) ||
         (extract_json_string_field_in_range(object_start, object_end, "fs_slug", fs_slug, sizeof(fs_slug)) == 0))) {
      if (sync_string_ieq(slug, platform_slug) || sync_string_ieq(fs_slug, platform_slug)) {
        *out_platform_id = platform_id;
        return 0;
      }
    }

    cursor = next_cursor;
  }

  return 1;
}

/*
 * Looks up the numeric RomM platform id used by current API versions.
 * Returns ROMM_CLIENT_OK on success, with out_platform_id set to 0 when no match exists
 * or when /api/platforms is unavailable and the legacy rom query should be used instead.
 */
static int resolve_platform_id(
    const AppConfig *config,
    const char *platform_slug,
    int *out_platform_id) {
  if ((config == NULL) || !has_text(platform_slug) || (out_platform_id == NULL)) {
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }

  *out_platform_id = 0;

  char url[APP_CONFIG_MAX_URL_LEN + 32];
  if (build_api_url(config->romm_url, "/api/platforms", url, sizeof(url)) < 0) {
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }

  char *body = (char *)malloc(ROMM_HTTP_PLATFORM_BODY_SIZE);
  if (body == NULL) {
    return ROMM_CLIENT_ERR_NETWORK;
  }

  int status_code = 0;
  app_log_write(APP_LOG_LEVEL_INFO, "http", "request GET /api/platforms slug=%s", platform_slug);
  int transport_status = http_get_text(config, url, &status_code, body, ROMM_HTTP_PLATFORM_BODY_SIZE);
  if (transport_status < 0) {
    free(body);
    return transport_status;
  }
  app_log_write(APP_LOG_LEVEL_INFO, "http", "response GET /api/platforms status=%d slug=%s", status_code, platform_slug);
  if ((status_code == 401) || (status_code == 403)) {
    free(body);
    return ROMM_CLIENT_ERR_AUTH;
  }
  if (status_code != 200) {
    char body_preview[ROMM_HTTP_LOG_BODY_PREVIEW];
    build_body_preview(body, body_preview, sizeof(body_preview));
    app_log_write(
        APP_LOG_LEVEL_WARN,
        "http",
        "platform lookup failed status=%d slug=%s body=%s; falling back to legacy rom query",
        status_code,
        platform_slug,
        body_preview);
    free(body);
    return ROMM_CLIENT_OK;
  }

  int parse_status = parse_platform_id_from_list(body, platform_slug, out_platform_id);
  if (parse_status < 0) {
    app_log_write(
        APP_LOG_LEVEL_WARN,
        "http",
        "platform lookup returned unsupported response format for slug=%s; falling back to legacy rom query",
        platform_slug);
    {
      char body_preview[ROMM_HTTP_LOG_BODY_DEBUG];
      build_body_preview(body, body_preview, sizeof(body_preview));
      app_log_write(APP_LOG_LEVEL_DEBUG, "http", "platform raw response slug=%s body=%s", platform_slug, body_preview);
    }
    free(body);
    return ROMM_CLIENT_OK;
  }
  free(body);

  if (*out_platform_id > 0) {
    app_log_write(
        APP_LOG_LEVEL_INFO,
        "http",
        "resolved platform slug=%s -> platform_id=%d",
        platform_slug,
        *out_platform_id);
  } else {
    app_log_write(
        APP_LOG_LEVEL_WARN,
        "http",
        "platform slug=%s not found via /api/platforms; falling back to legacy rom query",
        platform_slug);
  }

  return ROMM_CLIENT_OK;
}

/*
 * Builds one /api/roms path with optional platform-id and search-term filters.
 */
static int build_rom_catalog_path(
    const char *platform_filter,
    int platform_id,
    int limit,
    int offset,
    const char *search_term,
    char *out_path,
    size_t out_path_size) {
  if ((out_path == NULL) || (out_path_size == 0U) || (limit <= 0) || (offset < 0)) {
    return -1;
  }

  if (has_text(search_term)) {
    char encoded_search_term[(ROMM_HTTP_MAX_SEARCH_TERM * 3) + 1];
    if (url_encode_query_value(search_term, encoded_search_term, sizeof(encoded_search_term)) < 0) {
      return -1;
    }

    if (platform_id > 0) {
      int written = snprintf(
          out_path,
          out_path_size,
          "/api/roms?limit=%d&offset=%d&platform_ids=%d&search_term=%s&fields=id,name,fs_name,fs_name_no_tags,fs_name_no_ext,platform_slug,serial,serials,serial_number,product_code,disc_id,game_id,alternative_names,alternativeNames",
          limit,
          offset,
          platform_id,
          encoded_search_term);
      return ((written > 0) && ((size_t)written < out_path_size)) ? 0 : -1;
    }

    int written = snprintf(
        out_path,
        out_path_size,
        "/api/roms?limit=%d&offset=%d&platform=%s&search_term=%s&fields=id,name,fs_name,fs_name_no_tags,fs_name_no_ext,platform_slug,serial,serials,serial_number,product_code,disc_id,game_id,alternative_names,alternativeNames",
        limit,
        offset,
        platform_filter,
        encoded_search_term);
    return ((written > 0) && ((size_t)written < out_path_size)) ? 0 : -1;
  }

  if (platform_id > 0) {
    int written = snprintf(
        out_path,
        out_path_size,
        "/api/roms?limit=%d&offset=%d&platform_ids=%d&fields=id,name,fs_name,fs_name_no_tags,fs_name_no_ext,platform_slug,serial,serials,serial_number,product_code,disc_id,game_id,alternative_names,alternativeNames",
        limit,
        offset,
        platform_id);
    return ((written > 0) && ((size_t)written < out_path_size)) ? 0 : -1;
  }

  int written = snprintf(
      out_path,
      out_path_size,
      "/api/roms?limit=%d&offset=%d&platform=%s&fields=id,name,fs_name,fs_name_no_tags,fs_name_no_ext,platform_slug,serial,serials,serial_number,product_code,disc_id,game_id,alternative_names,alternativeNames",
      limit,
      offset,
      platform_filter);
  return ((written > 0) && ((size_t)written < out_path_size)) ? 0 : -1;
}

/*
 * Loads one RomM catalog slice, optionally narrowed by search term.
 */
static int fetch_rom_catalog(
    const AppConfig *config,
    const char *platform_filter,
    int platform_id,
    const char *search_term,
    RomCatalogEntry *out_catalog,
    int max_catalog,
    char *body,
    size_t body_size,
    int *out_catalog_count) {
  if ((config == NULL) ||
      !has_text(platform_filter) ||
      (out_catalog == NULL) ||
      (max_catalog <= 0) ||
      (body == NULL) ||
      (body_size == 0U) ||
      (out_catalog_count == NULL)) {
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }

  *out_catalog_count = 0;

  int total = INT_MAX;
  int catalog_count = 0;
  for (int offset = 0; (offset < total) && (catalog_count < max_catalog); offset += ROMM_HTTP_ROM_PAGE_LIMIT) {
    char path[768];
    if (build_rom_catalog_path(
            platform_filter,
            platform_id,
            ROMM_HTTP_ROM_PAGE_LIMIT,
            offset,
            search_term,
            path,
            sizeof(path)) < 0) {
      return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
    }

    char url[APP_CONFIG_MAX_URL_LEN + sizeof(path)];
    if (build_api_url(config->romm_url, path, url, sizeof(url)) < 0) {
      return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
    }

    int status_code = 0;
    if (has_text(search_term)) {
      if (platform_id > 0) {
        app_log_write(
            APP_LOG_LEVEL_INFO,
            "http",
            "request GET /api/roms offset=%d limit=%d platform_id=%d term=%s",
            offset,
            ROMM_HTTP_ROM_PAGE_LIMIT,
            platform_id,
            search_term);
      } else {
        app_log_write(
            APP_LOG_LEVEL_INFO,
            "http",
            "request GET /api/roms offset=%d limit=%d platform=%s term=%s",
            offset,
            ROMM_HTTP_ROM_PAGE_LIMIT,
            platform_filter,
            search_term);
      }
    } else if (platform_id > 0) {
      app_log_write(
          APP_LOG_LEVEL_INFO,
          "http",
          "request GET /api/roms offset=%d limit=%d platform_id=%d",
          offset,
          ROMM_HTTP_ROM_PAGE_LIMIT,
          platform_id);
    } else {
      app_log_write(
          APP_LOG_LEVEL_INFO,
          "http",
          "request GET /api/roms offset=%d limit=%d platform=%s",
          offset,
          ROMM_HTTP_ROM_PAGE_LIMIT,
          platform_filter);
    }
    int transport_status = http_get_text(config, url, &status_code, body, body_size);
    if (transport_status < 0) {
      return transport_status;
    }
    if ((status_code == 401) || (status_code == 403)) {
      return ROMM_CLIENT_ERR_AUTH;
    }
    if (status_code != 200) {
      char body_preview[ROMM_HTTP_LOG_BODY_PREVIEW];
      build_body_preview(body, body_preview, sizeof(body_preview));
      if (has_text(search_term)) {
        app_log_write(
            APP_LOG_LEVEL_WARN,
            "http",
            "rom search fetch failed status=%d offset=%d term=%s body=%s",
            status_code,
            offset,
            search_term,
            body_preview);
      } else {
        app_log_write(
            APP_LOG_LEVEL_WARN,
            "http",
            "rom catalog fetch failed status=%d offset=%d body=%s",
            status_code,
            offset,
            body_preview);
      }
      return ROMM_CLIENT_ERR_NETWORK;
    }

    int page_total = -1;
    int remaining = max_catalog - catalog_count;
    int parsed = parse_rom_catalog_entries(body, &out_catalog[catalog_count], remaining, &page_total);
    if (parsed < 0) {
      char body_preview[ROMM_HTTP_LOG_BODY_DEBUG];
      build_body_preview(body, body_preview, sizeof(body_preview));
      if (has_text(search_term)) {
        app_log_write(
            APP_LOG_LEVEL_WARN,
            "http",
            "rom search returned unsupported response format offset=%d term=%s body=%s",
            offset,
            search_term,
            body_preview);
      } else {
        app_log_write(
            APP_LOG_LEVEL_WARN,
            "http",
            "rom catalog returned unsupported response format offset=%d body=%s",
            offset,
            body_preview);
      }
      return ROMM_CLIENT_ERR_NETWORK;
    }

    if (page_total > 0) {
      total = page_total;
    }
    if (parsed == 0) {
      break;
    }

    if (has_text(search_term)) {
      app_log_write(
          APP_LOG_LEVEL_INFO,
          "http",
          "response GET /api/roms status=%d offset=%d parsed=%d total_hint=%d term=%s",
          status_code,
          offset,
          parsed,
          page_total,
          search_term);
    } else {
      app_log_write(
          APP_LOG_LEVEL_INFO,
          "http",
          "response GET /api/roms status=%d offset=%d parsed=%d total_hint=%d",
          status_code,
          offset,
          parsed,
          page_total);
    }

    if (has_text(search_term)) {
      app_log_write(
          APP_LOG_LEVEL_DEBUG,
          "http",
          "rom search page parsed offset=%d parsed=%d total_hint=%d term=%s",
          offset,
          parsed,
          page_total,
          search_term);
    } else {
      app_log_write(
          APP_LOG_LEVEL_DEBUG,
          "http",
          "rom catalog page parsed offset=%d parsed=%d total_hint=%d",
          offset,
          parsed,
          page_total);
    }

    catalog_count += parsed;
  }

  *out_catalog_count = catalog_count;

  if (has_text(search_term)) {
    app_log_write(
        APP_LOG_LEVEL_INFO,
        "http",
        "rom search loaded entries=%d platform=%s term=%s",
        catalog_count,
        platform_filter,
        search_term);
  } else {
    app_log_write(
        APP_LOG_LEVEL_INFO,
        "http",
        "rom catalog loaded entries=%d platform=%s",
        catalog_count,
        platform_filter);
  }

  if (catalog_count <= 0) {
    if (has_text(search_term)) {
      app_log_write(
          APP_LOG_LEVEL_WARN,
          "http",
          "rom search returned no entries term=%s",
          search_term);
    } else {
      app_log_write(
          APP_LOG_LEVEL_WARN,
          "http",
          "rom catalog is empty or unsupported response format");
    }
  }

  return ROMM_CLIENT_OK;
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
    if (extract_json_int_field_in_range(object_start, object_end, "id", &item->remote_id) < 0) {
      item->remote_id = -1;
    }
    if (extract_json_int_field_in_range(object_start, object_end, "rom_id", &item->rom_id) < 0) {
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
 * Searches the retained remote entry bucket for one RomM game.
 * Remote selection intentionally ignores slot so the newest server-side save
 * by updated_at wins for the whole rom_id.
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
        (entry->rom_id == candidate->rom_id)) {
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

  RomCatalogEntry *search_catalog = (RomCatalogEntry *)malloc(sizeof(RomCatalogEntry) * ROMM_HTTP_MAX_SEARCH_ROMS);
  if (search_catalog == NULL) {
    return ROMM_CLIENT_ERR_NETWORK;
  }
  char *body = (char *)malloc(ROMM_HTTP_MAX_BODY_SIZE);
  if (body == NULL) {
    free(search_catalog);
    return ROMM_CLIENT_ERR_NETWORK;
  }

  const char *platform_filter = romm_catalog_platform_filter(config);
  int platform_id = 0;
  int platform_status = resolve_platform_id(config, platform_filter, &platform_id);
  if (platform_status < 0) {
    free(body);
    free(search_catalog);
    return platform_status;
  }

  char last_search_term[ROMM_HTTP_MAX_SEARCH_TERM];
  last_search_term[0] = '\0';
  int last_search_count = 0;

  int resolved_count = 0;
  for (int i = 0; i < item_count; ++i) {
    SyncSaveDescriptor *local_item = &items[i];
    if (local_item->rom_id > 0) {
      resolved_count++;
      continue;
    }

    GameMatcherResolution resolution;
    int resolved_rom_id = -1;

    char search_terms[ROMM_HTTP_MAX_SEARCH_VARIANTS][ROMM_HTTP_MAX_SEARCH_TERM];
    char search_reasons[ROMM_HTTP_MAX_SEARCH_VARIANTS][24];
    int search_term_count = build_rom_search_variants(local_item, search_terms, search_reasons);

    if (app_log_is_enabled(APP_LOG_LEVEL_DEBUG) && (search_term_count > 0)) {
      for (int variant_index = 0; variant_index < search_term_count; ++variant_index) {
        app_log_write(
            APP_LOG_LEVEL_DEBUG,
            "http",
            "rom search variant[%d/%d] game=%s reason=%s term=%s",
            variant_index + 1,
            search_term_count,
            local_item->game_id,
            search_reasons[variant_index],
            search_terms[variant_index]);
      }
    }

    if (search_term_count > 0) {
      for (int term_index = 0; term_index < search_term_count; ++term_index) {
        const char *active_search_term = search_terms[term_index];
        const char *active_reason = search_reasons[term_index];
        if (!sync_string_ieq(active_search_term, last_search_term)) {
          int search_status = fetch_rom_catalog(
              config,
              platform_filter,
              platform_id,
              active_search_term,
              search_catalog,
              ROMM_HTTP_MAX_SEARCH_ROMS,
              body,
              ROMM_HTTP_MAX_BODY_SIZE,
              &last_search_count);
          if (search_status < 0) {
            free(body);
            free(search_catalog);
            return search_status;
          }
          safe_copy(last_search_term, sizeof(last_search_term), active_search_term);
        }

        if (last_search_count <= 0) {
          if (app_log_is_enabled(APP_LOG_LEVEL_DEBUG)) {
            app_log_write(
                APP_LOG_LEVEL_DEBUG,
                "http",
                "rom search variant miss game=%s reason=%s term=%s",
                local_item->game_id,
                active_reason,
                active_search_term);
          }
          continue;
        }

        GameMatcherResolution candidate_resolution;
        int candidate_rom_id = game_matcher_resolve_rom_id_with_details(
            local_item,
            search_catalog,
            last_search_count,
            &candidate_resolution);
        if (candidate_rom_id > 0) {
          resolved_rom_id = candidate_rom_id;
          resolution = candidate_resolution;
          if (app_log_is_enabled(APP_LOG_LEVEL_DEBUG)) {
            app_log_write(
                APP_LOG_LEVEL_DEBUG,
                "http",
                "rom variant selected game=%s reason=%s term=%s rom_id=%d stage=%s/%s score=%d",
                local_item->game_id,
                active_reason,
                active_search_term,
                candidate_rom_id,
                game_matcher_match_stage_str(candidate_resolution.stage),
                game_matcher_match_field_str(candidate_resolution.field),
                candidate_resolution.score);
          }
          break;
        }
        if ((candidate_rom_id == GAME_MATCHER_AMBIGUOUS) &&
            (resolved_rom_id != GAME_MATCHER_AMBIGUOUS)) {
          resolved_rom_id = candidate_rom_id;
          resolution = candidate_resolution;
        }
      }
    } else {
      app_log_write(
          APP_LOG_LEVEL_WARN,
          "http",
          "cannot build RomM search term for game=%s title=%s",
          local_item->game_id,
          local_item->title);
    }

    if (resolved_rom_id > 0) {
      local_item->rom_id = resolved_rom_id;
      resolved_count++;
      if (resolution.score > 0) {
        app_log_write(
            APP_LOG_LEVEL_INFO,
            "http",
            "rom_id mapped game=%s title=%s -> rom_id=%d via %s/%s score=%d",
            local_item->game_id,
            local_item->title,
            resolved_rom_id,
            game_matcher_match_stage_str(resolution.stage),
            game_matcher_match_field_str(resolution.field),
            resolution.score);
      } else {
        app_log_write(
            APP_LOG_LEVEL_INFO,
            "http",
            "rom_id mapped game=%s title=%s -> rom_id=%d via %s/%s",
            local_item->game_id,
            local_item->title,
            resolved_rom_id,
            game_matcher_match_stage_str(resolution.stage),
            game_matcher_match_field_str(resolution.field));
      }
    } else {
      if (resolved_rom_id == GAME_MATCHER_AMBIGUOUS) {
        if (resolution.score > 0) {
          app_log_write(
              APP_LOG_LEVEL_WARN,
              "http",
              "rom_id ambiguous game=%s title=%s at %s/%s score=%d",
              local_item->game_id,
              local_item->title,
              game_matcher_match_stage_str(resolution.stage),
              game_matcher_match_field_str(resolution.field),
              resolution.score);
        } else {
          app_log_write(
              APP_LOG_LEVEL_WARN,
              "http",
              "rom_id ambiguous game=%s title=%s at %s/%s",
              local_item->game_id,
              local_item->title,
              game_matcher_match_stage_str(resolution.stage),
              game_matcher_match_field_str(resolution.field));
        }
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
  free(search_catalog);
  return resolved_count;
}

/*
 * Real RomM save-list callback backed by GET /api/saves.
 */
int romm_http_list_remote_saves_callback(
    void *context,
    const int *rom_ids,
    int rom_id_count,
    SyncSaveDescriptor *out_items,
    int max_items) {
  if ((context == NULL) || (out_items == NULL) || (max_items <= 0) ||
      (rom_id_count < 0) || ((rom_id_count > 0) && (rom_ids == NULL))) {
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }

  const AppConfig *config = (const AppConfig *)context;
  if (!app_config_has_server_url(config) || !app_config_has_auth(config)) {
    return ROMM_CLIENT_ERR_AUTH;
  }

  for (int i = 0; i < rom_id_count; ++i) {
    if (rom_ids[i] <= 0) {
      return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
    }
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
  const char *emulator = romm_save_emulator(config);
  int filter_count = (rom_id_count > 0) ? rom_id_count : 1;
  for (int filter_index = 0; filter_index < filter_count; ++filter_index) {
    int current_rom_id = (rom_id_count > 0) ? rom_ids[filter_index] : 0;
    int total = INT_MAX;
    for (int offset = 0; (offset < total); offset += ROMM_HTTP_PAGE_LIMIT) {
      char path[320];
      if (current_rom_id > 0) {
        snprintf(
            path,
            sizeof(path),
            "/api/saves?limit=%d&offset=%d&emulator=%s&rom_id=%d",
            ROMM_HTTP_PAGE_LIMIT,
            offset,
            emulator,
            current_rom_id);
      } else {
        snprintf(
            path,
            sizeof(path),
            "/api/saves?limit=%d&offset=%d&emulator=%s",
            ROMM_HTTP_PAGE_LIMIT,
            offset,
            emulator);
      }

      char url[APP_CONFIG_MAX_URL_LEN + sizeof(path)];
      if (build_api_url(config->romm_url, path, url, sizeof(url)) < 0) {
        free(body);
        free(page_items);
        return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
      }

      int status_code = 0;
      if (current_rom_id > 0) {
        app_log_write(
            APP_LOG_LEVEL_INFO,
            "http",
            "request GET /api/saves offset=%d limit=%d emulator=%s rom_id=%d",
            offset,
            ROMM_HTTP_PAGE_LIMIT,
            emulator,
            current_rom_id);
      } else {
        app_log_write(
            APP_LOG_LEVEL_INFO,
            "http",
            "request GET /api/saves offset=%d limit=%d emulator=%s",
            offset,
            ROMM_HTTP_PAGE_LIMIT,
            emulator);
      }
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
        char body_preview[ROMM_HTTP_LOG_BODY_PREVIEW];
        build_body_preview(body, body_preview, sizeof(body_preview));
        if (current_rom_id > 0) {
          app_log_write(
              APP_LOG_LEVEL_WARN,
              "http",
              "remote saves fetch failed status=%d offset=%d emulator=%s rom_id=%d body=%s",
              status_code,
              offset,
              emulator,
              current_rom_id,
              body_preview);
        } else {
          app_log_write(
              APP_LOG_LEVEL_WARN,
              "http",
              "remote saves fetch failed status=%d offset=%d emulator=%s body=%s",
              status_code,
              offset,
              emulator,
              body_preview);
        }
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

      if (current_rom_id > 0) {
        app_log_write(
            APP_LOG_LEVEL_INFO,
            "http",
            "response GET /api/saves status=%d offset=%d parsed=%d total_hint=%d unique_kept=%d rom_id=%d",
            status_code,
            offset,
            page_count,
            page_total,
            count,
            current_rom_id);
      } else {
        app_log_write(
            APP_LOG_LEVEL_INFO,
            "http",
            "response GET /api/saves status=%d offset=%d parsed=%d total_hint=%d unique_kept=%d",
            status_code,
            offset,
            page_count,
            page_total,
            count);
      }

      if ((count >= max_items) || (page_count < ROMM_HTTP_PAGE_LIMIT)) {
        break;
      }
    }

    if (count >= max_items) {
      break;
    }
  }

  free(body);
  free(page_items);
  if (rom_id_count > 0) {
    app_log_write(
        APP_LOG_LEVEL_INFO,
        "http",
        "remote saves listed=%d unique entries after filtered pagination/deduplication emulator=%s rom_filters=%d",
        count,
        emulator,
        rom_id_count);
  } else {
    app_log_write(
        APP_LOG_LEVEL_INFO,
        "http",
        "remote saves listed=%d unique entries after pagination/deduplication emulator=%s",
        count,
        emulator);
  }
  return count;
}

/*
 * Real RomM upload callback backed by POST /api/saves for new saves and
 * PUT /api/saves/{id} for matched remote saves. This keeps one current remote
 * save per rom/slot when the sync engine already identified the existing entry.
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

  const char *emulator = romm_save_emulator(config);
  char path[384];
  int http_method = SCE_HTTP_METHOD_POST;
  if (local_item->remote_id > 0) {
    http_method = SCE_HTTP_METHOD_PUT;
    if (has_text(config->device_id)) {
      snprintf(
          path,
          sizeof(path),
          "/api/saves/%d?device_id=%s",
          local_item->remote_id,
          config->device_id);
    } else {
      snprintf(path, sizeof(path), "/api/saves/%d", local_item->remote_id);
    }
  } else {
    const char *slot_query = slot_to_query_value(local_item->slot);
    if (has_text(config->device_id) && has_text(slot_query)) {
      snprintf(
          path,
          sizeof(path),
          "/api/saves?rom_id=%d&emulator=%s&device_id=%s&slot=%s",
          rom_id,
          emulator,
          config->device_id,
          slot_query);
    } else if (has_text(config->device_id)) {
      snprintf(
          path,
          sizeof(path),
          "/api/saves?rom_id=%d&emulator=%s&device_id=%s",
          rom_id,
          emulator,
          config->device_id);
    } else if (has_text(slot_query)) {
      snprintf(
          path,
          sizeof(path),
          "/api/saves?rom_id=%d&emulator=%s&slot=%s",
          rom_id,
          emulator,
          slot_query);
    } else {
      snprintf(
          path,
          sizeof(path),
          "/api/saves?rom_id=%d&emulator=%s",
          rom_id,
          emulator);
    }
  }

  char url[APP_CONFIG_MAX_URL_LEN + sizeof(path)];
  if (build_api_url(config->romm_url, path, url, sizeof(url)) < 0) {
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }

  char upload_filename[ROMM_HTTP_MAX_FILENAME];
  build_upload_filename(local_item, upload_filename, sizeof(upload_filename));

  char response_body[ROMM_HTTP_SMALL_BODY_SIZE];
  int status_code = 0;
  app_log_write(
      APP_LOG_LEVEL_INFO,
      "http",
      "request %s %s rom_id=%d game=%s upload=%s",
      (http_method == SCE_HTTP_METHOD_PUT) ? "PUT" : "POST",
      path,
      rom_id,
      has_text(local_item->game_id) ? local_item->game_id : "(unknown)",
      upload_filename);
  int transport_status = http_send_multipart_file(
      config,
      http_method,
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
      "upload save method=%s status=%d game=%s rom_id=%d remote_id=%d body=%s",
      (http_method == SCE_HTTP_METHOD_PUT) ? "PUT" : "POST",
      status_code,
      local_item->game_id,
      rom_id,
      local_item->remote_id,
      response_body);

  if ((status_code == 401) || (status_code == 403)) {
    return ROMM_CLIENT_ERR_AUTH;
  }
  if (status_code == 409) {
    return ROMM_CLIENT_ERR_CONFLICT;
  }
  if ((status_code == 200) || (status_code == 201) || (status_code == 204)) {
    int remote_save_id = -1;
    char remote_file_name[ROMM_HTTP_MAX_FILENAME];
    remote_file_name[0] = '\0';
    extract_json_int_field(response_body, "id", &remote_save_id);
    extract_json_string_field(response_body, "file_name", remote_file_name, sizeof(remote_file_name));
    if ((remote_save_id > 0) && has_text(remote_file_name)) {
      app_log_write(
          APP_LOG_LEVEL_INFO,
          "http",
          "response POST /api/saves status=%d game=%s rom_id=%d remote_id=%d file=%s",
          status_code,
          has_text(local_item->game_id) ? local_item->game_id : "(unknown)",
          rom_id,
          remote_save_id,
          remote_file_name);
    } else if (remote_save_id > 0) {
      app_log_write(
          APP_LOG_LEVEL_INFO,
          "http",
          "response POST /api/saves status=%d game=%s rom_id=%d remote_id=%d",
          status_code,
          has_text(local_item->game_id) ? local_item->game_id : "(unknown)",
          rom_id,
          remote_save_id);
    } else {
      app_log_write(
          APP_LOG_LEVEL_INFO,
          "http",
          "response POST /api/saves status=%d game=%s rom_id=%d",
          status_code,
          has_text(local_item->game_id) ? local_item->game_id : "(unknown)",
          rom_id);
    }
    return ROMM_CLIENT_OK;
  }

  {
    char body_preview[ROMM_HTTP_LOG_BODY_PREVIEW];
    build_body_preview(response_body, body_preview, sizeof(body_preview));
    app_log_write(
        APP_LOG_LEVEL_WARN,
        "http",
        "upload save failed status=%d game=%s rom_id=%d body=%s",
        status_code,
        has_text(local_item->game_id) ? local_item->game_id : "(unknown)",
        rom_id,
        body_preview);
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
  app_log_write(
      APP_LOG_LEVEL_INFO,
      "http",
      "request GET /api/saves/%d/content device_id=%s optimistic=%s",
      remote_item->remote_id,
      has_device_id ? "yes" : "no",
      has_device_id ? "true" : "false");
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

  int retried_without_device_id = 0;
  if ((status_code == 404) && has_device_id) {
    app_log_write(
        APP_LOG_LEVEL_INFO,
        "http",
        "response GET /api/saves/%d/content status=404 with device_id; retrying without device_id",
        remote_item->remote_id);
    snprintf(path, sizeof(path), "/api/saves/%d/content", remote_item->remote_id);
    if (build_api_url(config->romm_url, path, url, sizeof(url)) < 0) {
      return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
    }

    app_log_write(
        APP_LOG_LEVEL_INFO,
        "http",
        "request GET /api/saves/%d/content retry_without_device_id=1",
        remote_item->remote_id);
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
    retried_without_device_id = 1;
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
    app_log_write(
        APP_LOG_LEVEL_INFO,
        "http",
        "response GET /api/saves/%d/content status=200 file=%s retried_without_device_id=%d",
        remote_item->remote_id,
        has_text(remote_item->filename) ? remote_item->filename : "(unknown)",
        retried_without_device_id);
    return ROMM_CLIENT_OK;
  }

  {
    char body_preview[ROMM_HTTP_LOG_BODY_PREVIEW];
    build_body_preview(response_body, body_preview, sizeof(body_preview));
    app_log_write(
        APP_LOG_LEVEL_WARN,
        "http",
        "download save failed status=%d remote_id=%d body=%s",
        status_code,
        remote_item->remote_id,
        body_preview);
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
  app_log_write(
      APP_LOG_LEVEL_INFO,
      "http",
      "request POST /api/devices name=%s platform=%s client=%s version=%s",
      device_name,
      device_platform,
      client_name,
      client_version);
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
    char body_preview[ROMM_HTTP_LOG_BODY_PREVIEW];
    build_body_preview(response_body, body_preview, sizeof(body_preview));
    app_log_write(
        APP_LOG_LEVEL_WARN,
        "http",
        "register_device failed status=%d body=%s",
        http_status_code,
        body_preview);
    return ROMM_CLIENT_ERR_NETWORK;
  }

  if (extract_json_string_field(response_body, "device_id", out_device_id, out_device_id_size) == 0) {
    app_log_write(
        APP_LOG_LEVEL_INFO,
        "http",
        "response POST /api/devices status=%d device_id=%s",
        http_status_code,
        out_device_id);
    return ROMM_CLIENT_OK;
  }
  if (extract_json_scalar_field(response_body, "device_id", out_device_id, out_device_id_size) == 0) {
    app_log_write(
        APP_LOG_LEVEL_INFO,
        "http",
        "response POST /api/devices status=%d device_id=%s",
        http_status_code,
        out_device_id);
    return ROMM_CLIENT_OK;
  }
  if (extract_json_string_field(response_body, "id", out_device_id, out_device_id_size) == 0) {
    app_log_write(
        APP_LOG_LEVEL_INFO,
        "http",
        "response POST /api/devices status=%d device_id=%s",
        http_status_code,
        out_device_id);
    return ROMM_CLIENT_OK;
  }
  if (extract_json_scalar_field(response_body, "id", out_device_id, out_device_id_size) == 0) {
    app_log_write(
        APP_LOG_LEVEL_INFO,
        "http",
        "response POST /api/devices status=%d device_id=%s",
        http_status_code,
        out_device_id);
    return ROMM_CLIENT_OK;
  }

  app_log_write(APP_LOG_LEVEL_ERROR, "http", "register_device response missing device_id");
  return ROMM_CLIENT_ERR_NETWORK;
}
