#include "save_scanner.h"
#include "sfo_parser.h"
#include "debugScreen.h"

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define printf psvDebugScreenPrintf

/*
 * Prints scanner logs when verbose mode is enabled.
 */
static void scan_log(int verbose, const char *format, ...) {
  if (!verbose || format == NULL) {
    return;
  }

  char message[256];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);

  printf("[scan] %s\n", message);
}

/*
 * Returns a short explanation string for SFO parser status codes.
 */
static const char *sfo_status_str(int status) {
  switch (status) {
    case 0: return "ok";
    case -1: return "invalid arguments";
    case -2: return "file too small or read failed";
    case -3: return "invalid SFO magic";
    case -4: return "invalid header offsets";
    case -5: return "invalid value bounds";
    case -6: return "key not found";
    default: return "unknown error";
  }
}

/*
 * Returns 1 if path points to a directory, 0 otherwise.
 */
static int path_is_directory(const char *path) {
  if (path == NULL) {
    return 0;
  }

  SceUID dfd = sceIoDopen(path);
  if (dfd < 0) {
    return 0;
  }
  sceIoDclose(dfd);
  return 1;
}

/*
 * Logs PARAM.SFO metadata for a game directory when available.
 */
static void log_param_sfo_metadata(const char *dir_path, int verbose) {
  if (!verbose || dir_path == NULL) {
    return;
  }

  char sfo_path[ROMM_MAX_PATH_LEN];
  int wrote = snprintf(sfo_path, sizeof(sfo_path), "%s/PARAM.SFO", dir_path);
  if ((wrote < 0) || (wrote >= (int)sizeof(sfo_path))) {
    return;
  }

  char title[128];
  int title_status = sfo_read_title(sfo_path, title, sizeof(title));
  if (title_status < 0) {
    scan_log(verbose, "PARAM.SFO unreadable status=%d (%s) path=%s",
             title_status, sfo_status_str(title_status), sfo_path);
    return;
  }

  char category[32];
  int category_status = sfo_read_key(sfo_path, "CATEGORY", category, sizeof(category));
  if (category_status == 0) {
    scan_log(verbose, "PARAM.SFO: title=%s category=%s dir=%s", title, category, dir_path);
  } else {
    scan_log(verbose, "PARAM.SFO: title=%s dir=%s", title, dir_path);
  }
}

/*
 * Clamps an unsigned value to the provided inclusive range.
 */
static unsigned clamp_unsigned(unsigned value, unsigned min, unsigned max) {
  if (value < min) {
    return min;
  }
  if (value > max) {
    return max;
  }
  return value;
}

/*
 * Extracts the parent folder name from a full path.
 * For ux0:/.../SAVEDATA/SLPS00548/SCEVMC0.VMP, this returns SLPS00548.
 */
static void extract_parent_folder_name(const char *path, char *out, size_t out_size) {
  if ((path == NULL) || (out == NULL) || (out_size == 0)) {
    return;
  }

  out[0] = '\0';

  const char *last_sep = strrchr(path, '/');
  if (last_sep == NULL || last_sep == path) {
    return;
  }

  const char *parent_end = last_sep - 1;
  while (parent_end > path && *parent_end == '/') {
    parent_end--;
  }

  const char *parent_start = parent_end;
  while (parent_start > path && *(parent_start - 1) != '/') {
    parent_start--;
  }

  size_t len = (size_t)(parent_end - parent_start + 1);
  if (len >= out_size) {
    len = out_size - 1;
  }

  memcpy(out, parent_start, len);
  out[len] = '\0';
}

/*
 * Returns 1 when the provided file name ends with .vmp (case-insensitive).
 */
static int has_vmp_extension(const char *name) {
  size_t len = strlen(name);
  if (len < 4) {
    return 0;
  }

  const char *ext = name + (len - 4);
  return ((ext[0] == '.') &&
          ((ext[1] == 'v') || (ext[1] == 'V')) &&
          ((ext[2] == 'm') || (ext[2] == 'M')) &&
          ((ext[3] == 'p') || (ext[3] == 'P')));
}

/*
 * Converts a Vita date/time value to a readable local timestamp string.
 */
static void format_timestamp(const SceDateTime *dt, char *out, size_t out_size) {
  if ((dt == NULL) || (out == NULL) || (out_size == 0)) {
    return;
  }

  unsigned year = clamp_unsigned((unsigned)dt->year, 0U, 9999U);
  unsigned month = clamp_unsigned((unsigned)dt->month, 1U, 12U);
  unsigned day = clamp_unsigned((unsigned)dt->day, 1U, 31U);
  unsigned hour = clamp_unsigned((unsigned)dt->hour, 0U, 23U);
  unsigned minute = clamp_unsigned((unsigned)dt->minute, 0U, 59U);
  unsigned second = clamp_unsigned((unsigned)dt->second, 0U, 59U);

  snprintf(out, out_size,
           "%04u-%02u-%02u %02u:%02u:%02u",
           year,
           month,
           day,
           hour,
           minute,
           second);
}

/*
 * Appends one detected VMP file to the result buffer if capacity allows.
 */
