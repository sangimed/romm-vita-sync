#include "sfo_parser.h"

#include <psp2/io/fcntl.h>

#include <stdint.h>
#include <string.h>

/*
 * SFO binary format structures.
 * See: https://www.psdevwiki.com/ps3/PARAM.SFO
 */

#define SFO_HEADER_SIZE 20
#define SFO_INDEX_ENTRY_SIZE 16
#define SFO_MAX_FILE_SIZE (64 * 1024)

static uint16_t read_le16(const unsigned char *p) {
  return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_le32(const unsigned char *p) {
  return (uint32_t)p[0] |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

/*
 * Reads an SFO file entirely into a buffer.
 * Returns number of bytes read, or negative on error.
 */
static int read_sfo_file(const char *path, unsigned char *buf, size_t buf_size) {
  SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0) {
    return fd;
  }

  int total = 0;
  while ((size_t)total < buf_size) {
    int read = sceIoRead(fd, buf + total, (unsigned int)(buf_size - (size_t)total));
    if (read < 0) {
      sceIoClose(fd);
      return read;
    }
    if (read == 0) {
      break;
    }
    total += read;
  }

  sceIoClose(fd);
  return total;
}

/*
 * Reads a PARAM.SFO file and extracts the value for the requested key.
 * Returns 0 on success, negative on error.
 */
int sfo_read_key(const char *sfo_path, const char *key_name, char *out_value, size_t out_size) {
  if ((sfo_path == NULL) || (key_name == NULL) || (out_value == NULL) || (out_size == 0)) {
    return -1;
  }

  out_value[0] = '\0';

  unsigned char buf[SFO_MAX_FILE_SIZE];
  int file_size = read_sfo_file(sfo_path, buf, sizeof(buf));
  if (file_size < SFO_HEADER_SIZE) {
    return -2;
  }

  /* Verify magic "\0PSF" */
  if (buf[0] != 0x00 || buf[1] != 'P' || buf[2] != 'S' || buf[3] != 'F') {
    return -3;
  }

  uint32_t key_table = read_le32(buf + 8);
  uint32_t data_table = read_le32(buf + 12);
  uint32_t entry_count = read_le32(buf + 16);

  size_t index_start = SFO_HEADER_SIZE;
  size_t index_end = index_start + ((size_t)entry_count * SFO_INDEX_ENTRY_SIZE);

  if (index_end > (size_t)file_size || key_table > (size_t)file_size ||
      data_table > (size_t)file_size || key_table < SFO_HEADER_SIZE ||
      data_table < SFO_HEADER_SIZE) {
    return -4;
  }

  for (uint32_t i = 0; i < entry_count; ++i) {
    size_t entry_off = index_start + ((size_t)i * SFO_INDEX_ENTRY_SIZE);
    const unsigned char *entry = buf + entry_off;

    uint16_t key_off_rel = read_le16(entry + 0);
    uint32_t data_len = read_le32(entry + 4);
    uint32_t data_off_rel = read_le32(entry + 12);

    uint32_t k_off = key_table + key_off_rel;
    if (k_off >= (uint32_t)file_size) {
      continue;
    }

    const char *key = (const char *)(buf + k_off);
    size_t key_space = (size_t)file_size - (size_t)k_off;
    size_t key_len = strnlen(key, key_space);
    if (key_len == key_space) {
      continue;
    }

    if (strcmp(key, key_name) == 0) {
      uint32_t d_off = data_table + data_off_rel;

      if (d_off > (uint32_t)file_size || data_len > (uint32_t)file_size ||
          d_off + data_len > (uint32_t)file_size) {
        return -5;
      }

      const char *value = (const char *)(buf + d_off);
      size_t value_len = strnlen(value, data_len);
      size_t copy_len = (value_len < (out_size - 1)) ? value_len : (out_size - 1);
      memcpy(out_value, value, copy_len);
      out_value[copy_len] = '\0';
      return 0;
    }
  }

  return -6; /* key not found */
}

/*
 * Reads a PARAM.SFO file and extracts the TITLE value.
 * Returns 0 on success, negative on error.
 */
int sfo_read_title(const char *sfo_path, char *out_title, size_t out_size) {
  return sfo_read_key(sfo_path, "TITLE", out_title, out_size);
}
