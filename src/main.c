#include <psp2/kernel/threadmgr.h>
#include <stdio.h>

#include "debugScreen.h"

#include "ps1_paths.h"
#include "save_scanner.h"
#include "ui_inventory.h"

#define printf psvDebugScreenPrintf

/*
 * Entry point for the first milestone flow.
 * Initializes debug output, runs a read-only VMP scan, prints metadata,
 * and keeps the screen visible for quick validation on device.
 */
int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  psvDebugScreenInit();

  printf("romm-vita-sync - local VMP inventory\n\n");

  ScanResult result;
  int status = scan_vmp_files(kPs1VmpCandidateRoots, (int)PS1_VMP_CANDIDATE_ROOT_COUNT, 2, 1, &result);

  if (status < 0) {
    printf("Scan failed with code: %d\n", status);
  } else {
    render_inventory(&result);
  }

  printf("\nPress START to exit not implemented yet.\n");
  printf("Keeping screen visible indefinitely...\n");

  for (;;) {
    sceKernelDelayThread(1000 * 1000);
  }
}
