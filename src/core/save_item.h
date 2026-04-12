#ifndef SAVE_ITEM_H
#define SAVE_ITEM_H

#include <stdint.h>

#define ROMM_MAX_PATH_LEN 512
#define ROMM_TIMESTAMP_LEN 20
#define ROMM_GAME_ID_LEN 32
#define ROMM_GAME_TITLE_LEN 128

typedef struct SaveItem {
  char game_id[ROMM_GAME_ID_LEN];
  char game_title[ROMM_GAME_TITLE_LEN];
  char path[ROMM_MAX_PATH_LEN];
  uint64_t size_bytes;
  char timestamp[ROMM_TIMESTAMP_LEN];
} SaveItem;

#endif
