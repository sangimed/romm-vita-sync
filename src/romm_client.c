#include "romm_client.h"

/*
 * Delegates remote save listing to the configured RomM client callback.
 * rom_ids optionally narrows the request to the mapped remote games needed by
 * the current sync batch; pass NULL/0 to keep the legacy unfiltered behavior.
 */
int romm_client_list_remote_saves(
    const RommClient *client,
    const int *rom_ids,
    int rom_id_count,
    SyncSaveDescriptor *out_items,
    int max_items) {
  if ((client == NULL) || (out_items == NULL) || (max_items <= 0) ||
      (rom_id_count < 0) || ((rom_id_count > 0) && (rom_ids == NULL))) {
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }

  if (client->list_remote_saves == NULL) {
    return ROMM_CLIENT_ERR_NOT_IMPLEMENTED;
  }

  return client->list_remote_saves(client->context, rom_ids, rom_id_count, out_items, max_items);
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
 * Delegates device registration to the configured RomM client callback.
 */
int romm_client_register_device(
    const RommClient *client,
    const char *device_name,
    const char *device_platform,
    const char *client_name,
    const char *client_version,
    char *out_device_id,
    size_t out_device_id_size) {
  if ((client == NULL) ||
      (device_name == NULL) || (device_name[0] == '\0') ||
      (device_platform == NULL) || (device_platform[0] == '\0') ||
      (client_name == NULL) || (client_name[0] == '\0') ||
      (client_version == NULL) || (client_version[0] == '\0') ||
      (out_device_id == NULL) || (out_device_id_size == 0U)) {
    return ROMM_CLIENT_ERR_INVALID_ARGUMENT;
  }

  if (client->register_device == NULL) {
    return ROMM_CLIENT_ERR_NOT_IMPLEMENTED;
  }

  out_device_id[0] = '\0';
  return client->register_device(
      client->context,
      device_name,
      device_platform,
      client_name,
      client_version,
      out_device_id,
      out_device_id_size);
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
    case ROMM_CLIENT_ERR_CONFLICT:
      return "sync conflict (server newer)";
    case ROMM_CLIENT_ERR_AUTH:
      return "authentication failed";
    case ROMM_CLIENT_ERR_NETWORK:
      return "network error";
    default:
      return "unknown error";
  }
}
