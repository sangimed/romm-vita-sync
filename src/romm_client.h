#ifndef ROMM_CLIENT_H
#define ROMM_CLIENT_H

#include "sync_types.h"

#define ROMM_CLIENT_OK 0
#define ROMM_CLIENT_ERR_INVALID_ARGUMENT -1
#define ROMM_CLIENT_ERR_NOT_IMPLEMENTED -2

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

typedef struct RommClient {
  void *context;
  RommClientListRemoteSavesFn list_remote_saves;
  RommClientUploadSaveFn upload_save;
  RommClientDownloadSaveFn download_save;
} RommClient;

int romm_client_list_remote_saves(
    const RommClient *client,
    SyncSaveDescriptor *out_items,
    int max_items);

int romm_client_upload_save(
    const RommClient *client,
    const SyncSaveDescriptor *local_item);

int romm_client_download_save(
    const RommClient *client,
    const SyncSaveDescriptor *remote_item,
    const char *destination_path);

const char *romm_client_status_str(int status);

#endif
