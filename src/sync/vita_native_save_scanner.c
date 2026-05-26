#include "vita_native_save_scanner.h"

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/sysmodule.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_log.h"
#include "sfo_parser.h"
#include "sync_types.h"
#include "vita_native_save_policy.h"

#define VITA_NATIVE_MAX_DEPTH 8
#define VITA_TAR_BLOCK_SIZE 512
#define VITA_TAR_NAME_FIELD_SIZE 100
#define VITA_NATIVE_ARCHIVE_README_NAME "00-README-romm-vita-sync.txt"
#define VITA_NATIVE_ARCHIVE_README_MAX 1536
#define VITA_NATIVE_FILE_MODE (SCE_S_IRUSR | SCE_S_IWUSR | SCE_S_IRSYS | SCE_S_IWSYS)
#define VITA_NATIVE_DIRECTORY_MODE (SCE_S_IRWXU | SCE_S_IRWXS)
#define SQLITE_OK 0
#define SQLITE_ROW 100
#define SQLITE_DONE 101
#define SQLITE_OPEN_READONLY 0x00000001

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;

extern int sqlite3_open_v2(const char *filename, sqlite3 **ppDb, int flags, const char *zVfs);
extern int sqlite3_close(sqlite3 *db);
extern int sqlite3_prepare_v2(sqlite3 *db, const char *zSql, int nByte, sqlite3_stmt **ppStmt, const char **pzTail);
extern int sqlite3_bind_text(sqlite3_stmt *stmt, int index, const char *value, int n, void (*destructor)(void *));
extern int sqlite3_step(sqlite3_stmt *stmt);
extern const unsigned char *sqlite3_column_text(sqlite3_stmt *stmt, int column);
extern int sqlite3_finalize(sqlite3_stmt *stmt);

/*
 * Native savedata PARAM.SFO files can be sparse, so resolve a TITLE_ID through
 * LiveArea's app.db and installed app/appmeta metadata before falling back to
 * the save container.
 */
static const char *const VITA_NATIVE_METADATA_PREFIXES[] = {
    "ux0:",
    "ux0:/",
    "ur0:",
    "ur0:/",
    "uma0:",
    "uma0:/",
    "imc0:",
    "imc0:/",
    "xmc0:",
    "xmc0:/",
    "grw0:",
    "grw0:/",
    "gro0:",
    "gro0:/",
};

static const char *const VITA_NATIVE_APP_DB_PATHS[] = {
    "ur0:shell/db/app.db",
    "ur0:/shell/db/app.db",
    "ux0:shell/db/app.db",
    "ux0:/shell/db/app.db",
};

static const char *const VITA_NATIVE_APP_TITLE_KEYS[] = {
    "TITLE",
    "STITLE",
    "TITLE_00",
    "TITLE_01",
    "TITLE_02",
    "TITLE_03",
    "TITLE_04",
    "TITLE_05",
    "TITLE_06",
    "TITLE_07",
    "TITLE_08",
    "TITLE_09",
    "TITLE_10",
    "TITLE_11",
    "TITLE_12",
    "TITLE_13",
    "TITLE_14",
    "TITLE_15",
    "TITLE_16",
    "TITLE_17",
};

static const char *const VITA_NATIVE_SAVEDATA_TITLE_KEYS[] = {
    "TITLE",
    "STITLE",
    "SAVEDATA_TITLE",
    "SUB_TITLE",
    "DETAIL",
    "TITLE_00",
    "TITLE_01",
    "TITLE_02",
    "TITLE_03",
    "TITLE_04",
    "TITLE_05",
    "TITLE_06",
    "TITLE_07",
    "TITLE_08",
    "TITLE_09",
    "TITLE_10",
    "TITLE_11",
    "TITLE_12",
    "TITLE_13",
    "TITLE_14",
    "TITLE_15",
    "TITLE_16",
    "TITLE_17",
};

static int has_text_local(const char *value) {
  return (value != NULL) && (value[0] != '\0');
}

static int has_visible_text(const char *value) {
  if (value == NULL) {
    return 0;
  }

  while (*value != '\0') {
    if ((unsigned char)*value > ' ') {
      return 1;
    }
    value++;
  }
  return 0;
}

static int is_same_text_case_insensitive(const char *lhs, const char *rhs) {
  if ((lhs == NULL) || (rhs == NULL)) {
    return 0;
  }

  while ((*lhs != '\0') && (*rhs != '\0')) {
    unsigned char lc = (unsigned char)*lhs;
    unsigned char rc = (unsigned char)*rhs;
    if ((lc >= 'A') && (lc <= 'Z')) {
      lc = (unsigned char)(lc + ('a' - 'A'));
    }
    if ((rc >= 'A') && (rc <= 'Z')) {
      rc = (unsigned char)(rc + ('a' - 'A'));
    }
    if (lc != rc) {
      return 0;
    }
    lhs++;
    rhs++;
  }

  return (*lhs == '\0') && (*rhs == '\0');
}

static int is_title_id_value(const char *value, const char *title_id) {
  return has_text_local(value) && has_text_local(title_id) &&
         is_same_text_case_insensitive(value, title_id);
}

static void vita_scan_log(int verbose, AppLogLevel level, const char *format, ...) {
  if (!verbose || format == NULL) {
    return;
  }

  char message[256];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  app_log_write(level, "vita-native", "%s", message);
}

static int path_exists(const char *path) {
  SceIoStat st;
  memset(&st, 0, sizeof(st));
  return has_text_local(path) && (sceIoGetstat(path, &st) >= 0);
}

static int path_is_directory_local(const char *path) {
  SceUID dfd;
  if (!has_text_local(path)) {
    return 0;
  }
  dfd = sceIoDopen(path);
  if (dfd < 0) {
    return 0;
  }
  sceIoDclose(dfd);
  return 1;
}

static int path_is_directory_stat(const char *path) {
  SceIoStat st;
  if (!has_text_local(path)) {
    return 0;
  }
  memset(&st, 0, sizeof(st));
  if (sceIoGetstat(path, &st) < 0) {
    return 0;
  }
  return SCE_S_ISDIR(st.st_mode) || SCE_SO_ISDIR(st.st_attr);
}

static int is_path_separator(char c) {
  return (c == '/') || (c == '\\');
}

static int is_device_root_path(const char *path) {
  if (!has_text_local(path)) {
    return 0;
  }

  const char *colon = strchr(path, ':');
  if (colon == NULL) {
    return 0;
  }
  if (colon[1] == '\0') {
    return 1;
  }
  return (is_path_separator(colon[1]) && (colon[2] == '\0'));
}

static int create_vita_directory_one(const char *path) {
  if (!has_text_local(path) || is_device_root_path(path)) {
    return 0;
  }
  if (path_is_directory_stat(path)) {
    return 0;
  }

  int status = sceIoMkdir(path, VITA_NATIVE_DIRECTORY_MODE);
  if (status >= 0 || path_is_directory_stat(path)) {
    return 0;
  }
  return status;
}

static int ensure_vita_directory_tree(const char *directory_path) {
  if (!has_text_local(directory_path)) {
    return -1;
  }

  char path_copy[ROMM_MAX_PATH_LEN];
  size_t length = strlen(directory_path);
  if (length >= sizeof(path_copy)) {
    return -1;
  }
  memcpy(path_copy, directory_path, length + 1U);

  for (size_t i = 0U; i < length; ++i) {
    if (!is_path_separator(path_copy[i])) {
      continue;
    }
    if (i == 0U) {
      continue;
    }

    char previous = path_copy[i];
    path_copy[i] = '\0';
    int status = create_vita_directory_one(path_copy);
    path_copy[i] = previous;
    if (status < 0) {
      return status;
    }
  }

  return create_vita_directory_one(path_copy);
}

static int copy_truncated(char *out, size_t out_size, const char *value) {
  if ((out == NULL) || (out_size == 0U)) {
    return -1;
  }
  out[0] = '\0';
  if (value == NULL) {
    return -1;
  }
  size_t value_len = strlen(value);
  size_t copy_len = (value_len < (out_size - 1U)) ? value_len : (out_size - 1U);
  memcpy(out, value, copy_len);
  out[copy_len] = '\0';
  return (value_len < out_size) ? 0 : -1;
}

