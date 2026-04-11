#include "vmp_srm_converter.h"

#include <stdio.h>
#include <string.h>

#define ROMM_COPY_BUFFER_SIZE 4096U

/*
 * Converts one ASCII character to lowercase for case-insensitive comparisons.
 */
static char ascii_lower(char value) {
  if ((value >= 'A') && (value <= 'Z')) {
    return (char)(value - 'A' + 'a');
  }

  return value;
}

/*
 * Returns file size while preserving file position at the beginning.
 */
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

/*
 * Copies an exact number of bytes from input stream to output stream.
 */
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

/*
 * Writes a fixed memory buffer to a stream.
 */
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

/*
 * Case-insensitive extension check helper.
 */
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

/*
 * Builds a default output path by swapping or appending file extensions.
 */
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

/*
 * Maps converter status codes to user-facing strings.
 */
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

/*
 * Builds default SRM output path from a VMP input path.
 */
int vmp_build_default_srm_path(const char *vmp_path, char *out_path, size_t out_path_size) {
  return build_default_output_path(vmp_path, ".vmp", ".srm", out_path, out_path_size);
}

/*
 * Builds default VMP output path from a SRM input path.
 */
int srm_build_default_vmp_path(const char *srm_path, char *out_path, size_t out_path_size) {
  return build_default_output_path(srm_path, ".srm", ".vmp", out_path, out_path_size);
}

/*
 * Maps one SRM payload size to the number of contained memory cards.
 */
static int srm_card_count_from_size(long input_size, int *out_card_count) {
  if (out_card_count == NULL) {
    return ROMM_VMP_SRM_ERR_INVALID_ARGUMENT;
  }

  *out_card_count = 0;
  if ((unsigned long)input_size == (unsigned long)ROMM_PS1_SRM_SIZE) {
    *out_card_count = 1;
    return ROMM_VMP_SRM_OK;
  }
  if ((unsigned long)input_size == (unsigned long)ROMM_PS1_DUAL_SRM_SIZE) {
    *out_card_count = 2;
    return ROMM_VMP_SRM_OK;
  }

  return ROMM_VMP_SRM_ERR_UNSUPPORTED_SIZE;
}

/*
 * Converts a standard 131200-byte VMP container into a 131072-byte SRM payload.
 * This is the standard single-card raw PS1 memory card format expected by
 * pcsx_rearmed, DuckStation, and other PS1 emulators.
 */
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

/*
 * Returns the number of memory cards stored in an SRM payload.
 */
int srm_get_card_count(const char *srm_path, int *out_card_count) {
  if ((srm_path == NULL) || (out_card_count == NULL) || (srm_path[0] == '\0')) {
    return ROMM_VMP_SRM_ERR_INVALID_ARGUMENT;
  }

  FILE *input = fopen(srm_path, "rb");
  if (input == NULL) {
    return ROMM_VMP_SRM_ERR_OPEN_INPUT;
  }

  int status = ROMM_VMP_SRM_OK;
  do {
    long input_size = 0;
    status = get_file_size(input, &input_size);
    if (status != ROMM_VMP_SRM_OK) {
      break;
    }

    status = srm_card_count_from_size(input_size, out_card_count);
  } while (0);

  fclose(input);
  return status;
}

/*
 * Rebuilds one VMP file from a selected SRM card using header bytes from a
 * trusted template.
 */
int srm_card_to_vmp_file(
    const char *srm_path,
    int card_index,
    const char *template_vmp_path,
    const char *vmp_path) {
  if ((srm_path == NULL) || (template_vmp_path == NULL) || (vmp_path == NULL) ||
      (srm_path[0] == '\0') || (template_vmp_path[0] == '\0') || (vmp_path[0] == '\0') ||
      (card_index < 0)) {
    return ROMM_VMP_SRM_ERR_INVALID_ARGUMENT;
  }

  if (strcmp(srm_path, vmp_path) == 0) {
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

    int card_count = 0;
    status = srm_card_count_from_size(input_size, &card_count);
    if (status != ROMM_VMP_SRM_OK) {
      break;
    }

    if (card_index >= card_count) {
      status = ROMM_VMP_SRM_ERR_INVALID_ARGUMENT;
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

    long payload_offset = (long)(card_index * (int)ROMM_PS1_SRM_SIZE);
    if (fseek(input, payload_offset, SEEK_SET) != 0) {
      status = ROMM_VMP_SRM_ERR_READ;
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

/*
 * Rebuilds the first VMP file from an SRM payload using header bytes from a
 * trusted template.
 */
int srm_to_vmp_file(const char *srm_path, const char *template_vmp_path, const char *vmp_path) {
  return srm_card_to_vmp_file(srm_path, 0, template_vmp_path, vmp_path);
}
