#ifndef SCAN_TO_SYNC_ADAPTER_H
#define SCAN_TO_SYNC_ADAPTER_H

#include "save_scanner.h"
#include "sync_types.h"

/*
 * Converts scan results into canonical synchronization descriptors.
 * Returns number of items written, negative value on invalid arguments.
 */
int scan_result_to_sync_saves(
    const ScanResult *scan_result,
    SyncSaveDescriptor *out_items,
    int max_items);

#endif
