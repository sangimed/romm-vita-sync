#include "game_matcher.h"
#include "sync_types.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static SyncSaveDescriptor make_local_item(const char *game_id, const char *title) {
  SyncSaveDescriptor item;
  sync_save_descriptor_init(&item);
  if (game_id != NULL) {
    snprintf(item.game_id, sizeof(item.game_id), "%s", game_id);
  }
  if (title != NULL) {
    snprintf(item.title, sizeof(item.title), "%s", title);
  }
  return item;
}

static GameMatcherRomCandidate make_candidate(
    int rom_id,
    const char *name,
    const char *fs_name_no_tags,
    const char *fs_name_no_ext,
    const char *serial,
    const char *serials,
    const char *alternative_names) {
  GameMatcherRomCandidate candidate;
  memset(&candidate, 0, sizeof(candidate));
  candidate.rom_id = rom_id;
  snprintf(candidate.platform_slug, sizeof(candidate.platform_slug), "%s", "psx");
  if (name != NULL) {
    snprintf(candidate.name, sizeof(candidate.name), "%s", name);
  }
  if (fs_name_no_tags != NULL) {
    snprintf(candidate.fs_name_no_tags, sizeof(candidate.fs_name_no_tags), "%s", fs_name_no_tags);
  }
  if (fs_name_no_ext != NULL) {
    snprintf(candidate.fs_name_no_ext, sizeof(candidate.fs_name_no_ext), "%s", fs_name_no_ext);
  }
  if (serial != NULL) {
    snprintf(candidate.serial, sizeof(candidate.serial), "%s", serial);
  }
  if (serials != NULL) {
    snprintf(candidate.serials, sizeof(candidate.serials), "%s", serials);
  }
  if (alternative_names != NULL) {
    snprintf(candidate.alternative_names, sizeof(candidate.alternative_names), "%s", alternative_names);
  }
  return candidate;
}

static void test_normalization_symbols_and_tags(void) {
  char normalized[ROMM_GAME_TITLE_LEN];

  game_matcher_normalize_title("R4 RIDGE RACER TYPE 4®", normalized, sizeof(normalized));
  assert(strcmp(normalized, "r4 ridge racer type 4") == 0);

  game_matcher_normalize_title("Médal Ôf Hônor™ [USA] (Disc 1)", normalized, sizeof(normalized));
  assert(strcmp(normalized, "medal of honor usa disc 1") == 0);

  game_matcher_normalize_title("Crash Bandicoot™ 3", normalized, sizeof(normalized));
  assert(strcmp(normalized, "crash bandicoot 3") == 0);
}

static void test_r4_matches_ridge_racer_type_4(void) {
  SyncSaveDescriptor local = make_local_item("SLPS01800", "R4 RIDGE RACER TYPE 4®");
  GameMatcherRomCandidate catalog[] = {
      make_candidate(4002, "Ridge Racer Type 3", "Ridge Racer Type 3", "Ridge Racer Type 3", NULL, NULL, "R3"),
      make_candidate(4003, "Ridge Racer Type 4", "Ridge Racer Type 4", "Ridge Racer Type 4", NULL, NULL, "R4|Ridge Racer Type 4")};

  GameMatcherResolution resolution;
  int rom_id = game_matcher_resolve_rom_id_with_details(&local, catalog, 2, &resolution);
  assert(rom_id == 4003);
  assert(resolution.rom_id == 4003);
  assert(resolution.stage == GAME_MATCHER_MATCH_STAGE_TITLE);
  assert(resolution.score > 0);
}

static void test_alternative_name_exact_match(void) {
  SyncSaveDescriptor local = make_local_item("", "R4");
  GameMatcherRomCandidate catalog[] = {
      make_candidate(4003, "Ridge Racer Type 4", "Ridge Racer Type 4", "Ridge Racer Type 4", NULL, NULL, "R4|Ridge Racer Type 4")};

  GameMatcherResolution resolution;
  int rom_id = game_matcher_resolve_rom_id_with_details(&local, catalog, 1, &resolution);
  assert(rom_id == 4003);
  assert(resolution.field == GAME_MATCHER_MATCH_FIELD_ALTERNATIVE_NAMES);
}

static void test_regression_metal_gear_solid(void) {
  SyncSaveDescriptor local = make_local_item("SLES01370", "Metal Gear Solid");
  GameMatcherRomCandidate catalog[] = {
      make_candidate(1001, "Metal Gear Solid", "Metal Gear Solid", "Metal Gear Solid", "SLES01370", "SLES01370", NULL)};

  GameMatcherResolution resolution;
  int rom_id = game_matcher_resolve_rom_id_with_details(&local, catalog, 1, &resolution);
  assert(rom_id == 1001);
}

static void test_regression_crash_bandicoot_3(void) {
  SyncSaveDescriptor local = make_local_item("SCES01420", "Crash Bandicoot™ 3");
  GameMatcherRomCandidate catalog[] = {
      make_candidate(2001, "Crash Bandicoot 3", "Crash Bandicoot 3", "Crash Bandicoot 3", NULL, NULL, NULL)};

  GameMatcherResolution resolution;
  int rom_id = game_matcher_resolve_rom_id_with_details(&local, catalog, 1, &resolution);
  assert(rom_id == 2001);
  assert(resolution.stage == GAME_MATCHER_MATCH_STAGE_TITLE);
}

static void test_diacritics_title_match(void) {
  SyncSaveDescriptor local = make_local_item("", "Médal Ôf Hônor™ [USA]");
  GameMatcherRomCandidate catalog[] = {
      make_candidate(3001, "Medal of Honor", "Medal of Honor", "Medal of Honor", NULL, NULL, NULL)};

  GameMatcherResolution resolution;
  int rom_id = game_matcher_resolve_rom_id_with_details(&local, catalog, 1, &resolution);
  assert(rom_id == 3001);
}

static void test_sequel_mismatch_penalty(void) {
  SyncSaveDescriptor local = make_local_item("", "Ridge Racer Type IV");
  GameMatcherRomCandidate catalog[] = {
      make_candidate(4002, "Ridge Racer Type 3", "Ridge Racer Type 3", "Ridge Racer Type 3", NULL, NULL, NULL),
      make_candidate(4003, "Ridge Racer Type 4", "Ridge Racer Type 4", "Ridge Racer Type 4", NULL, NULL, NULL)};

  GameMatcherResolution resolution;
  int rom_id = game_matcher_resolve_rom_id_with_details(&local, catalog, 2, &resolution);
  assert(rom_id == 4003);
  assert(resolution.stage == GAME_MATCHER_MATCH_STAGE_TITLE);
}

int main(void) {
  test_normalization_symbols_and_tags();
  test_r4_matches_ridge_racer_type_4();
  test_alternative_name_exact_match();
  test_regression_metal_gear_solid();
  test_regression_crash_bandicoot_3();
  test_diacritics_title_match();
  test_sequel_mismatch_penalty();

  printf("All game_matcher tests passed.\n");
  return 0;
}
