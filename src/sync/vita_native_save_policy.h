#ifndef VITA_NATIVE_SAVE_POLICY_H
#define VITA_NATIVE_SAVE_POLICY_H

#include <stddef.h>
#include <stdint.h>

/*
 * Returns non-zero only when a Vita native savedata container has the minimum
 * metadata needed for a faithful backup export. The keystone is treated as
 * mandatory because it is part of Vita PFS/signature validation; exporting a
 * container without it would create a server object that cannot be restored as
 * a recognized Vita save. out_reason receives a short English explanation when
 * provided.
 */
int vita_native_save_container_is_exportable(
    int has_sce_sys,
    int has_keystone,
    int has_param_sfo,
    char *out_reason,
    size_t out_reason_size);

/*
 * Returns non-zero when a savedata TITLE_ID matches the official PS Vita game
 * product-code shape used by regional retail/digital games: PCS[A-H] followed
 * by five digits. Homebrew commonly uses free-form title IDs and is excluded
 * from the native game list by this policy.
 */
int vita_native_save_title_id_is_official_game(const char *title_id);

/*
 * Returns the restore safety notice shown in logs and archive manifests.
 * Native Vita restore is supported only for raw romm-vita-sync archives that
 * validate the expected TITLE_ID, preserve sce_sys/keystone metadata, and are
 * restored through the backup-first archive restore path.
 */
const char *vita_native_save_restore_safety_notice(void);

/*
 * Returns the fixed Vita3K import notice for native Vita archive exports.
 * These archives preserve raw PFS/container metadata and are not decrypted
 * Vita3K save imports; users must export decrypted data separately before
 * copying progress into an emulator save directory.
 */
const char *vita_native_save_vita3k_import_notice(void);

/*
 * Extracts the source savedata timestamp encoded in romm-vita-sync Vita native
 * archive filenames. Accepts both the legacy <TITLE_ID>_<timestamp>.tar shape
 * and the explicit <TITLE_ID>_raw-pfs-backup_<timestamp>.tar shape.
 * Returns 0 on success, or -1 when the filename is not a recognized Vita
 * native archive name.
 */
int vita_native_save_archive_timestamp_from_filename(
    const char *filename,
    int64_t *out_timestamp_unix);

/*
 * Returns non-zero when a remote save filename is a Vita native raw archive
 * candidate for the expected TITLE_ID. This is a lightweight preflight guard:
 * full restore still validates tar entries, metadata, and keystone contents
 * after download. out_reason receives a short English explanation when
 * provided.
 */
int vita_native_save_archive_is_restore_candidate(
    const char *filename,
    const char *expected_title_id,
    char *out_reason,
    size_t out_reason_size);

/*
 * Validates one tar member name before restore extraction and copies the root
 * TITLE_ID into out_title_id. Rejects absolute paths, parent traversal,
 * backslashes, drive-like names, empty path segments, and non-official Vita
 * root IDs. Returns 0 on success or -1 on rejection.
 */
int vita_native_save_archive_member_title_id(
    const char *member_name,
    char *out_title_id,
    size_t out_title_id_size);

#endif