static int ensure_sqlite_loaded(int verbose) {
  static int attempted = 0;
  static int available = 0;

  if (available) {
    return 0;
  }
  if (attempted) {
    return -1;
  }
  attempted = 1;

  int status = sceSysmoduleIsLoaded(SCE_SYSMODULE_SQLITE);
  if (status < 0) {
    status = sceSysmoduleLoadModule(SCE_SYSMODULE_SQLITE);
  }
  if (status < 0) {
    vita_scan_log(verbose, APP_LOG_LEVEL_WARN, "SQLite module unavailable for Vita app.db lookup status=0x%08X", (unsigned int)status);
    return -1;
  }

  available = 1;
  return 0;
}

static int open_vita_app_db(int verbose, sqlite3 **out_db) {
  if (out_db == NULL) {
    return -1;
  }
  *out_db = NULL;

  if (ensure_sqlite_loaded(verbose) < 0) {
    return -1;
  }

  for (size_t i = 0; i < (sizeof(VITA_NATIVE_APP_DB_PATHS) / sizeof(VITA_NATIVE_APP_DB_PATHS[0])); ++i) {
    const char *db_path = VITA_NATIVE_APP_DB_PATHS[i];
    if (!path_exists(db_path)) {
      continue;
    }

    sqlite3 *db = NULL;
    int status = sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL);
    if ((status == SQLITE_OK) && (db != NULL)) {
      *out_db = db;
      return 0;
    }
    if (db != NULL) {
      sqlite3_close(db);
    }
    vita_scan_log(verbose, APP_LOG_LEVEL_WARN, "app.db open failed status=%d path=%s", status, db_path);
  }

  return -1;
}

static int copy_known_vita_title(const char *title_id, char *out_title, size_t out_title_size) {
  static const struct {
    const char *title_id;
    const char *title;
  } known_titles[] = {
      {"PCSF00012", "Uncharted: Golden Abyss"},
  };

  if (!has_text_local(title_id) || (out_title == NULL) || (out_title_size == 0U)) {
    return -1;
  }

  for (size_t i = 0; i < (sizeof(known_titles) / sizeof(known_titles[0])); ++i) {
    if (is_same_text_case_insensitive(title_id, known_titles[i].title_id)) {
      copy_truncated(out_title, out_title_size, known_titles[i].title);
      return 0;
    }
  }

  return -1;
}

static int app_db_query_text(
    sqlite3 *db,
    const char *sql,
    const char *title_id,
    const char *key,
    char *out_title,
    size_t out_title_size) {
  if ((db == NULL) || !has_text_local(sql) || !has_text_local(title_id) ||
      (out_title == NULL) || (out_title_size == 0U)) {
    return -1;
  }

  sqlite3_stmt *stmt = NULL;
  int status = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if ((status != SQLITE_OK) || (stmt == NULL)) {
    return -1;
  }

  status = sqlite3_bind_text(stmt, 1, title_id, -1, NULL);
  if ((status == SQLITE_OK) && (key != NULL)) {
    status = sqlite3_bind_text(stmt, 2, key, -1, NULL);
  }
  if (status != SQLITE_OK) {
    sqlite3_finalize(stmt);
    return -1;
  }

  int result = -1;
  status = sqlite3_step(stmt);
  if (status == SQLITE_ROW) {
    const unsigned char *text = sqlite3_column_text(stmt, 0);
    if ((text != NULL) && has_visible_text((const char *)text) &&
        !is_title_id_value((const char *)text, title_id)) {
      copy_truncated(out_title, out_title_size, (const char *)text);
      result = 0;
    }
  } else if (status != SQLITE_DONE) {
    result = -1;
  }

  sqlite3_finalize(stmt);
  return result;
}

static int populate_vita_title_from_app_db(
    const char *title_id,
    int verbose,
    char *out_title,
    size_t out_title_size) {
  if (!has_text_local(title_id) || (out_title == NULL) || (out_title_size == 0U)) {
    return -1;
  }

  sqlite3 *db = NULL;
  if (open_vita_app_db(verbose, &db) < 0) {
    return -1;
  }

  static const char *const key_value_sql[] = {
      "SELECT val FROM tbl_appinfo WHERE titleId COLLATE NOCASE = ? AND key COLLATE NOCASE = ? LIMIT 1",
      "SELECT val FROM tbl_appinfo WHERE titleid COLLATE NOCASE = ? AND key COLLATE NOCASE = ? LIMIT 1",
      "SELECT val FROM tbl_appinfo WHERE title_id COLLATE NOCASE = ? AND key COLLATE NOCASE = ? LIMIT 1",
  };
  for (size_t sql_index = 0; sql_index < (sizeof(key_value_sql) / sizeof(key_value_sql[0])); ++sql_index) {
    for (size_t key_index = 0; key_index < (sizeof(VITA_NATIVE_APP_TITLE_KEYS) / sizeof(VITA_NATIVE_APP_TITLE_KEYS[0])); ++key_index) {
      if (app_db_query_text(
              db,
              key_value_sql[sql_index],
              title_id,
              VITA_NATIVE_APP_TITLE_KEYS[key_index],
              out_title,
              out_title_size) == 0) {
        sqlite3_close(db);
        vita_scan_log(verbose, APP_LOG_LEVEL_INFO, "resolved Vita title from app.db title_id=%s title=%s", title_id, out_title);
        return 0;
      }
    }
  }

  static const char *const fallback_sql[] = {
      "SELECT title FROM tbl_appinfo WHERE titleId COLLATE NOCASE = ? LIMIT 1",
      "SELECT title FROM tbl_appinfo WHERE titleid COLLATE NOCASE = ? LIMIT 1",
      "SELECT title FROM tbl_appinfo WHERE title_id COLLATE NOCASE = ? LIMIT 1",
      "SELECT name FROM tbl_appinfo WHERE titleId COLLATE NOCASE = ? LIMIT 1",
      "SELECT name FROM tbl_appinfo WHERE titleid COLLATE NOCASE = ? LIMIT 1",
      "SELECT name FROM tbl_appinfo WHERE title_id COLLATE NOCASE = ? LIMIT 1",
      "SELECT val FROM tbl_appinfo WHERE titleId COLLATE NOCASE = ? AND key COLLATE NOCASE LIKE 'TITLE\\_%' ESCAPE '\\' LIMIT 1",
      "SELECT val FROM tbl_appinfo WHERE titleid COLLATE NOCASE = ? AND key COLLATE NOCASE LIKE 'TITLE\\_%' ESCAPE '\\' LIMIT 1",
      "SELECT val FROM tbl_appinfo WHERE title_id COLLATE NOCASE = ? AND key COLLATE NOCASE LIKE 'TITLE\\_%' ESCAPE '\\' LIMIT 1",
      "SELECT title.val FROM tbl_appinfo AS title JOIN tbl_appinfo AS savedata ON title.titleId = savedata.titleId WHERE title.key COLLATE NOCASE = 'TITLE' AND savedata.key COLLATE NOCASE = 'INSTALL_DIR_SAVEDATA' AND savedata.val LIKE '%' || ? || '%' LIMIT 1",
      "SELECT title.val FROM tbl_appinfo AS title JOIN tbl_appinfo AS savedata ON title.titleid = savedata.titleid WHERE title.key COLLATE NOCASE = 'TITLE' AND savedata.key COLLATE NOCASE = 'INSTALL_DIR_SAVEDATA' AND savedata.val LIKE '%' || ? || '%' LIMIT 1",
  };
  for (size_t i = 0; i < (sizeof(fallback_sql) / sizeof(fallback_sql[0])); ++i) {
    if (app_db_query_text(db, fallback_sql[i], title_id, NULL, out_title, out_title_size) == 0) {
      sqlite3_close(db);
      vita_scan_log(verbose, APP_LOG_LEVEL_INFO, "resolved Vita title from app.db title_id=%s title=%s", title_id, out_title);
      return 0;
    }
  }

  sqlite3_close(db);
  return -1;
}

static int join_path_suffix(const char *base, const char *suffix, char *out, size_t out_size) {
  if (!has_text_local(base) || !has_text_local(suffix) || (out == NULL) || (out_size == 0U)) {
    return -1;
  }
  size_t base_len = strlen(base);
  size_t suffix_len = strlen(suffix);
  if ((base_len + suffix_len + 1U) > out_size) {
    out[0] = '\0';
    return -1;
  }
  memcpy(out, base, base_len);
  memcpy(out + base_len, suffix, suffix_len);
  out[base_len + suffix_len] = '\0';
  return 0;
}

