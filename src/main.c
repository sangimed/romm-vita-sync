#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <string.h>

#include "debugScreen.h"

#include "app_config.h"
#include "app_log.h"
#include "ps1_paths.h"
#include "romm_client.h"
#include "romm_http_client.h"
#include "scan_to_sync_adapter.h"
#include "save_scanner.h"
#include "sync_engine.h"
#include "ui_inventory.h"

#define printf psvDebugScreenPrintf

/*
 * Returns non-zero when a string is non-null and non-empty.
 */
static int has_text(const char *value) {
  return (value != NULL) && (value[0] != '\0');
}

/*
 * Maps AppConfig numeric log level to AppLogLevel enum safely.
 */
static AppLogLevel app_log_level_from_config(int config_level) {
  if (config_level <= APP_CONFIG_LOG_LEVEL_ERROR) {
    return APP_LOG_LEVEL_ERROR;
  }
  if (config_level == APP_CONFIG_LOG_LEVEL_WARN) {
    return APP_LOG_LEVEL_WARN;
  }
  if (config_level >= APP_CONFIG_LOG_LEVEL_DEBUG) {
    return APP_LOG_LEVEL_DEBUG;
  }
  return APP_LOG_LEVEL_INFO;
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
  printf("  log level           : %s\n", app_log_level_str(app_log_level_from_config(config->log_level)));
  printf("  scan verbose        : %s\n", config->log_scan_verbose ? "true" : "false");
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
    app_log_write(APP_LOG_LEVEL_WARN, "main", "Device registration skipped (RomM url/auth missing)");
    return 0;
  }
  app_log_write(APP_LOG_LEVEL_INFO, "main", "Registering device on RomM via /api/devices");

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
    app_log_write(APP_LOG_LEVEL_ERROR, "main", "Device registration failed: %s (%d)", romm_client_status_str(status), status);
    return 0;
  }

  if (app_config_set_device_id(config, registered_device_id) != APP_CONFIG_OK) {
    app_log_write(APP_LOG_LEVEL_ERROR, "main", "Device registration failed: invalid device id returned");
    return 0;
  }

  int save_status = app_config_save(APP_CONFIG_DEFAULT_PATH, config);
  if (save_status != APP_CONFIG_OK) {
    app_log_write(APP_LOG_LEVEL_ERROR, "main", "Device id acquired but settings save failed: %s (%d)", app_config_status_str(save_status), save_status);
    return 0;
  }

  app_log_write(APP_LOG_LEVEL_INFO, "main", "Device registered and persisted: %s", config->device_id);
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
  app_log_set_level(app_log_level_from_config(app_config.log_level));
  app_log_write(APP_LOG_LEVEL_INFO, "main", "Startup config status=%s (%d)", app_config_status_str(config_status), config_status);

  RommClient romm_client;
  romm_client.context = &app_config;
  romm_client.list_remote_saves = romm_http_list_remote_saves_callback;
  romm_client.upload_save = romm_http_upload_save_callback;
  romm_client.download_save = romm_http_download_save_callback;
  romm_client.register_device = romm_http_register_device_callback;

  int wrote_config = ensure_device_registration(&app_config, &romm_client);
  if ((config_status == APP_CONFIG_ERR_NOT_FOUND) && wrote_config) {
    config_status = APP_CONFIG_OK;
  }
  if (wrote_config) {
    app_log_write(APP_LOG_LEVEL_INFO, "main", "settings.ini updated after device bootstrap");
  }

  render_loaded_config(&app_config, config_status);

  ScanResult result;
  app_log_write(APP_LOG_LEVEL_INFO, "main", "Starting local save scan");
  int status = scan_vmp_files(
      kPs1VmpCandidateRoots,
      (int)PS1_VMP_CANDIDATE_ROOT_COUNT,
      2,
      app_config.log_scan_verbose,
      &result);

  if (status < 0) {
    printf("Scan failed with code: %d\n", status);
    app_log_write(APP_LOG_LEVEL_ERROR, "main", "Scan failed with status=%d", status);
  } else {
    app_log_write(APP_LOG_LEVEL_INFO, "main", "Scan completed: found=%d access_errors=%d", result.stats.vmp_found, result.stats.access_errors);
    render_inventory(&result);

    SyncSaveDescriptor local_items[ROMM_SYNC_MAX_ITEMS];
    int local_count = scan_result_to_sync_saves(
        &result,
        local_items,
        (int)(sizeof(local_items) / sizeof(local_items[0])));

    if (local_count < 0) {
      printf("\nFailed to build sync inventory.\n");
      app_log_write(APP_LOG_LEVEL_ERROR, "main", "Failed to convert scan result to sync descriptors");
    } else {
      int mapped_count = romm_http_resolve_rom_ids(&app_config, local_items, local_count);
      if (mapped_count < 0) {
        app_log_write(
            APP_LOG_LEVEL_WARN,
            "main",
            "ROM mapping failed: %s (%d)",
            romm_client_status_str(mapped_count),
            mapped_count);
      } else {
        app_log_write(APP_LOG_LEVEL_INFO, "main", "ROM mapping resolved=%d/%d", mapped_count, local_count);
      }

      app_log_write(APP_LOG_LEVEL_INFO, "main", "Starting sync engine (dry_run=%d, locals=%d)", app_config.sync_dry_run, local_count);
      SyncEngineConfig config;
      sync_engine_config_init(&config);
      config.device_id = app_config.device_id[0] != '\0' ? app_config.device_id : NULL;
      config.state_store_path = app_config.sync_state_store_path[0] != '\0' ? app_config.sync_state_store_path : NULL;
      config.backup_directory = app_config.sync_backup_directory[0] != '\0' ? app_config.sync_backup_directory : NULL;
      config.dry_run = app_config.sync_dry_run;

      SyncRunReport sync_report;
      int sync_status = sync_engine_run(&config, local_items, local_count, &romm_client, &sync_report);
      if (sync_status < 0) {
        printf("\nSync dry-run failed: %s (%d)\n", sync_engine_status_str(sync_status), sync_status);
        app_log_write(APP_LOG_LEVEL_ERROR, "main", "sync_engine_run failed: %s (%d)", sync_engine_status_str(sync_status), sync_status);
      } else {
        app_log_write(
            APP_LOG_LEVEL_INFO,
            "main",
            "Sync completed: actions=%d uploads=%d downloads=%d conflicts=%d errors=%d",
            sync_report.action_count,
            sync_report.uploads_planned,
            sync_report.downloads_planned,
            sync_report.conflicts_detected,
            sync_report.transfer_errors);
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
