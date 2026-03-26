#include "vmp_srm_converter.h"

#include <stdio.h>
#include <string.h>

#define ROMM_COPY_BUFFER_SIZE 4096U

static int get_file_size(FILE *fp, long *out_size) {
  if ((fp == NULL) || (out_size == NULL)) {
    return ROMM_VMP_SRM_ERR_INVALID_ARGUMENT;
  }

  if (fseek(fp, 0, SEEK_END) != 0) {
    return ROMM_VMP_SRM_ERR_READ;
  }

  long size = ftell(fp);
  if (size < 0) {
    return ROMM_VMP_SRM_ERR_READ;
  }

  if (fseek(fp, 0, SEEK_SET) != 0) {
    return ROMM_VMP_SRM_ERR_READ;
  }

  *out_size = size;
  return ROMM_VMP_SRM_OK;
}

static int copy_payload(FILE *input, FILE *output, size_t bytes_to_copy) {
  if ((input == NULL) || (output == NULL)) {
    return ROMM_VMP_SRM_ERR_INVALID_ARGUMENT;
  }

  unsigned char buffer[ROMM_COPY_BUFFER_SIZE];
  size_t remaining = bytes_to_copy;

  while (remaining > 0U) {
    size_t chunk = remaining;
    if (chunk > sizeof(buffer)) {
      chunk = sizeof(buffer);
    }

    size_t read_count = fread(buffer, 1, chunk, input);
    if (read_count != chunk) {
      return ROMM_VMP_SRM_ERR_READ;
    }

    size_t write_count = fwrite(buffer, 1, chunk, output);
    if (write_count != chunk) {
      return ROMM_VMP_SRM_ERR_WRITE;
    }

    remaining -= chunk;
  }

  return ROMM_VMP_SRM_OK;
}

const char *vmp_srm_status_str(int status) {
  switch (status) {
    case ROMM_VMP_SRM_OK:
      return "ok";
    case ROMM_VMP_SRM_ERR_INVALID_ARGUMENT:
      return "invalid argument";
    case ROMM_VMP_SRM_ERR_OPEN_INPUT:
      return "cannot open input file";
    case ROMM_VMP_SRM_ERR_OPEN_OUTPUT:
      return "cannot open output file";
    case ROMM_VMP_SRM_ERR_READ:
      return "read failure";
    case ROMM_VMP_SRM_ERR_WRITE:
      return "write failure";
    case ROMM_VMP_SRM_ERR_UNSUPPORTED_SIZE:
      return "unsupported VMP size";
    case ROMM_VMP_SRM_ERR_PATH_TOO_LONG:
      return "output path too long";
    default:
      return "unknown error";
  }
}

int vmp_build_default_srm_path(const char *vmp_path, char *out_path, size_t out_path_size) {
  if ((vmp_path == NULL) || (out_path == NULL) || (out_path_size == 0U)) {
    return ROMM_VMP_SRM_ERR_INVALID_ARGUMENT;
  }

  size_t input_len = strlen(vmp_path);
  if (input_len + 1U > out_path_size) {
    return ROMM_VMP_SRM_ERR_PATH_TOO_LONG;
  }

  memcpy(out_path, vmp_path, input_len + 1U);

  if (input_len >= 4U) {
    char *ext = out_path + input_len - 4U;
    if (((ext[0] == '.') && ((ext[1] == 'v') || (ext[1] == 'V')) &&
         ((ext[2] == 'm') || (ext[2] == 'M')) && ((ext[3] == 'p') || (ext[3] == 'P')))) {
      ext[1] = 's';
      ext[2] = 'r';
      ext[3] = 'm';
      return ROMM_VMP_SRM_OK;
    }
  }

  if (input_len + 4U >= out_path_size) {
    return ROMM_VMP_SRM_ERR_PATH_TOO_LONG;
  }

  memcpy(out_path + input_len, ".srm", 5U);
  return ROMM_VMP_SRM_OK;
}

int vmp_to_srm_file(const char *vmp_path, const char *srm_path) {
  if ((vmp_path == NULL) || (srm_path == NULL) || (vmp_path[0] == '\0') || (srm_path[0] == '\0')) {
    return ROMM_VMP_SRM_ERR_INVALID_ARGUMENT;
  }

  if (strcmp(vmp_path, srm_path) == 0) {
    return ROMM_VMP_SRM_ERR_INVALID_ARGUMENT;
  }

  FILE *input = fopen(vmp_path, "rb");
  if (input == NULL) {
    return ROMM_VMP_SRM_ERR_OPEN_INPUT;
  }

  int status = ROMM_VMP_SRM_OK;
  FILE *output = NULL;

  do {
    long file_size = 0;
    status = get_file_size(input, &file_size);
    if (status != ROMM_VMP_SRM_OK) {
      break;
    }

    if ((unsigned long)file_size != (unsigned long)ROMM_PS1_VMP_SIZE) {
      status = ROMM_VMP_SRM_ERR_UNSUPPORTED_SIZE;
      break;
    }

    if (fseek(input, (long)ROMM_VMP_HEADER_SIZE, SEEK_SET) != 0) {
      status = ROMM_VMP_SRM_ERR_READ;
      break;
    }

    output = fopen(srm_path, "wb");
    if (output == NULL) {
      status = ROMM_VMP_SRM_ERR_OPEN_OUTPUT;
      break;
    }

    status = copy_payload(input, output, ROMM_PS1_SRM_SIZE);
    if (status != ROMM_VMP_SRM_OK) {
      break;
    }

    if (fflush(output) != 0) {
      status = ROMM_VMP_SRM_ERR_WRITE;
      break;
    }
  } while (0);

  if (output != NULL) {
    fclose(output);
    if (status != ROMM_VMP_SRM_OK) {
      remove(srm_path);
    }
  }

  fclose(input);
  return status;
}