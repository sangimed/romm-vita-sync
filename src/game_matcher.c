#include "game_matcher.h"

#include <string.h>

/*
 * Returns non-zero when a string is non-null and non-empty.
 */
static int has_text(const char *value) {
  return (value != NULL) && (value[0] != '\0');
}

/*
 * Validates whether a slot value is one of the known PS1 card slots.
 */
static int slot_is_known(SyncSlot slot) {
  return (slot == SYNC_SLOT_0) || (slot == SYNC_SLOT_1);
}

/*
 * Returns non-zero when two slots can be considered compatible for matching.
 * Unknown slots are treated as compatible fallbacks.
 */
static int slot_is_compatible(SyncSlot local_slot, SyncSlot remote_slot) {
  if (!slot_is_known(local_slot) || !slot_is_known(remote_slot)) {
    return 1;
  }

  return local_slot == remote_slot;
}

/*
 * Matches items by filename when both descriptors provide one.
 */
static int filename_matches(const SyncSaveDescriptor *local_item, const SyncSaveDescriptor *remote_item) {
  if ((local_item == NULL) || (remote_item == NULL)) {
    return 0;
  }

  if (!has_text(local_item->filename) || !has_text(remote_item->filename)) {
    return 0;
  }

  return sync_string_ieq(local_item->filename, remote_item->filename);
}

/*
 * Matches items by game identifier when both descriptors provide one.
 */
static int game_id_matches(const SyncSaveDescriptor *local_item, const SyncSaveDescriptor *remote_item) {
  if ((local_item == NULL) || (remote_item == NULL)) {
    return 0;
  }

  if (!has_text(local_item->game_id) || !has_text(remote_item->game_id)) {
    return 0;
  }

  return sync_string_ieq(local_item->game_id, remote_item->game_id);
}

/*
 * Finds the best remote candidate using stable priority rules:
 * exact game+slot+filename, then game+slot, then filename-based fallbacks.
 */
int game_matcher_find_remote_index(
    const SyncSaveDescriptor *local_item,
    const SyncSaveDescriptor *remote_items,
    int remote_count) {
  if ((local_item == NULL) || (remote_items == NULL) || (remote_count <= 0)) {
    return -1;
  }

  for (int i = 0; i < remote_count; ++i) {
    const SyncSaveDescriptor *candidate = &remote_items[i];
    if (game_id_matches(local_item, candidate) &&
        slot_is_known(local_item->slot) &&
        slot_is_known(candidate->slot) &&
        (local_item->slot == candidate->slot) &&
        filename_matches(local_item, candidate)) {
      return i;
    }
  }

  for (int i = 0; i < remote_count; ++i) {
    const SyncSaveDescriptor *candidate = &remote_items[i];
    if (game_id_matches(local_item, candidate) && slot_is_compatible(local_item->slot, candidate->slot)) {
      return i;
    }
  }

  for (int i = 0; i < remote_count; ++i) {
    const SyncSaveDescriptor *candidate = &remote_items[i];
    if (filename_matches(local_item, candidate) &&
        slot_is_known(local_item->slot) &&
        slot_is_known(candidate->slot) &&
        (local_item->slot == candidate->slot)) {
      return i;
    }
  }

  for (int i = 0; i < remote_count; ++i) {
    const SyncSaveDescriptor *candidate = &remote_items[i];
    if (filename_matches(local_item, candidate)) {
      return i;
    }
  }

  return -1;
}
