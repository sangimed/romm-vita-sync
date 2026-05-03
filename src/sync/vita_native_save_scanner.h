#ifndef VITA_NATIVE_SAVE_SCANNER_H
#define VITA_NATIVE_SAVE_SCANNER_H

#include "sync_types.h"

#define VITA_NATIVE_SAVEDATA_ROOT "ux0:user/00/savedata"
#define VITA_NATIVE_EXPORT_CACHE_DIR "ux0:data/romm-vita-sync/cache/vita-native"
#define VITA_NATIVE_RESTORE_STAGING_DIR "ux0:data/romm-vita-sync/cache/vita-native-restore"
#define VITA_NATIVE_BACKUP_ROOT "ux0:data/romm-vita-sync/backups"

#define VITA_NATIVE_RESTORE_OK 0
#define VITA_NATIVE_RESTORE_ERR_INVALID_ARGUMENT -1001
#define VITA_NATIVE_RESTORE_ERR_CREATE_DIRECTORY -1002
#define VITA_NATIVE_RESTORE_ERR_OPEN_ARCHIVE -1003
#define VITA_NATIVE_RESTORE_ERR_READ_ARCHIVE -1004
#define VITA_NATIVE_RESTORE_ERR_UNSUPPORTED_ARCHIVE -1005
#define VITA_NATIVE_RESTORE_ERR_WRITE_FILE -1006
#define VITA_NATIVE_RESTORE_ERR_BACKUP -1007
#define VITA_NATIVE_RESTORE_ERR_REMOVE_EXISTING -1008
#define VITA_NATIVE_RESTORE_ERR_RENAME -1009
#define VITA_NATIVE_RESTORE_ERR_VALIDATE -1010

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

/*
 * Restores one romm-vita-sync raw PFS archive into the Vita savedata root.
 * The archive is extracted into a staging directory first, validated for the
 * expected TITLE_ID, sce_sys/PARAM.SFO, and sce_sys/keystone, then the existing
 * local container is backed up as a tar before replacement. out_restored_path
 * receives the installed `ux0:user/00/savedata/<TITLE_ID>` path when provided.
 * Returns VITA_NATIVE_RESTORE_OK on success or a negative restore status code.
 */
int vita_native_restore_archive(
    const char *archive_path,
    const char *expected_title_id,
    const char *savedata_root,
    const char *backup_directory,
    int64_t backup_timestamp_unix,
    int64_t restored_timestamp_unix,
    int verbose,
    char *out_restored_path,
    size_t out_restored_path_size);

/*
 * Returns a concise user-facing message for Vita native restore status codes.
 */
const char *vita_native_restore_status_str(int status);

#endif
