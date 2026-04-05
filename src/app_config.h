#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include "save_item.h"
#include "sync_types.h"

#define APP_CONFIG_OK 0
#define APP_CONFIG_ERR_INVALID_ARGUMENT -1
#define APP_CONFIG_ERR_NOT_FOUND -2
#define APP_CONFIG_ERR_OPEN -3
#define APP_CONFIG_ERR_READ -4
#define APP_CONFIG_ERR_FORMAT -5
#define APP_CONFIG_ERR_CREATE_DIRECTORY -6
#define APP_CONFIG_ERR_WRITE -7

#define APP_CONFIG_MAX_URL_LEN 256
#define APP_CONFIG_MAX_USERNAME_LEN 96
#define APP_CONFIG_MAX_PASSWORD_LEN 160
#define APP_CONFIG_MAX_TOKEN_LEN 256
#define APP_CONFIG_MAX_PLATFORM_FILTER_LEN 32
#define APP_CONFIG_MAX_EMULATOR_LEN 64
#define APP_CONFIG_MAX_DEVICE_NAME_LEN 64
#define APP_CONFIG_MAX_DEVICE_PLATFORM_LEN 32
#define APP_CONFIG_MAX_CLIENT_NAME_LEN 64
#define APP_CONFIG_MAX_CLIENT_VERSION_LEN 32

#define APP_CONFIG_LOG_LEVEL_ERROR 0
#define APP_CONFIG_LOG_LEVEL_WARN 1
#define APP_CONFIG_LOG_LEVEL_INFO 2
#define APP_CONFIG_LOG_LEVEL_DEBUG 3

#define APP_CONFIG_DEFAULT_PATH "ux0:data/romm-vita-sync/settings.ini"
#define APP_CONFIG_DEFAULT_STATE_STORE_PATH "ux0:data/romm-vita-sync/sync_state.tsv"
#define APP_CONFIG_DEFAULT_BACKUP_DIRECTORY "ux0:data/romm-vita-sync/backups"

typedef struct AppConfig {
  char romm_url[APP_CONFIG_MAX_URL_LEN];
  char romm_username[APP_CONFIG_MAX_USERNAME_LEN];
  char romm_password[APP_CONFIG_MAX_PASSWORD_LEN];
  char romm_token[APP_CONFIG_MAX_TOKEN_LEN];
  char romm_platform_filter[APP_CONFIG_MAX_PLATFORM_FILTER_LEN];
  char romm_save_emulator[APP_CONFIG_MAX_EMULATOR_LEN];
  int romm_verify_tls;
  int romm_timeout_seconds;

  char device_id[ROMM_SYNC_MAX_DEVICE_ID_LEN];
  char device_name[APP_CONFIG_MAX_DEVICE_NAME_LEN];
  char device_platform[APP_CONFIG_MAX_DEVICE_PLATFORM_LEN];
  char device_client[APP_CONFIG_MAX_CLIENT_NAME_LEN];
  char device_client_version[APP_CONFIG_MAX_CLIENT_VERSION_LEN];

  char sync_state_store_path[ROMM_MAX_PATH_LEN];
  char sync_backup_directory[ROMM_MAX_PATH_LEN];
  int sync_dry_run;
  int sync_auto_on_startup;

  int log_level;
  int log_file_enabled;
  int log_scan_verbose;
} AppConfig;

/*
 * Initializes settings with safe defaults.
 */
void app_config_init_defaults(AppConfig *config);

/*
 * Loads settings from an ini-like file.
 * Supported sections: [RomM], [Device], [Sync], [Log].
 */
int app_config_load(const char *path, AppConfig *out_config);

/*
 * Saves settings to an ini-like file.
 */
int app_config_save(const char *path, const AppConfig *config);

/*
 * Updates device_id in memory with validation.
 */
int app_config_set_device_id(AppConfig *config, const char *device_id);

/*
 * Returns non-zero when a server URL is configured.
 */
int app_config_has_server_url(const AppConfig *config);

/*
 * Returns non-zero when credentials are configured (token or username+password).
 */
int app_config_has_auth(const AppConfig *config);

/*
 * Returns a readable message for a configuration status code.
 */
const char *app_config_status_str(int status);

#endif
