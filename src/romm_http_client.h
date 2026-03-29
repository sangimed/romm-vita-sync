#ifndef ROMM_HTTP_CLIENT_H
#define ROMM_HTTP_CLIENT_H

#include <stddef.h>

#include "sync_types.h"

/*
 * Real RomM device registration callback.
 * context must point to AppConfig.
 */
int romm_http_register_device_callback(
    void *context,
    const char *device_name,
    const char *device_platform,
    const char *client_name,
    const char *client_version,
    char *out_device_id,
    size_t out_device_id_size);

/*
 * Real RomM save-list callback backed by GET /api/saves.
 * context must point to AppConfig.
 */
int romm_http_list_remote_saves_callback(
    void *context,
    SyncSaveDescriptor *out_items,
    int max_items);

/*
 * Real RomM upload callback backed by POST /api/saves.
 * local_item->rom_id must be set.
 */
int romm_http_upload_save_callback(
    void *context,
    const SyncSaveDescriptor *local_item);

/*
 * Real RomM download callback backed by GET /api/saves/{id}/content.
 */
int romm_http_download_save_callback(
    void *context,
    const SyncSaveDescriptor *remote_item,
    const char *destination_path);

/*
 * Resolves local save descriptors to RomM rom_id values via /api/roms metadata.
 * Returns number of resolved descriptors, or a negative RomM client error code.
 */
int romm_http_resolve_rom_ids(
    const void *context,
    SyncSaveDescriptor *items,
    int item_count);

#endif