static int read_first_sfo_text_key(
    const char *sfo_path,
    const char *const *keys,
    size_t key_count,
    const char *title_id,
    char *out_value,
    size_t out_size) {
  if (!has_text_local(sfo_path) || (keys == NULL) || (out_value == NULL) || (out_size == 0U)) {
    return -1;
  }

  out_value[0] = '\0';
  for (size_t i = 0; i < key_count; ++i) {
    char candidate[ROMM_GAME_TITLE_LEN];
    candidate[0] = '\0';
    if (has_text_local(keys[i]) &&
        sfo_read_key(sfo_path, keys[i], candidate, sizeof(candidate)) == 0 &&
        has_visible_text(candidate) &&
        !is_title_id_value(candidate, title_id)) {
      copy_truncated(out_value, out_size, candidate);
      return 0;
    }
  }

  return -1;
}

static int read_vita_title_from_sfo(
    const char *sfo_path,
    const char *title_id,
    char *out_title,
    size_t out_title_size) {
  return read_first_sfo_text_key(
      sfo_path,
      VITA_NATIVE_APP_TITLE_KEYS,
      sizeof(VITA_NATIVE_APP_TITLE_KEYS) / sizeof(VITA_NATIVE_APP_TITLE_KEYS[0]),
      title_id,
      out_title,
      out_title_size);
}

static int try_vita_title_sfo_path(
    const char *path_format,
    const char *prefix,
    const char *title_id,
    char *out_title,
    size_t out_title_size) {
  if (!has_text_local(path_format) || !has_text_local(prefix) || !has_text_local(title_id) ||
      (out_title == NULL) || (out_title_size == 0U)) {
    return -1;
  }

  char sfo_path[ROMM_MAX_PATH_LEN];
  int wrote = snprintf(sfo_path, sizeof(sfo_path), path_format, prefix, title_id);
  if ((wrote < 0) || ((size_t)wrote >= sizeof(sfo_path))) {
    return -1;
  }

  return read_vita_title_from_sfo(sfo_path, title_id, out_title, out_title_size);
}

static int populate_vita_title_from_installed_app(
    const char *title_id,
    char *out_title,
    size_t out_title_size) {
  if (!has_text_local(title_id) || (out_title == NULL) || (out_title_size == 0U)) {
    return -1;
  }

  static const char *const app_sfo_formats[] = {
      "%sapp/%s/sce_sys/param.sfo",
      "%sapp/%s/sce_sys/PARAM.SFO",
      "%spatch/%s/sce_sys/param.sfo",
      "%spatch/%s/sce_sys/PARAM.SFO",
      "%sappmeta/%s/param.sfo",
      "%sappmeta/%s/PARAM.SFO",
      "%sappmeta/%s/sce_sys/param.sfo",
      "%sappmeta/%s/sce_sys/PARAM.SFO",
  };

  for (size_t prefix_index = 0;
       prefix_index < (sizeof(VITA_NATIVE_METADATA_PREFIXES) / sizeof(VITA_NATIVE_METADATA_PREFIXES[0]));
       ++prefix_index) {
    for (size_t format_index = 0;
         format_index < (sizeof(app_sfo_formats) / sizeof(app_sfo_formats[0]));
         ++format_index) {
      if (try_vita_title_sfo_path(
              app_sfo_formats[format_index],
              VITA_NATIVE_METADATA_PREFIXES[prefix_index],
              title_id,
              out_title,
              out_title_size) == 0) {
        return 0;
      }
    }
  }

  return -1;
}

static void write_two_digits(char *out, unsigned value) {
  value %= 100U;
  out[0] = (char)('0' + (value / 10U));
  out[1] = (char)('0' + (value % 10U));
}

static void write_four_digits(char *out, unsigned value) {
  value %= 10000U;
  out[0] = (char)('0' + ((value / 1000U) % 10U));
  out[1] = (char)('0' + ((value / 100U) % 10U));
  out[2] = (char)('0' + ((value / 10U) % 10U));
  out[3] = (char)('0' + (value % 10U));
}

static void format_vita_timestamp(const SceDateTime *dt, char *out, size_t out_size) {
  if ((dt == NULL) || (out == NULL) || (out_size < ROMM_TIMESTAMP_LEN)) {
    return;
  }
  write_four_digits(out, (unsigned)dt->year);
  out[4] = '-';
  write_two_digits(out + 5, (unsigned)dt->month);
  out[7] = '-';
  write_two_digits(out + 8, (unsigned)dt->day);
  out[10] = ' ';
  write_two_digits(out + 11, (unsigned)dt->hour);
  out[13] = ':';
  write_two_digits(out + 14, (unsigned)dt->minute);
  out[16] = ':';
  write_two_digits(out + 17, (unsigned)dt->second);
  out[19] = '\0';
}

static int recursive_container_stats(
    const char *path,
    int depth,
    uint64_t *in_out_size,
    int64_t *in_out_latest_timestamp) {
  if (!has_text_local(path) || (in_out_size == NULL) || (in_out_latest_timestamp == NULL) ||
      (depth > VITA_NATIVE_MAX_DEPTH)) {
    return -1;
  }

  SceUID dfd = sceIoDopen(path);
  if (dfd < 0) {
    return dfd;
  }

  SceIoDirent entry;
  for (;;) {
    memset(&entry, 0, sizeof(entry));
    int read = sceIoDread(dfd, &entry);
    if (read <= 0) {
      break;
    }
    if ((strcmp(entry.d_name, ".") == 0) || (strcmp(entry.d_name, "..") == 0)) {
      continue;
    }

    char child[ROMM_MAX_PATH_LEN];
    if (join_path_suffix(path, "/", child, sizeof(child)) < 0 ||
        join_path_suffix(child, entry.d_name, child, sizeof(child)) < 0) {
      continue;
    }

    if (path_is_directory_local(child)) {
      recursive_container_stats(child, depth + 1, in_out_size, in_out_latest_timestamp);
      continue;
    }

    *in_out_size += (uint64_t)entry.d_stat.st_size;
    char timestamp_text[ROMM_TIMESTAMP_LEN];
    int64_t timestamp_unix = 0;
    format_vita_timestamp(&entry.d_stat.st_mtime, timestamp_text, sizeof(timestamp_text));
    if (sync_parse_local_timestamp(timestamp_text, &timestamp_unix) == 0 &&
        timestamp_unix > *in_out_latest_timestamp) {
      *in_out_latest_timestamp = timestamp_unix;
    }
  }

  sceIoDclose(dfd);
  return 0;
}

/*
 * Validates that a candidate Vita native savedata directory has the metadata
 * needed for an export-only backup. The policy requires keystone because it is
 * part of Vita PFS/signature recognition; candidates without it are skipped
 * rather than uploaded as server objects that cannot be restored safely.
 */
static int validate_vita_container(
    const char *container_path,
    int *out_has_sce_sys,
    int *out_has_keystone,
    int *out_has_param_sfo,
    char *out_reason,
    size_t out_reason_size) {
  if (!has_text_local(container_path)) {
    return 0;
  }

  char sce_sys[ROMM_MAX_PATH_LEN];
  char keystone[ROMM_MAX_PATH_LEN];
  char param_lower[ROMM_MAX_PATH_LEN];
  char param_upper[ROMM_MAX_PATH_LEN];
  if ((join_path_suffix(container_path, "/sce_sys", sce_sys, sizeof(sce_sys)) < 0) ||
      (join_path_suffix(container_path, "/sce_sys/keystone", keystone, sizeof(keystone)) < 0) ||
      (join_path_suffix(container_path, "/sce_sys/param.sfo", param_lower, sizeof(param_lower)) < 0) ||
      (join_path_suffix(container_path, "/sce_sys/PARAM.SFO", param_upper, sizeof(param_upper)) < 0)) {
    return 0;
  }

  int has_sce_sys = path_is_directory_local(sce_sys);
  int has_keystone = path_exists(keystone);
  int has_param_sfo = path_exists(param_lower) || path_exists(param_upper);

  if (out_has_sce_sys != NULL) {
    *out_has_sce_sys = has_sce_sys;
  }
  if (out_has_keystone != NULL) {
    *out_has_keystone = has_keystone;
  }
  if (out_has_param_sfo != NULL) {
    *out_has_param_sfo = has_param_sfo;
  }

  return vita_native_save_container_is_exportable(
      has_sce_sys,
      has_keystone,
      has_param_sfo,
      out_reason,
      out_reason_size);
}

