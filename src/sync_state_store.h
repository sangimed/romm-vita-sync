#ifndef SYNC_STATE_STORE_H
#define SYNC_STATE_STORE_H

#include "sync_types.h"

#define SYNC_STATE_STORE_OK 0
#define SYNC_STATE_STORE_ERR_INVALID_ARGUMENT -1
#define SYNC_STATE_STORE_ERR_OPEN -2
#define SYNC_STATE_STORE_ERR_READ -3
#define SYNC_STATE_STORE_ERR_WRITE -4
#define SYNC_STATE_STORE_ERR_FORMAT -5
#define SYNC_STATE_STORE_ERR_FULL -6
#define SYNC_STATE_STORE_ERR_CREATE_DIRECTORY -7

typedef struct SyncStateStore {
  char device_id[ROMM_SYNC_MAX_DEVICE_ID_LEN];
  SyncStateEntry entries[ROMM_SYNC_MAX_STATE_ENTRIES];
  int count;
} SyncStateStore;

void sync_state_store_init(SyncStateStore *store);
void sync_state_store_set_device_id(SyncStateStore *store, const char *device_id);

int sync_state_store_load(const char *path, SyncStateStore *out_store);
int sync_state_store_save(const char *path, const SyncStateStore *store);

SyncStateEntry *sync_state_store_find(
    SyncStateStore *store,
    const char *game_id,
    const char *filename,
    SyncSlot slot);

const SyncStateEntry *sync_state_store_find_const(
    const SyncStateStore *store,
    const char *game_id,
    const char *filename,
    SyncSlot slot);

int sync_state_store_upsert(SyncStateStore *store, const SyncStateEntry *entry);
const char *sync_state_store_status_str(int status);

#endif
