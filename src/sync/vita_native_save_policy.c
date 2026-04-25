#include "vita_native_save_policy.h"

#include <stdio.h>

static void set_reason(char *out_reason, size_t out_reason_size, const char *reason) {
  if ((out_reason == NULL) || (out_reason_size == 0U)) {
    return;
  }

  snprintf(out_reason, out_reason_size, "%s", reason != NULL ? reason : "");
}

int vita_native_save_container_is_exportable(
    int has_sce_sys,
    int has_keystone,
    int has_param_sfo,
    char *out_reason,
    size_t out_reason_size) {
  if (!has_sce_sys) {
    set_reason(out_reason, out_reason_size, "missing sce_sys metadata directory");
    return 0;
  }
  if (!has_param_sfo) {
    set_reason(out_reason, out_reason_size, "missing PARAM.SFO metadata");
    return 0;
  }
  if (!has_keystone) {
    set_reason(out_reason, out_reason_size, "missing keystone signature metadata required by Vita PFS");
    return 0;
  }

  set_reason(out_reason, out_reason_size, "exportable signed Vita container with keystone metadata");
  return 1;
}

static char ascii_upper(char value) {
  if ((value >= 'a') && (value <= 'z')) {
    return (char)(value - ('a' - 'A'));
  }
  return value;
}

static int ascii_digit(char value) {
  return (value >= '0') && (value <= '9');
}

int vita_native_save_title_id_is_official_game(const char *title_id) {
  if (title_id == NULL) {
    return 0;
  }

  if ((ascii_upper(title_id[0]) != 'P') ||
      (ascii_upper(title_id[1]) != 'C') ||
      (ascii_upper(title_id[2]) != 'S')) {
    return 0;
  }

  char region = ascii_upper(title_id[3]);
  if ((region < 'A') || (region > 'H')) {
    return 0;
  }

  for (int i = 4; i < 9; ++i) {
    if (!ascii_digit(title_id[i])) {
      return 0;
    }
  }

  return title_id[9] == '\0';
}

const char *vita_native_save_restore_unsupported_reason(void) {
  return "restore not supported yet for Vita native saves: PFS/keystone signature metadata must be preserved or regenerated safely";
}