static void populate_vita_title(
    const char *container_path,
    const char *title_id,
    int verbose,
    char *out_title,
    size_t out_title_size) {
  if ((out_title == NULL) || (out_title_size == 0U)) {
    return;
  }
  out_title[0] = '\0';

  if (populate_vita_title_from_app_db(title_id, verbose, out_title, out_title_size) == 0) {
    return;
  }

  if (populate_vita_title_from_installed_app(title_id, out_title, out_title_size) == 0) {
    return;
  }

  if (!has_text_local(container_path)) {
    return;
  }

  char param_path[ROMM_MAX_PATH_LEN];
  if (join_path_suffix(container_path, "/sce_sys/param.sfo", param_path, sizeof(param_path)) == 0 &&
      read_first_sfo_text_key(
          param_path,
          VITA_NATIVE_SAVEDATA_TITLE_KEYS,
          sizeof(VITA_NATIVE_SAVEDATA_TITLE_KEYS) / sizeof(VITA_NATIVE_SAVEDATA_TITLE_KEYS[0]),
          title_id,
          out_title,
          out_title_size) == 0) {
    return;
  }
  if (join_path_suffix(container_path, "/sce_sys/PARAM.SFO", param_path, sizeof(param_path)) == 0) {
    if (read_first_sfo_text_key(
        param_path,
        VITA_NATIVE_SAVEDATA_TITLE_KEYS,
        sizeof(VITA_NATIVE_SAVEDATA_TITLE_KEYS) / sizeof(VITA_NATIVE_SAVEDATA_TITLE_KEYS[0]),
        title_id,
        out_title,
        out_title_size) == 0) {
      return;
    }
  }

  if (copy_known_vita_title(title_id, out_title, out_title_size) == 0) {
    vita_scan_log(verbose, APP_LOG_LEVEL_INFO, "resolved Vita title from built-in fallback title_id=%s title=%s", title_id, out_title);
  }
}

int vita_native_scan_save_containers(
    const char *root,
    int verbose,
    SyncSaveDescriptor *out_items,
    int max_items) {
  if ((out_items == NULL) || (max_items <= 0)) {
    return -1;
  }

  const char *scan_root = has_text_local(root) ? root : VITA_NATIVE_SAVEDATA_ROOT;
  vita_scan_log(verbose, APP_LOG_LEVEL_INFO, "capability detection: probing %s", scan_root);
  vita_scan_log(verbose, APP_LOG_LEVEL_INFO, "%s", vita_native_save_restore_safety_notice());

  SceUID dfd = sceIoDopen(scan_root);
  if (dfd < 0) {
    vita_scan_log(verbose, APP_LOG_LEVEL_WARN, "Vita native savedata root inaccessible status=%d path=%s", (int)dfd, scan_root);
    return 0;
  }

  int count = 0;
  SceIoDirent entry;
  for (;;) {
    memset(&entry, 0, sizeof(entry));
    int read = sceIoDread(dfd, &entry);
    if (read <= 0) {
      break;
    }
    if ((strcmp(entry.d_name, ".") == 0) || (strcmp(entry.d_name, "..") == 0)) {
      continue;
    }
    if (!vita_native_save_title_id_is_official_game(entry.d_name)) {
      vita_scan_log(
          verbose,
          APP_LOG_LEVEL_INFO,
          "skipped non-official Vita savedata container title_id=%s",
          entry.d_name);
      continue;
    }

    char container_path[ROMM_MAX_PATH_LEN];
    if (join_path_suffix(scan_root, "/", container_path, sizeof(container_path)) < 0 ||
        join_path_suffix(container_path, entry.d_name, container_path, sizeof(container_path)) < 0 ||
        !path_is_directory_local(container_path)) {
      continue;
    }

    int has_sce_sys = 0;
    int has_keystone = 0;
    int has_param_sfo = 0;
    char policy_reason[128];
    policy_reason[0] = '\0';
    if (!validate_vita_container(container_path, &has_sce_sys, &has_keystone, &has_param_sfo, policy_reason, sizeof(policy_reason))) {
      vita_scan_log(verbose, APP_LOG_LEVEL_WARN, "skipped Vita container candidate title_id=%s sce_sys=%d keystone=%d param_sfo=%d reason=%s", entry.d_name, has_sce_sys, has_keystone, has_param_sfo, has_text_local(policy_reason) ? policy_reason : "not exportable");
      continue;
    }
    if (count >= max_items) {
      break;
    }

    SyncSaveDescriptor *item = &out_items[count];
    sync_save_descriptor_init(item);
    item->platform = SYNC_SAVE_PLATFORM_VITA_NATIVE_EXPERIMENTAL;
    item->slot = SYNC_SLOT_UNKNOWN;
    copy_truncated(item->game_id, sizeof(item->game_id), entry.d_name);
    copy_truncated(item->path, sizeof(item->path), container_path);
    copy_truncated(item->filename, sizeof(item->filename), entry.d_name);
    populate_vita_title(container_path, entry.d_name, verbose, item->title, sizeof(item->title));

    uint64_t total_size = 0U;
    int64_t latest_timestamp = 0;
    recursive_container_stats(container_path, 0, &total_size, &latest_timestamp);
    item->size_bytes = total_size;
    item->timestamp_unix = latest_timestamp;
    sync_format_timestamp(latest_timestamp, item->timestamp_text, sizeof(item->timestamp_text));

    vita_scan_log(verbose, APP_LOG_LEVEL_INFO, "detected Vita save container title_id=%s title=%s size=%llu sce_sys=%d keystone=%d platform=%s policy=%s", item->game_id, has_text_local(item->title) ? item->title : "(unknown)", (unsigned long long)item->size_bytes, has_sce_sys, has_keystone, sync_save_platform_id(item->platform), policy_reason);
    count += 1;
  }

  sceIoDclose(dfd);
  return count;
}

static void tar_write_octal(char *field, size_t field_size, unsigned long long value) {
  if ((field == NULL) || (field_size == 0U)) {
    return;
  }
  snprintf(field, field_size, "%0*llo", (int)field_size - 1, value);
}

static int tar_write_zero_block(SceUID fd) {
  char block[VITA_TAR_BLOCK_SIZE];
  memset(block, 0, sizeof(block));
  return (sceIoWrite(fd, block, sizeof(block)) == (int)sizeof(block)) ? 0 : -1;
}

static int tar_write_header(SceUID fd, const char *name, uint64_t size, int is_dir) {
  if (!has_text_local(name) || (strlen(name) >= VITA_TAR_NAME_FIELD_SIZE)) {
    return -1;
  }

  char header[VITA_TAR_BLOCK_SIZE];
  memset(header, 0, sizeof(header));
  snprintf(header, VITA_TAR_NAME_FIELD_SIZE, "%s", name);
  tar_write_octal(header + 100, 8, is_dir ? 0755ULL : 0644ULL);
  tar_write_octal(header + 108, 8, 0ULL);
  tar_write_octal(header + 116, 8, 0ULL);
  tar_write_octal(header + 124, 12, is_dir ? 0ULL : (unsigned long long)size);
  tar_write_octal(header + 136, 12, 0ULL);
  memset(header + 148, ' ', 8);
  header[156] = is_dir ? '5' : '0';
  memcpy(header + 257, "ustar", 5);
  memcpy(header + 263, "00", 2);

  unsigned int checksum = 0U;
  for (int i = 0; i < VITA_TAR_BLOCK_SIZE; ++i) {
    checksum += (unsigned char)header[i];
  }
  snprintf(header + 148, 8, "%06o", checksum);
  header[154] = '\0';
  header[155] = ' ';

  return (sceIoWrite(fd, header, sizeof(header)) == (int)sizeof(header)) ? 0 : -1;
}

