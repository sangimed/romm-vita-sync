#ifndef ROMM_CLIENT_H
#define ROMM_CLIENT_H

#include <stddef.h>

#include "sync_types.h"

#define ROMM_CLIENT_OK 0
#define ROMM_CLIENT_ERR_INVALID_ARGUMENT -1
#define ROMM_CLIENT_ERR_NOT_IMPLEMENTED -2
#define ROMM_CLIENT_ERR_CONFLICT -3
#define ROMM_CLIENT_ERR_AUTH -4
#define ROMM_CLIENT_ERR_NETWORK -5

typedef int (*RommClientListRemoteSavesFn)(
    void *context,
    SyncSaveDescriptor *out_items,
    int max_items);

typedef int (*RommClientUploadSaveFn)(
    void *context,
    const SyncSaveDescriptor *local_item);

typedef int (*RommClientDownloadSaveFn)(
    void *context,
    const SyncSaveDescriptor *remote_item,
    const char *destination_path);

typedef int (*RommClientRegisterDeviceFn)(
    void *context,
    const char *device_name,
    const char *device_platform,
    const char *client_name,
    const char *client_version,
    char *out_device_id,
    size_t out_device_id_size);

typedef struct RommClient {
  void *context;
  RommClientListRemoteSavesFn list_remote_saves;
  RommClientUploadSaveFn upload_save;
  RommClientDownloadSaveFn download_save;
  RommClientRegisterDeviceFn register_device;
} RommClient;

int romm_client_list_remote_saves(
    const RommClient *client,
    SyncSaveDescriptor *out_items,
    int max_items);

/*
 * Upload callbacks should return ROMM_CLIENT_ERR_CONFLICT when RomM answers 409.
 * When local_item->remote_id is set, callbacks should overwrite that existing
 * remote save instead of creating a new server entry.
 */
int romm_client_upload_save(
    const RommClient *client,
    const SyncSaveDescriptor *local_item);

int romm_client_download_save(
    const RommClient *client,
    const SyncSaveDescriptor *remote_item,
    const char *destination_path);

/*
 * Registers (or resolves) a device in RomM and returns the canonical device_id.
 */
int romm_client_register_device(
    const RommClient *client,
    const char *device_name,
    const char *device_platform,
    const char *client_name,
    const char *client_version,
    char *out_device_id,
    size_t out_device_id_size);

const char *romm_client_status_str(int status);

#endif
