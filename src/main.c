#include <psp2/kernel/threadmgr.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "debugScreen.h"

#include "app_config.h"
#include "ps1_paths.h"
#include "romm_client.h"
#include "scan_to_sync_adapter.h"
#include "save_scanner.h"
#include "sync_engine.h"
#include "ui_inventory.h"

#define printf psvDebugScreenPrintf

typedef struct DemoRommClientContext {
  const AppConfig *config;
} DemoRommClientContext;

/*
 * Returns non-zero when a string is non-null and non-empty.
 */
static int has_text(const char *value) {
  return (value != NULL) && (value[0] != '\0');
}

/*
 * FNV-1a accumulator used to create deterministic demo identifiers.
 */
static uint32_t hash_accumulate(uint32_t hash, const char *text) {
  if (text == NULL) {
    return hash;
  }

  for (const unsigned char *cursor = (const unsigned char *)text; *cursor != 0U; ++cursor) {
    hash ^= (uint32_t)(*cursor);
    hash *= 16777619U;
  }

  return hash;
}

/*
 * Temporary stub used by the demo RomM client to expose no remote saves.
 */
static int demo_list_remote_saves(void *context, SyncSaveDescriptor *out_items, int max_items) {
  (void)out_items;
  (void)max_items;

  const DemoRommClientContext *ctx = (const DemoRommClientContext *)context;
  if ((ctx == NULL) || (ctx->config == NULL)) {
    return 0;
  }

  /*
   * Real API integration is intentionally kept out of this milestone.
   * We still route through loaded config so the connection contract is explicit.
   */
  if (!app_config_has_server_url(ctx->config) || !app_config_has_auth(ctx->config)) {
    return 0;
  }

  return 0;
}

/*
 * Infers whether this upload should simulate a server-side conflict.
 */
static int demo_should_simulate_upload_conflict(const SyncSaveDescriptor *local_item) {
  if ((local_item == NULL) || (local_item->filename[0] == '\0')) {
    return 0;
  }

  /*
   * Useful for local validation of the conflict path:
   * naming a file "*conflict*.VMP" triggers a fake 409.
   */
  return (strstr(local_item->filename, "conflict") != NULL) || (strstr(local_item->filename, "CONFLICT") != NULL);
}

/*
 * Temporary upload stub that models RomM conflict semantics.
 */