static void try_add_vmp_file(const char *path, const SceIoStat *st, int verbose, ScanResult *result) {
  if ((path == NULL) || (st == NULL) || (result == NULL)) {
    return;
  }

  if (result->count >= ROMM_MAX_VMP_ITEMS) {
    return;
  }

  SaveItem *item = &result->items[result->count];
  extract_parent_folder_name(path, item->game_id, sizeof(item->game_id));
  snprintf(item->path, sizeof(item->path), "%s", path);
  item->size_bytes = (uint64_t)st->st_size;
  format_timestamp(&st->st_mtime, item->timestamp, sizeof(item->timestamp));

  /* Try to read game title from PARAM.SFO in the same directory */
  item->game_title[0] = '\0';
  if (item->game_id[0] != '\0') {
    const char *last_sep = strrchr(path, '/');
    if (last_sep != NULL) {
      char sfo_path[ROMM_MAX_PATH_LEN];
      size_t dir_len = (size_t)(last_sep - path);
      if (dir_len + sizeof("/PARAM.SFO") <= sizeof(sfo_path)) {
        memcpy(sfo_path, path, dir_len);
        memcpy(sfo_path + dir_len, "/PARAM.SFO", sizeof("/PARAM.SFO"));
        int sfo_status = sfo_read_title(sfo_path, item->game_title, sizeof(item->game_title));
        if (sfo_status < 0) {
          scan_log(verbose, "PARAM.SFO unreadable status=%d (%s) path=%s",
                   sfo_status, sfo_status_str(sfo_status), sfo_path);
        }
      }
    }
  }

  result->count += 1;
  result->stats.vmp_found += 1;
  scan_log(verbose, "VMP detected: game=%s file=%s",
           item->game_id[0] != '\0' ? item->game_id : "unknown",
           item->path);
}

/*
 * Recursively scans a directory tree and collects matching .VMP files.
 * The scan is read-only and depth-limited to avoid unbounded traversal.
 */
static int scan_directory_recursive(const char *root, int depth, int max_depth, int verbose, ScanResult *result) {
  if ((root == NULL) || (result == NULL) || (depth > max_depth)) {
    return 0;
  }

  scan_log(verbose, "scan directory (depth=%d): %s", depth, root);

  SceUID dfd = sceIoDopen(root);
  if (dfd < 0) {
    result->stats.access_errors += 1;
    scan_log(verbose, "directory access error (%d): %s", (int)dfd, root);
    return dfd;
  }

  result->stats.directories_scanned += 1;

  SceIoDirent entry;
  memset(&entry, 0, sizeof(entry));

  for (;;) {
    memset(&entry, 0, sizeof(entry));
    int read = sceIoDread(dfd, &entry);
    if (read < 0) {
      result->stats.access_errors += 1;
      break;
    }
    if (read == 0) {
      break;
    }

    const char *name = entry.d_name;
    if ((strcmp(name, ".") == 0) || (strcmp(name, "..") == 0)) {
      continue;
    }

    char full_path[ROMM_MAX_PATH_LEN];
    int wrote = snprintf(full_path, sizeof(full_path), "%s/%s", root, name);
    if ((wrote < 0) || (wrote >= (int)sizeof(full_path))) {
      continue;
    }

    if (path_is_directory(full_path)) {
      log_param_sfo_metadata(full_path, verbose);
      if (depth < max_depth) {
        scan_directory_recursive(full_path, depth + 1, max_depth, verbose, result);
      }
      continue;
    }

    result->stats.files_scanned += 1;
    if (has_vmp_extension(name)) {
      try_add_vmp_file(full_path, &entry.d_stat, verbose, result);
    }
  }

  sceIoDclose(dfd);
  return 0;
}

/*
 * Scans all candidate roots for PS1 virtual memory card files.
 * Populates inventory and statistics in out_result.
 */
int scan_vmp_files(const char *const *roots, int root_count, int max_depth, int verbose, ScanResult *out_result) {
  if ((roots == NULL) || (out_result == NULL) || (root_count <= 0) || (max_depth < 0)) {
    return -1;
  }

  memset(out_result, 0, sizeof(*out_result));
  scan_log(verbose, "scan start roots=%d max_depth=%d", root_count, max_depth);

  for (int i = 0; i < root_count; ++i) {
    const char *root = roots[i];
    if ((root == NULL) || (root[0] == '\0')) {
      continue;
    }

    out_result->stats.paths_checked += 1;
    scan_log(verbose, "probing root: %s", root);

    SceUID probe = sceIoDopen(root);
    if (probe < 0) {
      out_result->stats.access_errors += 1;
      scan_log(verbose, "root inaccessible dec=%d hex=0x%08X path=%s", (int)probe, (unsigned int)probe, root);
      if (strstr(root, "pspemu/") != NULL) {
        scan_log(verbose, "hint: access to pspemu may require an UNSAFE SELF");
      }
      continue;
    }
    sceIoDclose(probe);

    out_result->stats.paths_accessible += 1;
    scan_log(verbose, "root accessible: %s", root);
    scan_directory_recursive(root, 0, max_depth, verbose, out_result);
  }

  scan_log(verbose, "fin scan vmp=%d dirs=%d files=%d errors=%d",
           out_result->stats.vmp_found,
           out_result->stats.directories_scanned,
           out_result->stats.files_scanned,
           out_result->stats.access_errors);

  return 0;
}
