#include "scan_to_sync_adapter.h"

#include <stdio.h>

/*
 * Transforms scanner output into synchronization descriptors used by SyncEngine.
 */
int scan_result_to_sync_saves(
    const ScanResult *scan_result,
    SyncSaveDescriptor *out_items,
    int max_items) {
  if ((scan_result == NULL) || (out_items == NULL) || (max_items <= 0)) {
    return -1;
  }

  int count = scan_result->count;
  if (count > max_items) {
    count = max_items;
  }

  for (int i = 0; i < count; ++i) {
    const SaveItem *scan_item = &scan_result->items[i];
    SyncSaveDescriptor *sync_item = &out_items[i];
    sync_save_descriptor_init(sync_item);

    snprintf(sync_item->game_id, sizeof(sync_item->game_id), "%s", scan_item->game_id);
    snprintf(sync_item->title, sizeof(sync_item->title), "%s", scan_item->game_title);
    snprintf(sync_item->timestamp_text, sizeof(sync_item->timestamp_text), "%s", scan_item->timestamp);
    snprintf(sync_item->path, sizeof(sync_item->path), "%s", scan_item->path);
    sync_item->size_bytes = scan_item->size_bytes;

    if (sync_extract_filename(scan_item->path, sync_item->filename, sizeof(sync_item->filename)) < 0) {
      sync_item->filename[0] = '\0';
    }

    if (sync_parse_local_timestamp(scan_item->timestamp, &sync_item->timestamp_unix) < 0) {
      sync_item->timestamp_unix = 0;
    }

    if (sync_slot_from_filename(sync_item->filename, &sync_item->slot) < 0) {
      sync_item->slot = SYNC_SLOT_UNKNOWN;
    }
  }

  return count;
}
