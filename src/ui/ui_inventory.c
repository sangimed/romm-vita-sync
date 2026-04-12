#include "ui_inventory.h"

#include <stdio.h>

#include "debugScreen.h"

#define printf psvDebugScreenPrintf

/*
 * Prints a human-readable summary of scan statistics and each detected PS1
 * save target, including synthetic slot-0 restore targets.
 */
void render_inventory(const ScanResult *result) {
  if (result == NULL) {
    printf("No scan result available.\n");
    return;
  }

  printf("Scan summary\n");
  printf("  paths checked   : %d\n", result->stats.paths_checked);
  printf("  paths accessible: %d\n", result->stats.paths_accessible);
  printf("  dirs scanned    : %d\n", result->stats.directories_scanned);
  printf("  files scanned   : %d\n", result->stats.files_scanned);
  printf("  VMP found       : %d\n", result->stats.vmp_found);
  printf("  access errors   : %d\n\n", result->stats.access_errors);

  if (result->count == 0) {
    printf("No PS1 save target found in candidate paths.\n");
    return;
  }

  printf("Detected PS1 save targets\n");
  for (int i = 0; i < result->count; ++i) {
    const SaveItem *item = &result->items[i];
    printf("[%03d] %s\n", i + 1, item->path);
    printf("      game: %s\n", item->game_id[0] != '\0' ? item->game_id : "(unknown)");
    printf("      title: %s\n", item->game_title[0] != '\0' ? item->game_title : "(no PARAM.SFO)");
    printf("      size: %llu bytes\n", (unsigned long long)item->size_bytes);
    printf("      time: %s\n", item->timestamp);
  }
}
