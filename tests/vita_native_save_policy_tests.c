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

static void test_restore_policy_is_explicitly_unsupported_until_resigning_exists(void) {
  const char *reason = vita_native_save_restore_unsupported_reason();

  assert(reason != NULL);
  assert(strstr(reason, "restore not supported") != NULL);
  assert(strstr(reason, "PFS") != NULL);
  assert(strstr(reason, "keystone") != NULL);
}

int main(void) {
  test_export_requires_keystone_signature_metadata();
  test_export_accepts_complete_signed_container_metadata();
  test_restore_policy_is_explicitly_unsupported_until_resigning_exists();

  printf("All vita_native_save_policy tests passed.\n");
  return 0;
}
