#ifndef SYNC_ENGINE_H
#define SYNC_ENGINE_H

#include <stdint.h>

#include "romm_client.h"
#include "sync_types.h"

#define SYNC_ENGINE_OK 0
#define SYNC_ENGINE_ERR_INVALID_ARGUMENT -1
#define SYNC_ENGINE_ERR_REMOTE_LIST -2
#define SYNC_ENGINE_ERR_STATE_LOAD -3
#define SYNC_ENGINE_ERR_STATE_SAVE -4
#define SYNC_ENGINE_ERR_UNRESOLVED_ROM_ID -5

typedef SyncActionType (*SyncConflictResolverCallback)(
    const SyncActionRecord *candidate_action,
    const SyncSaveDescriptor *local_item,
    const SyncSaveDescriptor *remote_item,
    void *user_data);

typedef int64_t (*SyncNowCallback)(void);
typedef void (*SyncProgressCallback)(
    int completed_units,
    int total_units,
    int local_index,
    int local_total,
    const char *message,
    void *user_data);

typedef struct SyncEngineConfig {
  const char *device_id;
  const char *state_store_path;
  const char *backup_directory;
  int dry_run;
  SyncConflictResolverCallback resolve_conflict;
  void *resolve_conflict_user_data;
  SyncNowCallback now_callback;
  SyncProgressCallback progress_callback;
  void *progress_user_data;
} SyncEngineConfig;

void sync_engine_config_init(SyncEngineConfig *config);

/*
 * Executes one deterministic manual synchronization pass.
 * local_items is the current local inventory.
 * When provided, progress_callback receives real-time stage/item checkpoints.
 */
int sync_engine_run(
    const SyncEngineConfig *config,
    const SyncSaveDescriptor *local_items,
    int local_count,
    const RommClient *romm_client,
    SyncRunReport *out_report);

const char *sync_engine_status_str(int status);

#endif