static int tar_write_file_padding(SceUID fd, uint64_t size) {
  uint64_t remainder = size % VITA_TAR_BLOCK_SIZE;
  if (remainder == 0U) {
    return 0;
  }

  char padding[VITA_TAR_BLOCK_SIZE];
  int padding_size = (int)(VITA_TAR_BLOCK_SIZE - remainder);
  memset(padding, 0, sizeof(padding));
  return (sceIoWrite(fd, padding, padding_size) == padding_size) ? 0 : -1;
}

static int build_child_path(
    const char *parent,
    const char *child,
    char *out_path,
    size_t out_path_size);

static int tar_add_path(SceUID tar_fd, const char *filesystem_path, const char *archive_name, int depth) {
  if (!has_text_local(filesystem_path) || !has_text_local(archive_name) || (depth > VITA_NATIVE_MAX_DEPTH)) {
    return -1;
  }

  if (path_is_directory_local(filesystem_path)) {
    char dir_name[ROMM_MAX_PATH_LEN];
    snprintf(dir_name, sizeof(dir_name), "%s/", archive_name);
    if (tar_write_header(tar_fd, dir_name, 0U, 1) < 0) {
      return -1;
    }

    SceUID dfd = sceIoDopen(filesystem_path);
    if (dfd < 0) {
      return dfd;
    }
    SceIoDirent entry;
    int read = 0;
    for (;;) {
      memset(&entry, 0, sizeof(entry));
      read = sceIoDread(dfd, &entry);
      if (read <= 0) {
        break;
      }
      if ((strcmp(entry.d_name, ".") == 0) || (strcmp(entry.d_name, "..") == 0)) {
        continue;
      }
      char child_fs[ROMM_MAX_PATH_LEN];
      char child_archive[ROMM_MAX_PATH_LEN];
      if ((build_child_path(filesystem_path, entry.d_name, child_fs, sizeof(child_fs)) < 0) ||
          (build_child_path(archive_name, entry.d_name, child_archive, sizeof(child_archive)) < 0)) {
        sceIoDclose(dfd);
        return -1;
      }
      int child_status = tar_add_path(tar_fd, child_fs, child_archive, depth + 1);
      if (child_status < 0) {
        sceIoDclose(dfd);
        return child_status;
      }
    }
    sceIoDclose(dfd);
    if (read < 0) {
      return read;
    }
    return 0;
  }

  SceIoStat st;
  memset(&st, 0, sizeof(st));
  if (sceIoGetstat(filesystem_path, &st) < 0) {
    return -1;
  }
  if (tar_write_header(tar_fd, archive_name, (uint64_t)st.st_size, 0) < 0) {
    return -1;
  }

  SceUID in_fd = sceIoOpen(filesystem_path, SCE_O_RDONLY, 0);
  if (in_fd < 0) {
    return in_fd;
  }

  char buffer[4096];
  int read = 0;
  while ((read = sceIoRead(in_fd, buffer, sizeof(buffer))) > 0) {
    if (sceIoWrite(tar_fd, buffer, read) != read) {
      sceIoClose(in_fd);
      return -1;
    }
  }
  sceIoClose(in_fd);

  if (tar_write_file_padding(tar_fd, (uint64_t)st.st_size) < 0) {
    return -1;
  }
  return 0;
}

static int build_child_path(
    const char *parent,
    const char *child,
    char *out_path,
    size_t out_path_size) {
  if (!has_text_local(parent) || !has_text_local(child) ||
      (out_path == NULL) || (out_path_size == 0U)) {
    return -1;
  }

  size_t parent_len = strlen(parent);
  const char *separator = ((parent_len > 0U) && is_path_separator(parent[parent_len - 1U])) ? "" : "/";
  int wrote = snprintf(out_path, out_path_size, "%s%s%s", parent, separator, child);
  return ((wrote > 0) && ((size_t)wrote < out_path_size)) ? 0 : -1;
}

static int extract_parent_path_local(
    const char *path,
    char *out_parent,
    size_t out_parent_size) {
  if (!has_text_local(path) || (out_parent == NULL) || (out_parent_size == 0U)) {
    return -1;
  }

  const char *last_forward = strrchr(path, '/');
  const char *last_backward = strrchr(path, '\\');
  const char *separator = last_forward;
  if ((last_backward != NULL) && ((separator == NULL) || (last_backward > separator))) {
    separator = last_backward;
  }
  if ((separator == NULL) || (separator == path)) {
    return -1;
  }

  size_t parent_len = (size_t)(separator - path);
  if (parent_len >= out_parent_size) {
    return -1;
  }
  memcpy(out_parent, path, parent_len);
  out_parent[parent_len] = '\0';
  return 0;
}

static int recursive_remove_path(const char *path, int depth) {
  if (!has_text_local(path) || is_device_root_path(path) || (depth > VITA_NATIVE_MAX_DEPTH + 2)) {
    return -1;
  }
  if (!path_exists(path)) {
    return 0;
  }

  if (path_is_directory_local(path)) {
    SceUID dfd = sceIoDopen(path);
    if (dfd < 0) {
      return dfd;
    }

    SceIoDirent entry;
    int read = 0;
    for (;;) {
      memset(&entry, 0, sizeof(entry));
      read = sceIoDread(dfd, &entry);
      if (read <= 0) {
        break;
      }
      if ((strcmp(entry.d_name, ".") == 0) || (strcmp(entry.d_name, "..") == 0)) {
        continue;
      }

      char child_path[ROMM_MAX_PATH_LEN];
      if (build_child_path(path, entry.d_name, child_path, sizeof(child_path)) < 0) {
        sceIoDclose(dfd);
        return -1;
      }
      int child_status = recursive_remove_path(child_path, depth + 1);
      if (child_status < 0) {
        sceIoDclose(dfd);
        return child_status;
      }
    }
    sceIoDclose(dfd);
    if (read < 0) {
      return read;
    }
    return sceIoRmdir(path);
  }

  return sceIoRemove(path);
}

static int unix_timestamp_to_vita_datetime_local(
    int64_t timestamp_unix,
    SceDateTime *out_datetime) {
  if ((timestamp_unix <= 0) || (out_datetime == NULL)) {
    return -1;
  }

  time_t raw = (time_t)timestamp_unix;
  struct tm *utc = gmtime(&raw);
  if (utc == NULL) {
    return -1;
  }

  memset(out_datetime, 0, sizeof(*out_datetime));
  out_datetime->year = (unsigned short)(utc->tm_year + 1900);
  out_datetime->month = (unsigned short)(utc->tm_mon + 1);
  out_datetime->day = (unsigned short)utc->tm_mday;
  out_datetime->hour = (unsigned short)utc->tm_hour;
  out_datetime->minute = (unsigned short)utc->tm_min;
  out_datetime->second = (unsigned short)utc->tm_sec;
  out_datetime->microsecond = 0U;
  return 0;
}

static int preserve_timestamp_on_path(const char *path, int64_t timestamp_unix) {
  if (!has_text_local(path) || (timestamp_unix <= 0)) {
    return -1;
  }

  SceIoStat st;
  memset(&st, 0, sizeof(st));
  if (sceIoGetstat(path, &st) < 0) {
    return -1;
  }
  if (unix_timestamp_to_vita_datetime_local(timestamp_unix, &st.st_mtime) < 0) {
    return -1;
  }
  return sceIoChstat(path, &st, SCE_CST_MT);
}

static int recursive_preserve_timestamp(const char *path, int64_t timestamp_unix, int depth) {
  if (!has_text_local(path) || (timestamp_unix <= 0) || (depth > VITA_NATIVE_MAX_DEPTH + 2)) {
    return -1;
  }

  if (path_is_directory_local(path)) {
    SceUID dfd = sceIoDopen(path);
    if (dfd < 0) {
      return dfd;
    }

    SceIoDirent entry;
    int read = 0;
    for (;;) {
      memset(&entry, 0, sizeof(entry));
      read = sceIoDread(dfd, &entry);
      if (read <= 0) {
        break;
      }
      if ((strcmp(entry.d_name, ".") == 0) || (strcmp(entry.d_name, "..") == 0)) {
        continue;
      }

      char child_path[ROMM_MAX_PATH_LEN];
      if (build_child_path(path, entry.d_name, child_path, sizeof(child_path)) < 0) {
        sceIoDclose(dfd);
        return -1;
      }
      int child_status = recursive_preserve_timestamp(child_path, timestamp_unix, depth + 1);
      if (child_status < 0) {
        sceIoDclose(dfd);
        return child_status;
      }
    }
    sceIoDclose(dfd);
    if (read < 0) {
      return read;
    }
  }

  return preserve_timestamp_on_path(path, timestamp_unix);
}

