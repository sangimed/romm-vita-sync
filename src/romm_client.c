#include "romm_client.h"

/*
 * Delegates remote save listing to the configured RomM client callback.
 */
int romm_client_list_remote_saves(
    const RommClient *client,
    SyncSaveDescriptor *out_items,
    int max_items) {
  if ((client == NULL) || (out_items == NULL) || (max_items <= 0)) {
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }

  if (client->list_remote_saves == NULL) {
    return ROMM_CLIENT_ERR_NOT_IMPLEMENTED;
  }

  return client->list_remote_saves(client->context, out_items, max_items);
}

/*
 * Delegates upload execution to the configured RomM client callback.
 */
int romm_client_upload_save(
    const RommClient *client,
    const SyncSaveDescriptor *local_item) {
  if ((client == NULL) || (local_item == NULL)) {
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }

  if (client->upload_save == NULL) {
    return ROMM_CLIENT_ERR_NOT_IMPLEMENTED;
  }

  return client->upload_save(client->context, local_item);
}

/*
 * Delegates download execution to the configured RomM client callback.
 */
int romm_client_download_save(
    const RommClient *client,
    const SyncSaveDescriptor *remote_item,
    const char *destination_path) {
  if ((client == NULL) || (remote_item == NULL) || (destination_path == NULL) || (destination_path[0] == '\0')) {
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }

  if (client->download_save == NULL) {
    return ROMM_CLIENT_ERR_NOT_IMPLEMENTED;
  }

  return client->download_save(client->context, remote_item, destination_path);
}

/*
 * Returns a human-readable message for RomM client status codes.
 */
const char *romm_client_status_str(int status) {
  switch (status) {
    case ROMM_CLIENT_OK:
      return "ok";
    case ROMM_CLIENT_ERR_INVALID_ARGUMENT:
      return "invalid argument";
    case ROMM_CLIENT_ERR_NOT_IMPLEMENTED:
      return "not implemented";
    default:
      return "unknown error";
  }
}
