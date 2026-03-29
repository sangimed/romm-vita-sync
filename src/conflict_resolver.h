#ifndef CONFLICT_RESOLVER_H
#define CONFLICT_RESOLVER_H

#include "sync_types.h"

/*
 * Detects synchronization conflict type by comparing local and remote metadata.
 */
SyncConflictType conflict_resolver_detect(
    const SyncSaveDescriptor *local_item,
    const SyncSaveDescriptor *remote_item,
    const char *local_device_id);

/*
 * Returns the default action for a given conflict class.
 */
SyncActionType conflict_resolver_default_action(SyncConflictType conflict);

/*
 * Returns 1 if explicit confirmation should be requested for this conflict.
 */
int conflict_resolver_requires_confirmation(SyncConflictType conflict);

#endif
