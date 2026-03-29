#include <psp2/kernel/threadmgr.h>
#include <stdio.h>

#include "debugScreen.h"

#include "ps1_paths.h"
#include "romm_client.h"
#include "scan_to_sync_adapter.h"
#include "save_scanner.h"
#include "sync_engine.h"
#include "ui_inventory.h"

#define printf psvDebugScreenPrintf

/*
 * Temporary stub used by the demo RomM client to expose no remote saves.
 */
static int demo_list_remote_saves(void *context, SyncSaveDescriptor *out_items, int max_items) {
  (void)context;
  (void)out_items;
  (void)max_items;
  return 0;
}

/*
 * Temporary upload stub for dry-run integration.
 */
static int demo_upload_save(void *context, const SyncSaveDescriptor *local_item) {
  (void)context;
  (void)local_item;
  return ROMM_CLIENT_ERR_NOT_IMPLEMENTED;
}

/*
 * Temporary download stub for dry-run integration.
 */
static int demo_download_save(void *context, const SyncSaveDescriptor *remote_item, const char *destination_path) {
  (void)context;
  (void)remote_item;
  (void)destination_path;
  return ROMM_CLIENT_ERR_NOT_IMPLEMENTED;
}

/*
 * Renders the planned synchronization actions to the debug screen.
 */
static void render_sync_plan(const SyncRunReport *report) {
  if (report == NULL) {
    printf("No sync report available.\n");
    return;
  }

  printf("\nSync dry-run plan\n");
  printf("  local saves        : %d\n", report->local_count);
  printf("  remote saves       : %d\n", report->remote_count);
  printf("  planned uploads    : %d\n", report->uploads_planned);
  printf("  planned downloads  : %d\n", report->downloads_planned);
  printf("  conflicts detected : %d\n", report->conflicts_detected);
  printf("  skipped            : %d\n", report->skipped);
  printf("  transfer errors    : %d\n", report->transfer_errors);

  if (report->action_count == 0) {
    printf("  no actions generated\n");
    return;
  }

  printf("\nPlanned actions\n");
  for (int i = 0; i < report->action_count; ++i) {
    const SyncActionRecord *action = &report->actions[i];
    printf("[%03d] game=%s slot=%s file=%s\n",
           i + 1,
           action->game_id[0] != '\0' ? action->game_id : "(unknown)",
           sync_slot_str(action->slot),
           action->filename[0] != '\0' ? action->filename : "(unknown)");
    printf("      action=%s conflict=%s status=%d reason=%s\n",
           sync_action_type_str(action->action),
           sync_conflict_type_str(action->conflict),
           action->status_code,
           action->reason[0] != '\0' ? action->reason : "(none)");
  }
}

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

    SyncSaveDescriptor local_items[ROMM_SYNC_MAX_ITEMS];
    int local_count = scan_result_to_sync_saves(
        &result,
        local_items,
        (int)(sizeof(local_items) / sizeof(local_items[0])));

    if (local_count < 0) {
      printf("\nFailed to build sync inventory.\n");
    } else {
      RommClient demo_client;
      demo_client.context = NULL;
      demo_client.list_remote_saves = demo_list_remote_saves;
      demo_client.upload_save = demo_upload_save;
      demo_client.download_save = demo_download_save;

      SyncEngineConfig config;
      sync_engine_config_init(&config);
      config.device_id = "vita-device-v1";
      config.state_store_path = NULL;
      config.backup_directory = NULL;
      config.dry_run = 1;

      SyncRunReport sync_report;
      int sync_status = sync_engine_run(&config, local_items, local_count, &demo_client, &sync_report);
      if (sync_status < 0) {
        printf("\nSync dry-run failed: %s (%d)\n", sync_engine_status_str(sync_status), sync_status);
      } else {
        render_sync_plan(&sync_report);
      }
    }
  }

  printf("\nPress START to exit not implemented yet.\n");
  printf("Keeping screen visible indefinitely...\n");

  for (;;) {
    sceKernelDelayThread(1000 * 1000);
  }
}