static int archive_read_exact(SceUID fd, void *buffer, size_t size) {
  if ((fd < 0) || ((buffer == NULL) && (size > 0U))) {
    return -1;
  }

  size_t offset = 0U;
  while (offset < size) {
    size_t remaining = size - offset;
    int chunk = (remaining > 4096U) ? 4096 : (int)remaining;
    int read = sceIoRead(fd, (char *)buffer + offset, chunk);
    if (read <= 0) {
      return -1;
    }
    offset += (size_t)read;
  }
  return 0;
}

static int archive_skip_bytes(SceUID fd, uint64_t byte_count) {
  char buffer[512];
  uint64_t remaining = byte_count;
  while (remaining > 0U) {
    int chunk = (remaining > sizeof(buffer)) ? (int)sizeof(buffer) : (int)remaining;
    int read = sceIoRead(fd, buffer, chunk);
    if (read != chunk) {
      return -1;
    }
    remaining -= (uint64_t)read;
  }
  return 0;
}

static int archive_skip_padding(SceUID fd, uint64_t size) {
  uint64_t remainder = size % VITA_TAR_BLOCK_SIZE;
  if (remainder == 0U) {
    return 0;
  }
  return archive_skip_bytes(fd, VITA_TAR_BLOCK_SIZE - remainder);
}

static int tar_header_is_zero(const char *header) {
  if (header == NULL) {
    return 0;
  }
  for (int i = 0; i < VITA_TAR_BLOCK_SIZE; ++i) {
    if (header[i] != '\0') {
      return 0;
    }
  }
  return 1;
}

static int tar_parse_octal_field(const char *field, size_t field_size, uint64_t *out_value) {
  if ((field == NULL) || (field_size == 0U) || (out_value == NULL)) {
    return -1;
  }

  uint64_t value = 0U;
  int saw_digit = 0;
  for (size_t i = 0U; i < field_size; ++i) {
    unsigned char c = (unsigned char)field[i];
    if ((c == '\0') || (c == ' ')) {
      if (saw_digit) {
        break;
      }
      continue;
    }
    if ((c < '0') || (c > '7')) {
      return -1;
    }
    value = (value * 8U) + (uint64_t)(c - '0');
    saw_digit = 1;
  }

  *out_value = value;
  return 0;
}

static int tar_header_checksum_is_valid(const char *header) {
  if (header == NULL) {
    return 0;
  }

  uint64_t stored_checksum = 0U;
  if (tar_parse_octal_field(header + 148, 8U, &stored_checksum) < 0) {
    return 0;
  }

  unsigned int computed_checksum = 0U;
  for (int i = 0; i < VITA_TAR_BLOCK_SIZE; ++i) {
    if ((i >= 148) && (i < 156)) {
      computed_checksum += (unsigned char)' ';
    } else {
      computed_checksum += (unsigned char)header[i];
    }
  }

  return stored_checksum == (uint64_t)computed_checksum;
}

static int archive_name_is_readme(const char *name) {
  return has_text_local(name) && (strcmp(name, VITA_NATIVE_ARCHIVE_README_NAME) == 0);
}

static int archive_write_file_from_tar(SceUID tar_fd, const char *target_path, uint64_t size) {
  if (!has_text_local(target_path)) {
    return -1;
  }

  char parent[ROMM_MAX_PATH_LEN];
  if (extract_parent_path_local(target_path, parent, sizeof(parent)) < 0 ||
      ensure_vita_directory_tree(parent) < 0) {
    return -1;
  }

  SceUID out_fd = sceIoOpen(target_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, VITA_NATIVE_FILE_MODE);
  if (out_fd < 0) {
    return out_fd;
  }

  char buffer[4096];
  uint64_t remaining = size;
  while (remaining > 0U) {
    int chunk = (remaining > sizeof(buffer)) ? (int)sizeof(buffer) : (int)remaining;
    int read = sceIoRead(tar_fd, buffer, chunk);
    if (read != chunk) {
      sceIoClose(out_fd);
      sceIoRemove(target_path);
      return -1;
    }
    if (sceIoWrite(out_fd, buffer, read) != read) {
      sceIoClose(out_fd);
      sceIoRemove(target_path);
      return -1;
    }
    remaining -= (uint64_t)read;
  }

  sceIoClose(out_fd);
  return 0;
}

static int extract_restore_archive_to_staging(
    const char *archive_path,
    const char *expected_title_id,
    const char *staging_directory,
    int verbose,
    char *out_archive_title_id,
    size_t out_archive_title_id_size) {
  if (!has_text_local(archive_path) || !has_text_local(expected_title_id) ||
      !has_text_local(staging_directory) ||
      (out_archive_title_id == NULL) || (out_archive_title_id_size == 0U)) {
    return VITA_NATIVE_RESTORE_ERR_INVALID_ARGUMENT;
  }

  out_archive_title_id[0] = '\0';
  SceUID tar_fd = sceIoOpen(archive_path, SCE_O_RDONLY, 0);
  if (tar_fd < 0) {
    return VITA_NATIVE_RESTORE_ERR_OPEN_ARCHIVE;
  }

  int status = VITA_NATIVE_RESTORE_OK;
  for (;;) {
    char header[VITA_TAR_BLOCK_SIZE];
    if (archive_read_exact(tar_fd, header, sizeof(header)) < 0) {
      status = VITA_NATIVE_RESTORE_ERR_READ_ARCHIVE;
      break;
    }
    if (tar_header_is_zero(header)) {
      break;
    }
    if (!tar_header_checksum_is_valid(header)) {
      status = VITA_NATIVE_RESTORE_ERR_UNSUPPORTED_ARCHIVE;
      break;
    }

    char name[VITA_TAR_NAME_FIELD_SIZE + 1];
    memcpy(name, header, VITA_TAR_NAME_FIELD_SIZE);
    name[VITA_TAR_NAME_FIELD_SIZE] = '\0';
    if (!has_text_local(name) || (header[VITA_TAR_NAME_FIELD_SIZE - 1] != '\0')) {
      status = VITA_NATIVE_RESTORE_ERR_UNSUPPORTED_ARCHIVE;
      break;
    }

    uint64_t size = 0U;
    if (tar_parse_octal_field(header + 124, 12U, &size) < 0) {
      status = VITA_NATIVE_RESTORE_ERR_UNSUPPORTED_ARCHIVE;
      break;
    }

    if (archive_name_is_readme(name)) {
      if ((archive_skip_bytes(tar_fd, size) < 0) ||
          (archive_skip_padding(tar_fd, size) < 0)) {
        status = VITA_NATIVE_RESTORE_ERR_READ_ARCHIVE;
        break;
      }
      continue;
    }

    char root_title_id[ROMM_GAME_ID_LEN];
    if ((vita_native_save_archive_member_title_id(name, root_title_id, sizeof(root_title_id)) < 0) ||
        !is_same_text_case_insensitive(root_title_id, expected_title_id)) {
      status = VITA_NATIVE_RESTORE_ERR_UNSUPPORTED_ARCHIVE;
      break;
    }

    if (!has_text_local(out_archive_title_id)) {
      copy_truncated(out_archive_title_id, out_archive_title_id_size, root_title_id);
    } else if (!is_same_text_case_insensitive(out_archive_title_id, root_title_id)) {
      status = VITA_NATIVE_RESTORE_ERR_UNSUPPORTED_ARCHIVE;
      break;
    }

    char target_path[ROMM_MAX_PATH_LEN];
    if (build_child_path(staging_directory, name, target_path, sizeof(target_path)) < 0) {
      status = VITA_NATIVE_RESTORE_ERR_INVALID_ARGUMENT;
      break;
    }

    char typeflag = header[156];
    if (typeflag == '5') {
      if (ensure_vita_directory_tree(target_path) < 0) {
        status = VITA_NATIVE_RESTORE_ERR_CREATE_DIRECTORY;
        break;
      }
      if ((archive_skip_bytes(tar_fd, size) < 0) ||
          (archive_skip_padding(tar_fd, size) < 0)) {
        status = VITA_NATIVE_RESTORE_ERR_READ_ARCHIVE;
        break;
      }
      continue;
    }

    if ((typeflag != '\0') && (typeflag != '0')) {
      status = VITA_NATIVE_RESTORE_ERR_UNSUPPORTED_ARCHIVE;
      break;
    }

    if (archive_write_file_from_tar(tar_fd, target_path, size) < 0) {
      status = VITA_NATIVE_RESTORE_ERR_WRITE_FILE;
      break;
    }
    if (archive_skip_padding(tar_fd, size) < 0) {
      status = VITA_NATIVE_RESTORE_ERR_READ_ARCHIVE;
      break;
    }
  }

  sceIoClose(tar_fd);

  if (status == VITA_NATIVE_RESTORE_OK) {
    vita_scan_log(verbose, APP_LOG_LEVEL_INFO, "restore archive extracted title_id=%s staging=%s", out_archive_title_id, staging_directory);
  }
  return status;
}

