#ifndef VITA_NATIVE_SAVE_SCANNER_H
#define VITA_NATIVE_SAVE_SCANNER_H

#include "sync_types.h"

#define VITA_NATIVE_SAVEDATA_ROOT "ux0:user/00/savedata"
#define VITA_NATIVE_EXPORT_CACHE_DIR "ux0:data/romm-vita-sync/cache/vita-native"
#define VITA_NATIVE_BACKUP_ROOT "ux0:data/romm-vita-sync/backups"

int vita_native_scan_save_containers(
    const char *root,
    int verbose,
    SyncSaveDescriptor *out_items,
    int max_items);

int vita_native_prepare_export_archives(
    SyncSaveDescriptor *items,
    int item_count,
    const char *cache_directory,
    int verbose);

#endif
