#include <string.h>
#include <stdio.h>

#include "vmp_srm_converter.h"

static void print_usage(const char *program_name) {
  fprintf(stderr, "Usage: %s <input.vmp> [output.srm]\n", program_name);
}

int main(int argc, char *argv[]) {
  if ((argc >= 2) &&
      ((strcmp(argv[1], "--help") == 0) || (strcmp(argv[1], "-h") == 0))) {
    print_usage(argv[0]);
    return 0;
  }

  if ((argc < 2) || (argc > 3)) {
    print_usage(argv[0]);
    return 1;
  }

  const char *input_path = argv[1];
  char output_path_buffer[1024];
  const char *output_path = NULL;

  if (argc == 3) {
    output_path = argv[2];
  } else {
    int path_status = vmp_build_default_srm_path(input_path, output_path_buffer, sizeof(output_path_buffer));
    if (path_status != ROMM_VMP_SRM_OK) {
      fprintf(stderr, "Failed to derive output path: %s\n", vmp_srm_status_str(path_status));
      return 1;
    }
    output_path = output_path_buffer;
  }

  int status = vmp_to_srm_file(input_path, output_path);
  if (status != ROMM_VMP_SRM_OK) {
    fprintf(stderr, "Conversion failed: %s\n", vmp_srm_status_str(status));
    return 1;
  }

  printf("Converted %s -> %s\n", input_path, output_path);
  return 0;
}