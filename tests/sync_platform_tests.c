#include "sync_types.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_default_descriptor_is_psone(void) {
  SyncSaveDescriptor item;
  sync_save_descriptor_init(&item);

  assert(item.platform == SYNC_SAVE_PLATFORM_PSONE);
  assert(strcmp(sync_save_platform_id(item.platform), "psOne") == 0);
  assert(strcmp(sync_save_platform_display_name(item.platform), "PS1 / psOne") == 0);
  assert(sync_save_platform_restore_supported(item.platform) == 1);
  assert(sync_save_platform_is_experimental(item.platform) == 0);
}

static void test_vita_native_descriptor_policy(void) {
  SyncSaveDescriptor item;
  sync_save_descriptor_init(&item);
  item.platform = SYNC_SAVE_PLATFORM_VITA_NATIVE_EXPERIMENTAL;

  assert(strcmp(sync_save_platform_id(item.platform), "psVita") == 0);
  assert(strcmp(sync_save_platform_display_name(item.platform), "PS Vita native saves") == 0);
  assert(strcmp(sync_save_platform_badge(item.platform), "raw archive sync") == 0);
  assert(sync_save_platform_restore_supported(item.platform) == 1);
  assert(sync_save_platform_is_experimental(item.platform) == 0);
}

static void test_vita_native_saves_do_not_use_ps1_latest_slot_rule(void) {
  SyncSaveDescriptor items[2];
  int selected_mask[2];

  sync_save_descriptor_init(&items[0]);
  items[0].platform = SYNC_SAVE_PLATFORM_VITA_NATIVE_EXPERIMENTAL;
  snprintf(items[0].game_id, sizeof(items[0].game_id), "%s", "PCSA00011");
  snprintf(items[0].filename, sizeof(items[0].filename), "%s", "PCSA00011_older.tar");
  items[0].timestamp_unix = 100;

  sync_save_descriptor_init(&items[1]);
  items[1].platform = SYNC_SAVE_PLATFORM_VITA_NATIVE_EXPERIMENTAL;
  snprintf(items[1].game_id, sizeof(items[1].game_id), "%s", "PCSA00011");
  snprintf(items[1].filename, sizeof(items[1].filename), "%s", "PCSA00011_newer.tar");
  items[1].timestamp_unix = 200;

  assert(sync_select_latest_local_per_game(items, 2, selected_mask, NULL) == 2);
  assert(selected_mask[0] == 1);
  assert(selected_mask[1] == 1);
}

static void test_platform_parse_keeps_existing_names(void) {
  assert(sync_save_platform_from_id("psOne") == SYNC_SAVE_PLATFORM_PSONE);
  assert(sync_save_platform_from_id("ps1") == SYNC_SAVE_PLATFORM_PSONE);
  assert(sync_save_platform_from_id("psVita") == SYNC_SAVE_PLATFORM_VITA_NATIVE_EXPERIMENTAL);
  assert(sync_save_platform_from_id("vita") == SYNC_SAVE_PLATFORM_VITA_NATIVE_EXPERIMENTAL);
  assert(sync_save_platform_from_id("unknown") == SYNC_SAVE_PLATFORM_UNKNOWN);
}

int main(void) {
  test_default_descriptor_is_psone();
  test_vita_native_descriptor_policy();
  test_vita_native_saves_do_not_use_ps1_latest_slot_rule();
  test_platform_parse_keeps_existing_names();

  printf("All sync_platform tests passed.\n");
  return 0;
}
