#include "conflict_resolver.h"

/*
 * Returns non-zero when a string is non-null and non-empty.
 */
static int has_text(const char *value) {
  return (value != NULL) && (value[0] != '\0');
}

/*
 * Compares hashes only when both sides expose one.
 */
static int hashes_differ(const SyncSaveDescriptor *local_item, const SyncSaveDescriptor *remote_item) {
  if ((local_item == NULL) || (remote_item == NULL)) {
    return 0;
  }

  if (!has_text(local_item->hash) || !has_text(remote_item->hash)) {
    return 0;
  }

  return !sync_string_ieq(local_item->hash, remote_item->hash);
}

/*
 * Applies deterministic conflict classification between local and remote saves.
 */
SyncConflictType conflict_resolver_detect(
    const SyncSaveDescriptor *local_item,
    const SyncSaveDescriptor *remote_item,
    const char *local_device_id) {
  if ((local_item == NULL) || (remote_item == NULL)) {
    return SYNC_CONFLICT_NONE;
  }

  if (has_text(local_device_id) &&
      has_text(remote_item->origin_device) &&
      sync_string_ieq(local_device_id, remote_item->origin_device)) {
    return SYNC_CONFLICT_SAME_ORIGIN_DEVICE;
  }

  if ((local_item->timestamp_unix > 0) && (remote_item->timestamp_unix > 0)) {
    if (local_item->timestamp_unix > remote_item->timestamp_unix) {
      return SYNC_CONFLICT_LOCAL_NEWER;
    }

    if (local_item->timestamp_unix < remote_item->timestamp_unix) {
      return SYNC_CONFLICT_REMOTE_NEWER;
    }

    if ((local_item->size_bytes != remote_item->size_bytes) ||
        hashes_differ(local_item, remote_item)) {
      return SYNC_CONFLICT_SAME_TIMESTAMP_DIFFERENT_CONTENT;
    }

    return SYNC_CONFLICT_NONE;
  }

  if ((local_item->size_bytes != remote_item->size_bytes) ||
      hashes_differ(local_item, remote_item)) {
    return SYNC_CONFLICT_SAME_TIMESTAMP_DIFFERENT_CONTENT;
  }

  return SYNC_CONFLICT_NONE;
}

/*
 * Returns the default decision used when no UI callback overrides it.
 */
SyncActionType conflict_resolver_default_action(SyncConflictType conflict) {
  switch (conflict) {
    case SYNC_CONFLICT_LOCAL_NEWER:
      return SYNC_ACTION_UPLOAD;
    case SYNC_CONFLICT_REMOTE_NEWER:
      return SYNC_ACTION_DOWNLOAD;
    case SYNC_CONFLICT_SAME_TIMESTAMP_DIFFERENT_CONTENT:
    case SYNC_CONFLICT_SAME_ORIGIN_DEVICE:
    case SYNC_CONFLICT_NONE:
    default:
      return SYNC_ACTION_SKIP;
  }
}

/*
 * Indicates whether the conflict should require explicit user confirmation.
 */
int conflict_resolver_requires_confirmation(SyncConflictType conflict) {
  switch (conflict) {
    case SYNC_CONFLICT_LOCAL_NEWER:
    case SYNC_CONFLICT_REMOTE_NEWER:
    case SYNC_CONFLICT_SAME_TIMESTAMP_DIFFERENT_CONTENT:
      return 1;
    case SYNC_CONFLICT_SAME_ORIGIN_DEVICE:
    case SYNC_CONFLICT_NONE:
    default:
      return 0;
  }
}
