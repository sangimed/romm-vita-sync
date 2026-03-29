#ifndef GAME_MATCHER_H
#define GAME_MATCHER_H

#include "sync_types.h"

/*
 * Finds the best remote candidate for a local save entry.
 * Returns remote index on success, or -1 when no deterministic match exists.
 */
int game_matcher_find_remote_index(
    const SyncSaveDescriptor *local_item,
    const SyncSaveDescriptor *remote_items,
    int remote_count);

#endif