static int backup_existing_vita_container(
    const char *container_path,
    const char *title_id,
    const char *backup_directory,
    int64_t backup_timestamp_unix,
    int verbose) {
  if (!has_text_local(container_path) || !has_text_local(title_id)) {
    return VITA_NATIVE_RESTORE_ERR_INVALID_ARGUMENT;
  }
  if (!path_exists(container_path)) {
    return VITA_NATIVE_RESTORE_OK;
  }
  if (!path_is_directory_local(container_path)) {
    return VITA_NATIVE_RESTORE_ERR_BACKUP;
  }

  const char *backup_dir = has_text_local(backup_directory) ? backup_directory : VITA_NATIVE_BACKUP_ROOT;
  if (ensure_vita_directory_tree(backup_dir) < 0) {
    return VITA_NATIVE_RESTORE_ERR_BACKUP;
  }

  char backup_path[ROMM_MAX_PATH_LEN];
  int64_t stamp = backup_timestamp_unix > 0 ? backup_timestamp_unix : 0;
  int wrote = snprintf(
      backup_path,
      sizeof(backup_path),
      "%s/%s_raw-pfs-before-restore_%lld.tar",
      backup_dir,
      title_id,
      (long long)stamp);
  if ((wrote <= 0) || ((size_t)wrote >= sizeof(backup_path))) {
    return VITA_NATIVE_RESTORE_ERR_BACKUP;
  }

  SceUID tar_fd = sceIoOpen(backup_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, VITA_NATIVE_FILE_MODE);
  if (tar_fd < 0) {
    return VITA_NATIVE_RESTORE_ERR_BACKUP;
  }

  int status = tar_add_path(tar_fd, container_path, title_id, 0);
  if (status == 0) {
    tar_write_zero_block(tar_fd);
    tar_write_zero_block(tar_fd);
  }
  sceIoClose(tar_fd);
  if (status < 0) {
    sceIoRemove(backup_path);
    return VITA_NATIVE_RESTORE_ERR_BACKUP;
  }

  vita_scan_log(verbose, APP_LOG_LEVEL_INFO, "existing Vita container backed up title_id=%s backup=%s", title_id, backup_path);
  return VITA_NATIVE_RESTORE_OK;
}

int vita_native_restore_archive(
    const char *archive_path,
    const char *expected_title_id,
    const char *savedata_root,
    const char *backup_directory,
    int64_t backup_timestamp_unix,
    int64_t restored_timestamp_unix,
    int verbose,
    char *out_restored_path,
    size_t out_restored_path_size) {
  if (!has_text_local(archive_path) || !has_text_local(expected_title_id) ||
      !vita_native_save_title_id_is_official_game(expected_title_id)) {
    return VITA_NATIVE_RESTORE_ERR_INVALID_ARGUMENT;
  }
  if ((out_restored_path != NULL) && (out_restored_path_size > 0U)) {
    out_restored_path[0] = '\0';
  }

  const char *root = has_text_local(savedata_root) ? savedata_root : VITA_NATIVE_SAVEDATA_ROOT;
  char staging_directory[ROMM_MAX_PATH_LEN];
  int64_t stamp = backup_timestamp_unix > 0 ? backup_timestamp_unix : 0;
  int wrote = snprintf(
      staging_directory,
      sizeof(staging_directory),
      "%s/%s_%lld",
      VITA_NATIVE_RESTORE_STAGING_DIR,
      expected_title_id,
      (long long)stamp);
  if ((wrote <= 0) || ((size_t)wrote >= sizeof(staging_directory))) {
    return VITA_NATIVE_RESTORE_ERR_INVALID_ARGUMENT;
  }

  if (path_exists(staging_directory) &&
      (recursive_remove_path(staging_directory, 0) < 0)) {
    return VITA_NATIVE_RESTORE_ERR_REMOVE_EXISTING;
  }
  if (ensure_vita_directory_tree(staging_directory) < 0) {
    return VITA_NATIVE_RESTORE_ERR_CREATE_DIRECTORY;
  }

  char archive_title_id[ROMM_GAME_ID_LEN];
  int status = extract_restore_archive_to_staging(
      archive_path,
      expected_title_id,
      staging_directory,
      verbose,
      archive_title_id,
      sizeof(archive_title_id));
  if (status != VITA_NATIVE_RESTORE_OK) {
    recursive_remove_path(staging_directory, 0);
    return status;
  }

  char staged_container[ROMM_MAX_PATH_LEN];
  char destination_container[ROMM_MAX_PATH_LEN];
  if ((build_child_path(staging_directory, archive_title_id, staged_container, sizeof(staged_container)) < 0) ||
      (build_child_path(root, archive_title_id, destination_container, sizeof(destination_container)) < 0)) {
    recursive_remove_path(staging_directory, 0);
    return VITA_NATIVE_RESTORE_ERR_INVALID_ARGUMENT;
  }

  int has_sce_sys = 0;
  int has_keystone = 0;
  int has_param_sfo = 0;
  char policy_reason[128];
  policy_reason[0] = '\0';
  if (!validate_vita_container(
          staged_container,
          &has_sce_sys,
          &has_keystone,
          &has_param_sfo,
          policy_reason,
          sizeof(policy_reason))) {
    vita_scan_log(verbose, APP_LOG_LEVEL_WARN, "restore archive validation failed title_id=%s sce_sys=%d keystone=%d param_sfo=%d reason=%s", archive_title_id, has_sce_sys, has_keystone, has_param_sfo, has_text_local(policy_reason) ? policy_reason : "not exportable");
    recursive_remove_path(staging_directory, 0);
    return VITA_NATIVE_RESTORE_ERR_VALIDATE;
  }

  status = backup_existing_vita_container(
      destination_container,
      archive_title_id,
      backup_directory,
      backup_timestamp_unix,
      verbose);
  if (status != VITA_NATIVE_RESTORE_OK) {
    recursive_remove_path(staging_directory, 0);
    return status;
  }

  int had_existing_container = path_exists(destination_container);
  char displaced_container[ROMM_MAX_PATH_LEN];
  displaced_container[0] = '\0';
  if (had_existing_container) {
    char displaced_name[ROMM_GAME_ID_LEN + 32];
    snprintf(
        displaced_name,
        sizeof(displaced_name),
        "%s_previous_%lld",
        archive_title_id,
        (long long)stamp);
    if (build_child_path(staging_directory, displaced_name, displaced_container, sizeof(displaced_container)) < 0) {
      recursive_remove_path(staging_directory, 0);
      return VITA_NATIVE_RESTORE_ERR_INVALID_ARGUMENT;
    }
    if (path_exists(displaced_container) &&
        (recursive_remove_path(displaced_container, 0) < 0)) {
      recursive_remove_path(staging_directory, 0);
      return VITA_NATIVE_RESTORE_ERR_REMOVE_EXISTING;
    }
    if (sceIoRename(destination_container, displaced_container) < 0) {
      recursive_remove_path(staging_directory, 0);
      return VITA_NATIVE_RESTORE_ERR_REMOVE_EXISTING;
    }
  }
  if (ensure_vita_directory_tree(root) < 0) {
    if (had_existing_container) {
      sceIoRename(displaced_container, destination_container);
    }
    recursive_remove_path(staging_directory, 0);
    return VITA_NATIVE_RESTORE_ERR_CREATE_DIRECTORY;
  }
  if (sceIoRename(staged_container, destination_container) < 0) {
    if (had_existing_container) {
      sceIoRename(displaced_container, destination_container);
    }
    recursive_remove_path(staging_directory, 0);
    return VITA_NATIVE_RESTORE_ERR_RENAME;
  }

  if (had_existing_container) {
    recursive_remove_path(displaced_container, 0);
  }
  if (restored_timestamp_unix > 0 &&
      recursive_preserve_timestamp(destination_container, restored_timestamp_unix, 0) < 0) {
    vita_scan_log(verbose, APP_LOG_LEVEL_WARN, "restore timestamp preserve failed title_id=%s timestamp=%lld", archive_title_id, (long long)restored_timestamp_unix);
  }

  recursive_remove_path(staging_directory, 0);
  if ((out_restored_path != NULL) && (out_restored_path_size > 0U)) {
    snprintf(out_restored_path, out_restored_path_size, "%s", destination_container);
  }
  vita_scan_log(verbose, APP_LOG_LEVEL_INFO, "Vita native archive restored title_id=%s destination=%s", archive_title_id, destination_container);
  return VITA_NATIVE_RESTORE_OK;
}

