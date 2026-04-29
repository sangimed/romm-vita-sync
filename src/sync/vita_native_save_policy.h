#ifndef VITA_NATIVE_SAVE_POLICY_H
#define VITA_NATIVE_SAVE_POLICY_H

#include <stddef.h>

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
 * Returns the fixed restore-block reason for Vita native saves. Native Vita
 * restore remains disabled until the project has a proven PFS/keystone-aware
 * write path that can preserve or regenerate the console-recognized signature
 * metadata instead of naively copying encrypted files back into savedata.
 */
const char *vita_native_save_restore_unsupported_reason(void);

#endif
