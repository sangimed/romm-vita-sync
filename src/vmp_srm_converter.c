#include "vmp_srm_converter.h"

#include <stdio.h>
#include <string.h>

#define ROMM_COPY_BUFFER_SIZE 4096U

static char ascii_lower(char value) {
  if ((value >= 'A') && (value <= 'Z')) {
    return (char)(value - 'A' + 'a');
  }

  return value;
}

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

static int write_buffer(FILE *output, const unsigned char *buffer, size_t bytes_to_write) {
  if ((output == NULL) || (buffer == NULL)) {
    return ROMM_VMP_SRM_ERR_INVALID_ARGUMENT;
  }

  size_t write_count = fwrite(buffer, 1, bytes_to_write, output);
  if (write_count != bytes_to_write) {
    return ROMM_VMP_SRM_ERR_WRITE;
  }

  return ROMM_VMP_SRM_OK;
}

static int path_has_extension(const char *path, const char *extension) {
  if ((path == NULL) || (extension == NULL)) {
    return 0;
  }

  size_t path_len = strlen(path);
  size_t extension_len = strlen(extension);

  if (path_len < extension_len) {
    return 0;
  }

  const char *path_extension = path + path_len - extension_len;
  for (size_t index = 0; index < extension_len; ++index) {
    if (ascii_lower(path_extension[index]) != ascii_lower(extension[index])) {
      return 0;
    }
  }

  return 1;
}

static int build_default_output_path(
    const char *input_path,
    const char *from_extension,
    const char *to_extension,
    char *out_path,
    size_t out_path_size) {
  if ((input_path == NULL) || (from_extension == NULL) || (to_extension == NULL) ||
      (out_path == NULL) || (out_path_size == 0U)) {
    return ROMM_VMP_SRM_ERR_INVALID_ARGUMENT;
  }

  size_t input_len = strlen(input_path);
  size_t to_extension_len = strlen(to_extension);

  if (path_has_extension(input_path, from_extension)) {
    size_t from_extension_len = strlen(from_extension);
    size_t base_len = input_len - from_extension_len;
    size_t output_len = base_len + to_extension_len;

    if (output_len + 1U > out_path_size) {
      return ROMM_VMP_SRM_ERR_PATH_TOO_LONG;
    }

    memcpy(out_path, input_path, base_len);
    memcpy(out_path + base_len, to_extension, to_extension_len + 1U);
    return ROMM_VMP_SRM_OK;
  }

  if (input_len + to_extension_len + 1U > out_path_size) {
    return ROMM_VMP_SRM_ERR_PATH_TOO_LONG;
  }

  memcpy(out_path, input_path, input_len);
  memcpy(out_path + input_len, to_extension, to_extension_len + 1U);
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
      return "unsupported file size";
    case ROMM_VMP_SRM_ERR_PATH_TOO_LONG:
      return "output path too long";
    default:
      return "unknown error";
  }
}

int vmp_build_default_srm_path(const char *vmp_path, char *out_path, size_t out_path_size) {
  return build_default_output_path(vmp_path, ".vmp", ".srm", out_path, out_path_size);
}

int srm_build_default_vmp_path(const char *srm_path, char *out_path, size_t out_path_size) {
  return build_default_output_path(srm_path, ".srm", ".vmp", out_path, out_path_size);
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

int srm_to_vmp_file(const char *srm_path, const char *template_vmp_path, const char *vmp_path) {
  if ((srm_path == NULL) || (template_vmp_path == NULL) || (vmp_path == NULL) ||
      (srm_path[0] == '\0') || (template_vmp_path[0] == '\0') || (vmp_path[0] == '\0')) {
    return ROMM_VMP_SRM_ERR_INVALID_ARGUMENT;
  }

  if ((strcmp(srm_path, vmp_path) == 0) || (strcmp(template_vmp_path, vmp_path) == 0)) {
    return ROMM_VMP_SRM_ERR_INVALID_ARGUMENT;
  }

  FILE *input = fopen(srm_path, "rb");
  if (input == NULL) {
    return ROMM_VMP_SRM_ERR_OPEN_INPUT;
  }

  int status = ROMM_VMP_SRM_OK;
  FILE *template_vmp = NULL;
  FILE *output = NULL;

  do {
    long input_size = 0;
    status = get_file_size(input, &input_size);
    if (status != ROMM_VMP_SRM_OK) {
      break;
    }

    if ((unsigned long)input_size != (unsigned long)ROMM_PS1_SRM_SIZE) {
      status = ROMM_VMP_SRM_ERR_UNSUPPORTED_SIZE;
      break;
    }

    template_vmp = fopen(template_vmp_path, "rb");
    if (template_vmp == NULL) {
      status = ROMM_VMP_SRM_ERR_OPEN_INPUT;
      break;
    }

    long template_size = 0;
    status = get_file_size(template_vmp, &template_size);
    if (status != ROMM_VMP_SRM_OK) {
      break;
    }

    if ((unsigned long)template_size != (unsigned long)ROMM_PS1_VMP_SIZE) {
      status = ROMM_VMP_SRM_ERR_UNSUPPORTED_SIZE;
      break;
    }

    unsigned char header[ROMM_VMP_HEADER_SIZE];
    size_t header_read_count = fread(header, 1, sizeof(header), template_vmp);
    if (header_read_count != sizeof(header)) {
      status = ROMM_VMP_SRM_ERR_READ;
      break;
    }

    output = fopen(vmp_path, "wb");
    if (output == NULL) {
      status = ROMM_VMP_SRM_ERR_OPEN_OUTPUT;
      break;
    }

    status = write_buffer(output, header, sizeof(header));
    if (status != ROMM_VMP_SRM_OK) {
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
      remove(vmp_path);
    }
  }

  if (template_vmp != NULL) {
    fclose(template_vmp);
  }

  fclose(input);
  return status;
}