const char *vita_native_restore_status_str(int status) {
  switch (status) {
    case VITA_NATIVE_RESTORE_OK:
      return "ok";
    case VITA_NATIVE_RESTORE_ERR_INVALID_ARGUMENT:
      return "invalid argument";
    case VITA_NATIVE_RESTORE_ERR_CREATE_DIRECTORY:
      return "cannot create restore directory";
    case VITA_NATIVE_RESTORE_ERR_OPEN_ARCHIVE:
      return "cannot open archive";
    case VITA_NATIVE_RESTORE_ERR_READ_ARCHIVE:
      return "archive read failed";
    case VITA_NATIVE_RESTORE_ERR_UNSUPPORTED_ARCHIVE:
      return "unsupported archive format";
    case VITA_NATIVE_RESTORE_ERR_WRITE_FILE:
      return "archive extraction write failed";
    case VITA_NATIVE_RESTORE_ERR_BACKUP:
      return "container backup failed";
    case VITA_NATIVE_RESTORE_ERR_REMOVE_EXISTING:
      return "cannot remove existing container";
    case VITA_NATIVE_RESTORE_ERR_RENAME:
      return "cannot install restored container";
    case VITA_NATIVE_RESTORE_ERR_VALIDATE:
      return "restored container validation failed";
    default:
      return "unknown restore error";
  }
}

/*
 * Builds the human-readable manifest stored in every Vita native archive.
 * The archive intentionally preserves raw PFS/container metadata, so the
 * manifest warns against copying the extracted payload directly into Vita3K.
 */
static void build_vita_native_archive_readme(
    const SyncSaveDescriptor *item,
    char *out_text,
    size_t out_text_size) {
  if ((out_text == NULL) || (out_text_size == 0U)) {
    return;
  }

  const char *title_id = ((item != NULL) && has_text_local(item->game_id)) ? item->game_id : "(unknown)";
  const char *title = ((item != NULL) && has_text_local(item->title)) ? item->title : "(unknown)";
  const char *notice = vita_native_save_vita3k_import_notice();
  const char *restore_notice = vita_native_save_restore_safety_notice();

  snprintf(
      out_text,
      out_text_size,
      "romm-vita-sync Vita native archive\n"
      "\n"
      "Title ID: %s\n"
      "Title: %s\n"
      "\n"
      "This tar is a raw PS Vita savedata container backup. It preserves files\n"
      "such as sce_pfs, sce_sys, PARAM.SFO, and sce_sys/keystone when present.\n"
      "\n"
      "Vita3K import note: %s.\n"
      "\n"
      "Do not copy only the game payload folder from this archive into Vita3K;\n"
      "that can leave Vita3K's SlotParam_*.bin out of sync with the payload and\n"
      "make the game reject the save. To move this progress into Vita3K, export\n"
      "the same save from a Vita with VitaShell > Open decrypted or a save\n"
      "manager, then copy the decrypted payload into the save folder generated\n"
      "by Vita3K.\n"
      "\n"
      "Native restore status: %s.\n",
      title_id,
      title,
      notice,
      restore_notice);
}

static int tar_add_text_file(SceUID tar_fd, const char *archive_name, const char *text) {
  if (!has_text_local(archive_name) || (text == NULL)) {
    return -1;
  }

  size_t text_size = strlen(text);
  if (tar_write_header(tar_fd, archive_name, (uint64_t)text_size, 0) < 0) {
    return -1;
  }
  if ((text_size > 0U) && (sceIoWrite(tar_fd, text, (int)text_size) != (int)text_size)) {
    return -1;
  }
  return tar_write_file_padding(tar_fd, (uint64_t)text_size);
}

int vita_native_prepare_export_archives(
    SyncSaveDescriptor *items,
    int item_count,
    const char *cache_directory,
    int verbose) {
  if ((items == NULL) || (item_count < 0)) {
    return -1;
  }

  const char *cache_dir = has_text_local(cache_directory) ? cache_directory : VITA_NATIVE_EXPORT_CACHE_DIR;
  int ensure_status = ensure_vita_directory_tree(cache_dir);
  if (ensure_status < 0) {
    app_log_write(
        APP_LOG_LEVEL_ERROR,
        "vita-native",
        "archive cache directory creation failed path=%s status=0x%08X",
        cache_dir,
        (unsigned int)ensure_status);
    return ensure_status;
  }

  for (int i = 0; i < item_count; ++i) {
    SyncSaveDescriptor *item = &items[i];
    if (item->platform != SYNC_SAVE_PLATFORM_VITA_NATIVE_EXPERIMENTAL) {
      continue;
    }

    char original_path[ROMM_MAX_PATH_LEN];
    snprintf(original_path, sizeof(original_path), "%s", item->path);

    char archive_path[ROMM_MAX_PATH_LEN];
    int64_t stamp = item->timestamp_unix > 0 ? item->timestamp_unix : 0;
    int wrote = snprintf(
        archive_path,
        sizeof(archive_path),
        "%s/%s_raw-pfs-backup_%lld.tar",
        cache_dir,
        item->game_id,
        (long long)stamp);
    if ((wrote < 0) || ((size_t)wrote >= sizeof(archive_path))) {
      return -1;
    }

    SceUID tar_fd = sceIoOpen(archive_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, VITA_NATIVE_FILE_MODE);
    if (tar_fd < 0) {
      app_log_write(
          APP_LOG_LEVEL_ERROR,
          "vita-native",
          "archive open failed path=%s status=0x%08X",
          archive_path,
          (unsigned int)tar_fd);
      return tar_fd;
    }
    int tar_status = tar_add_path(tar_fd, original_path, item->game_id, 0);
    if (tar_status == 0) {
      char readme_text[VITA_NATIVE_ARCHIVE_README_MAX];
      build_vita_native_archive_readme(item, readme_text, sizeof(readme_text));
      tar_status = tar_add_text_file(tar_fd, VITA_NATIVE_ARCHIVE_README_NAME, readme_text);
    }
    if (tar_status == 0) {
      tar_write_zero_block(tar_fd);
      tar_write_zero_block(tar_fd);
    }
    sceIoClose(tar_fd);
    if (tar_status < 0) {
      sceIoRemove(archive_path);
      app_log_write(
          APP_LOG_LEVEL_ERROR,
          "vita-native",
          "archive tar failed source=%s archive=%s status=0x%08X",
          original_path,
          archive_path,
          (unsigned int)tar_status);
      return tar_status;
    }

    SceIoStat archive_stat;
    memset(&archive_stat, 0, sizeof(archive_stat));
    if (sceIoGetstat(archive_path, &archive_stat) >= 0) {
      item->size_bytes = (uint64_t)archive_stat.st_size;
    }
    snprintf(item->path, sizeof(item->path), "%s", archive_path);
    sync_extract_filename(archive_path, item->filename, sizeof(item->filename));
    vita_scan_log(verbose, APP_LOG_LEVEL_INFO, "archive export prepared title_id=%s archive=%s size=%llu format=raw-pfs-backup readme=%s", item->game_id, archive_path, (unsigned long long)item->size_bytes, VITA_NATIVE_ARCHIVE_README_NAME);
    vita_scan_log(verbose, APP_LOG_LEVEL_INFO, "%s", vita_native_save_vita3k_import_notice());
  }

  return 0;
}