static int demo_upload_save(void *context, const SyncSaveDescriptor *local_item) {
  (void)context;
  if (demo_should_simulate_upload_conflict(local_item)) {
    return ROMM_CLIENT_ERR_CONFLICT;
  }

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
 * Temporary registration stub that simulates server-assigned device IDs.
 * Real HTTP registration will replace this callback in the network client.
 */
static int demo_register_device(
    void *context,
    const char *device_name,
    const char *device_platform,
    const char *client_name,
    const char *client_version,
    char *out_device_id,
    size_t out_device_id_size) {
  if ((out_device_id == NULL) || (out_device_id_size == 0U) ||
      !has_text(device_name) || !has_text(device_platform) ||
      !has_text(client_name) || !has_text(client_version)) {
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }

  const DemoRommClientContext *ctx = (const DemoRommClientContext *)context;
  if ((ctx == NULL) || (ctx->config == NULL)) {
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }

  if (!app_config_has_server_url(ctx->config) || !app_config_has_auth(ctx->config)) {
    return ROMM_CLIENT_ERR_AUTH;
  }

  uint32_t hash = 2166136261U;
  hash = hash_accumulate(hash, ctx->config->romm_url);
  hash = hash_accumulate(hash, device_name);
  hash = hash_accumulate(hash, device_platform);
  hash = hash_accumulate(hash, client_name);
  hash = hash_accumulate(hash, client_version);

  int written = snprintf(out_device_id, out_device_id_size, "vita-%08X", (unsigned int)hash);
  if ((written <= 0) || ((size_t)written >= out_device_id_size)) {
    out_device_id[0] = '\0';
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }

  return ROMM_CLIENT_OK;
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
 * Renders loaded config state to help validate runtime settings on-device.
 */
static void render_loaded_config(const AppConfig *config, int load_status) {
  if (config == NULL) {
    return;
  }

  printf("Config\n");
  printf("  file                : %s\n", APP_CONFIG_DEFAULT_PATH);
  printf("  load status         : %s (%d)\n", app_config_status_str(load_status), load_status);
  printf("  romm url configured : %s\n", app_config_has_server_url(config) ? "yes" : "no");
  printf("  auth configured     : %s\n", app_config_has_auth(config) ? "yes" : "no");
  printf("  device id           : %s\n", config->device_id[0] != '\0' ? config->device_id : "(not set)");
  printf("  state store         : %s\n", config->sync_state_store_path[0] != '\0' ? config->sync_state_store_path : "(disabled)");
  printf("  backup directory    : %s\n", config->sync_backup_directory[0] != '\0' ? config->sync_backup_directory : "(disabled)");
  printf("  dry-run             : %s\n\n", config->sync_dry_run ? "true" : "false");
}

/*
 * Registers device_id when missing, then persists it back to settings.ini.
 * Returns non-zero when settings.ini has been newly written.
 */
static int ensure_device_registration(AppConfig *config, const RommClient *romm_client) {
  if ((config == NULL) || (romm_client == NULL)) {
    return 0;
  }

  if (has_text(config->device_id)) {
    return 0;
  }

  if (!app_config_has_server_url(config) || !app_config_has_auth(config)) {
    printf("Device registration skipped: RomM url/auth not configured.\n");
    return 0;
  }

  char registered_device_id[ROMM_SYNC_MAX_DEVICE_ID_LEN];
  int status = romm_client_register_device(
      romm_client,
      config->device_name,
      config->device_platform,
      config->device_client,
      config->device_client_version,
      registered_device_id,
      sizeof(registered_device_id));

  if (status != ROMM_CLIENT_OK) {
    printf("Device registration failed: %s (%d)\n", romm_client_status_str(status), status);
    return 0;
  }

  if (app_config_set_device_id(config, registered_device_id) != APP_CONFIG_OK) {
    printf("Device registration failed: invalid device id returned.\n");
    return 0;
  }

  int save_status = app_config_save(APP_CONFIG_DEFAULT_PATH, config);
  if (save_status != APP_CONFIG_OK) {
    printf("Device id acquired but settings save failed: %s (%d)\n", app_config_status_str(save_status), save_status);
    return 0;
  }

  printf("Device registered and persisted: %s\n", config->device_id);
  return 1;
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
  AppConfig app_config;
  int config_status = app_config_load(APP_CONFIG_DEFAULT_PATH, &app_config);
  if ((config_status != APP_CONFIG_OK) && (config_status != APP_CONFIG_ERR_NOT_FOUND)) {
    printf("Config parsing error: %s\n", app_config_status_str(config_status));
    app_config_init_defaults(&app_config);
  }
  if (config_status == APP_CONFIG_ERR_NOT_FOUND) {
    app_config_init_defaults(&app_config);
  }

  DemoRommClientContext demo_context;
  demo_context.config = &app_config;

  RommClient demo_client;
  demo_client.context = &demo_context;
  demo_client.list_remote_saves = demo_list_remote_saves;
  demo_client.upload_save = demo_upload_save;
  demo_client.download_save = demo_download_save;
  demo_client.register_device = demo_register_device;

  int wrote_config = ensure_device_registration(&app_config, &demo_client);
  if ((config_status == APP_CONFIG_ERR_NOT_FOUND) && wrote_config) {
    config_status = APP_CONFIG_OK;
  }

  render_loaded_config(&app_config, config_status);

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
      SyncEngineConfig config;
      sync_engine_config_init(&config);
      config.device_id = app_config.device_id[0] != '\0' ? app_config.device_id : NULL;
      config.state_store_path = app_config.sync_state_store_path[0] != '\0' ? app_config.sync_state_store_path : NULL;
      config.backup_directory = app_config.sync_backup_directory[0] != '\0' ? app_config.sync_backup_directory : NULL;
      config.dry_run = app_config.sync_dry_run;

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
