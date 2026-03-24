#ifndef SAVE_SCANNER_H
#define SAVE_SCANNER_H

#include "save_item.h"

#define ROMM_MAX_VMP_ITEMS 256

typedef struct ScanStats {
  int paths_checked;
  int paths_accessible;
  int directories_scanned;
  int files_scanned;
  int vmp_found;
  int access_errors;
} ScanStats;

typedef struct ScanResult {
  SaveItem items[ROMM_MAX_VMP_ITEMS];
  int count;
  ScanStats stats;
} ScanResult;

/*
 * Scan candidate root directories for .VMP files.
 * roots: list of directories to probe
 * root_count: number of entries in roots
 * max_depth: recursive depth limit per root
 * verbose: when non-zero, prints detailed scan logs to debug screen
 * out_result: output inventory and counters
 * Returns 0 on success, negative value on invalid arguments.
 */
int scan_vmp_files(const char *const *roots, int root_count, int max_depth, int verbose, ScanResult *out_result);

#endif
