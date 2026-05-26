#include "vita_native_save_policy.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_export_requires_keystone_signature_metadata(void) {
  char reason[128];
  int ok = vita_native_save_container_is_exportable(1, 0, 1, reason, sizeof(reason));

  assert(ok == 0);
  assert(strstr(reason, "keystone") != NULL);
  assert(strstr(reason, "signature") != NULL);
}

static void test_export_accepts_complete_signed_container_metadata(void) {
  char reason[128];
  int ok = vita_native_save_container_is_exportable(1, 1, 1, reason, sizeof(reason));

  assert(ok == 1);
  assert(strstr(reason, "exportable") != NULL);
  assert(strstr(reason, "keystone") != NULL);
}

static void test_restore_policy_describes_backup_first_raw_archive_restore(void) {
  const char *reason = vita_native_save_restore_safety_notice();

  assert(reason != NULL);
  assert(strstr(reason, "raw PFS archives") != NULL);
  assert(strstr(reason, "keystone") != NULL);
  assert(strstr(reason, "back up") != NULL);
}

static void test_vita3k_import_notice_points_to_decrypted_export(void) {
  const char *notice = vita_native_save_vita3k_import_notice();

  assert(notice != NULL);
  assert(strstr(notice, "not direct Vita3K imports") != NULL);
  assert(strstr(notice, "Open decrypted") != NULL);
  assert(strstr(notice, "SlotParam") != NULL);
}

static void test_vita_archive_filename_timestamp_parsing(void) {
  int64_t timestamp = 0;

  assert(vita_native_save_archive_timestamp_from_filename("PCSF00438_1777323656.tar", &timestamp) == 0);
  assert(timestamp == 1777323656);

  timestamp = 0;
  assert(vita_native_save_archive_timestamp_from_filename(
             "ux0:data/romm-vita-sync/cache/vita-native/PCSF00438_raw-pfs-backup_1777751486.tar",
             &timestamp) == 0);
  assert(timestamp == 1777751486);

  assert(vita_native_save_archive_timestamp_from_filename("PCSF00438_raw-pfs-backup_abc.tar", &timestamp) < 0);
  assert(vita_native_save_archive_timestamp_from_filename("SCEVMC0_1777751486.srm", &timestamp) < 0);
}

static void test_vita_restore_candidate_preflight_accepts_matching_archives(void) {
  char reason[128];
  int ok = vita_native_save_archive_is_restore_candidate(
      "PCSF00438_raw-pfs-backup_1777751486.tar",
      "PCSF00438",
      reason,
      sizeof(reason));

  assert(ok == 1);
  assert(strstr(reason, "candidate") != NULL);

  ok = vita_native_save_archive_is_restore_candidate(
      "ux0:data/romm-vita-sync/cache/vita-native/pcsf00438_1777751486.TAR",
      "PCSF00438",
      reason,
      sizeof(reason));

  assert(ok == 1);
}

static void test_vita_restore_candidate_preflight_rejects_mismatches(void) {
  char reason[128];

  assert(vita_native_save_archive_is_restore_candidate(NULL, "PCSF00438", reason, sizeof(reason)) == 0);
  assert(strstr(reason, "missing") != NULL);

  assert(vita_native_save_archive_is_restore_candidate("PCSF00438_1777751486.srm", "PCSF00438", reason, sizeof(reason)) == 0);
  assert(strstr(reason, ".tar") != NULL);

  assert(vita_native_save_archive_is_restore_candidate("PCSF00438_raw-pfs-backup_abc.tar", "PCSF00438", reason, sizeof(reason)) == 0);
  assert(strstr(reason, "valid source timestamp") != NULL);

  assert(vita_native_save_archive_is_restore_candidate("PCSF00438_raw-pfs-backup_1777751486.tar", "PCSA00011", reason, sizeof(reason)) == 0);
  assert(strstr(reason, "does not match") != NULL);

  assert(vita_native_save_archive_is_restore_candidate("NPXS10000_1777751486.tar", "PCSF00438", reason, sizeof(reason)) == 0);
  assert(strstr(reason, "official Vita") != NULL);
}

static void test_vita_archive_member_title_id_rejects_unsafe_paths(void) {
  char title_id[16];

  assert(vita_native_save_archive_member_title_id("PCSF00438/", title_id, sizeof(title_id)) == 0);
  assert(strcmp(title_id, "PCSF00438") == 0);

  assert(vita_native_save_archive_member_title_id("PCSF00438/sce_sys/keystone", title_id, sizeof(title_id)) == 0);
  assert(strcmp(title_id, "PCSF00438") == 0);

  assert(vita_native_save_archive_member_title_id("/PCSF00438/sce_sys/keystone", title_id, sizeof(title_id)) < 0);
  assert(vita_native_save_archive_member_title_id("PCSF00438/../keystone", title_id, sizeof(title_id)) < 0);
  assert(vita_native_save_archive_member_title_id("PCSF00438//keystone", title_id, sizeof(title_id)) < 0);
  assert(vita_native_save_archive_member_title_id("PCSF00438\\sce_sys\\keystone", title_id, sizeof(title_id)) < 0);
  assert(vita_native_save_archive_member_title_id("PCSF00438:sce_sys/keystone", title_id, sizeof(title_id)) < 0);
  assert(vita_native_save_archive_member_title_id("NPXS10000/sce_sys/keystone", title_id, sizeof(title_id)) < 0);
}

static void test_official_vita_game_title_id_policy(void) {
  assert(vita_native_save_title_id_is_official_game("PCSA00011") == 1);
  assert(vita_native_save_title_id_is_official_game("PCSE01234") == 1);
  assert(vita_native_save_title_id_is_official_game("PCSH99999") == 1);
  assert(vita_native_save_title_id_is_official_game("pcsg00123") == 1);

  assert(vita_native_save_title_id_is_official_game(NULL) == 0);
  assert(vita_native_save_title_id_is_official_game("") == 0);
  assert(vita_native_save_title_id_is_official_game("VITASHELL") == 0);
  assert(vita_native_save_title_id_is_official_game("MOLECULA") == 0);
  assert(vita_native_save_title_id_is_official_game("NPXS10000") == 0);
  assert(vita_native_save_title_id_is_official_game("PCSI00001") == 0);
  assert(vita_native_save_title_id_is_official_game("PCSA0001") == 0);
  assert(vita_native_save_title_id_is_official_game("PCSA0001A") == 0);
}

int main(void) {
  test_export_requires_keystone_signature_metadata();
  test_export_accepts_complete_signed_container_metadata();
  test_restore_policy_describes_backup_first_raw_archive_restore();
  test_vita3k_import_notice_points_to_decrypted_export();
  test_vita_archive_filename_timestamp_parsing();
  test_vita_restore_candidate_preflight_accepts_matching_archives();
  test_vita_restore_candidate_preflight_rejects_mismatches();
  test_vita_archive_member_title_id_rejects_unsafe_paths();
  test_official_vita_game_title_id_policy();

  printf("All vita_native_save_policy tests passed.\n");
  return 0;
}
