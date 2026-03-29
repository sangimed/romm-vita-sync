#ifndef SYNC_TYPES_H
#define SYNC_TYPES_H

#include <stddef.h>
#include <stdint.h>

#include "save_item.h"

#define ROMM_SYNC_MAX_ITEMS 256
#define ROMM_SYNC_MAX_ACTIONS 512
#define ROMM_SYNC_MAX_STATE_ENTRIES 512
#define ROMM_SYNC_MAX_FILENAME_LEN 64
#define ROMM_SYNC_MAX_DEVICE_ID_LEN 64
#define ROMM_SYNC_MAX_HASH_LEN 65
#define ROMM_SYNC_MAX_REASON_LEN 128

typedef enum SyncSlot {
  SYNC_SLOT_UNKNOWN = -1,
  SYNC_SLOT_0 = 0,
  SYNC_SLOT_1 = 1
} SyncSlot;

typedef enum SyncActionType {
  SYNC_ACTION_NONE = 0,
  SYNC_ACTION_UPLOAD = 1,
  SYNC_ACTION_DOWNLOAD = 2,
  SYNC_ACTION_SKIP = 3
} SyncActionType;

typedef enum SyncConflictType {
  SYNC_CONFLICT_NONE = 0,
  SYNC_CONFLICT_REMOTE_NEWER = 1,
  SYNC_CONFLICT_LOCAL_NEWER = 2,
  SYNC_CONFLICT_SAME_TIMESTAMP_DIFFERENT_CONTENT = 3,
  SYNC_CONFLICT_SAME_ORIGIN_DEVICE = 4
} SyncConflictType;

typedef struct SyncSaveDescriptor {
  int remote_id;
  char game_id[ROMM_GAME_ID_LEN];
  char title[ROMM_GAME_TITLE_LEN];
  char filename[ROMM_SYNC_MAX_FILENAME_LEN];
  char path[ROMM_MAX_PATH_LEN];
  char remote_path[ROMM_MAX_PATH_LEN];
  uint64_t size_bytes;
  int64_t timestamp_unix;
  char hash[ROMM_SYNC_MAX_HASH_LEN];
  char origin_device[ROMM_SYNC_MAX_DEVICE_ID_LEN];
  int device_is_current;
  int device_is_untracked;
  SyncSlot slot;
} SyncSaveDescriptor;

typedef struct SyncStateEntry {
  char game_id[ROMM_GAME_ID_LEN];
  char filename[ROMM_SYNC_MAX_FILENAME_LEN];
  SyncSlot slot;
  uint64_t size_bytes;
  int64_t timestamp_unix;
  char origin_device[ROMM_SYNC_MAX_DEVICE_ID_LEN];
  int64_t last_upload_unix;
} SyncStateEntry;

typedef struct SyncActionRecord {
  char game_id[ROMM_GAME_ID_LEN];
  char filename[ROMM_SYNC_MAX_FILENAME_LEN];
  SyncSlot slot;
  SyncActionType action;
  SyncConflictType conflict;
  int executed;
  int status_code;
  char reason[ROMM_SYNC_MAX_REASON_LEN];
} SyncActionRecord;

typedef struct SyncRunReport {
  int local_count;
  int remote_count;
  int uploads_planned;
  int downloads_planned;
  int uploads_executed;
  int downloads_executed;
  int conflicts_detected;
  int skipped;
  int transfer_errors;
  int action_count;
  SyncActionRecord actions[ROMM_SYNC_MAX_ACTIONS];
} SyncRunReport;

/*
 * Utility helpers shared across synchronization modules.
 */
void sync_save_descriptor_init(SyncSaveDescriptor *item);
void sync_state_entry_init(SyncStateEntry *entry);
int sync_string_ieq(const char *lhs, const char *rhs);
int sync_extract_filename(const char *path, char *out_filename, size_t out_size);
int sync_slot_from_filename(const char *filename, SyncSlot *out_slot);
int sync_parse_local_timestamp(const char *timestamp, int64_t *out_timestamp_unix);
const char *sync_slot_str(SyncSlot slot);
const char *sync_action_type_str(SyncActionType action);
const char *sync_conflict_type_str(SyncConflictType conflict);

#endif
