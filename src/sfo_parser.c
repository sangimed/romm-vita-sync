#include "sfo_parser.h"

#include <psp2/io/fcntl.h>
#include <psp2/types.h>

#include <string.h>

/*
 * SFO binary format structures.
 * See: https://www.psdevwiki.com/ps3/PARAM.SFO
 */

typedef struct SfoHeader {
  unsigned char magic[4]; /* "\0PSF" */
  unsigned char version[4];
  uint32_t key_table_offset;
  uint32_t data_table_offset;
  uint32_t entry_count;
} __attribute__((packed)) SfoHeader;

typedef struct SfoIndexEntry {
  uint16_t key_offset;
  uint16_t data_format;
  uint32_t data_len;
  uint32_t data_max_len;
  uint32_t data_offset;
} __attribute__((packed)) SfoIndexEntry;

#define SFO_MAX_FILE_SIZE 4096

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
  if (file_size < (int)sizeof(SfoHeader)) {
    return -2;
  }

  SfoHeader *header = (SfoHeader *)buf;

  /* Verify magic "\0PSF" */
  if (header->magic[0] != 0x00 || header->magic[1] != 'P' ||
      header->magic[2] != 'S' || header->magic[3] != 'F') {
    return -3;
  }

  uint32_t entry_count = header->entry_count;
  uint32_t key_table = header->key_table_offset;
  uint32_t data_table = header->data_table_offset;

  /* Sanity: index table starts right after header */
  size_t index_start = sizeof(SfoHeader);
  size_t index_end = index_start + entry_count * sizeof(SfoIndexEntry);

  if (index_end > (size_t)file_size || key_table > (size_t)file_size ||
      data_table > (size_t)file_size) {
    return -4;
  }

  SfoIndexEntry *entries = (SfoIndexEntry *)(buf + index_start);

  for (uint32_t i = 0; i < entry_count; ++i) {
    uint32_t k_off = key_table + entries[i].key_offset;
    if (k_off >= (uint32_t)file_size) {
      continue;
    }

    const char *key = (const char *)(buf + k_off);

    if (strcmp(key, key_name) == 0) {
      uint32_t d_off = data_table + entries[i].data_offset;
      uint32_t d_len = entries[i].data_len;

      if (d_off + d_len > (uint32_t)file_size) {
        return -5;
      }

      const char *value = (const char *)(buf + d_off);
      size_t copy_len = (d_len < out_size) ? d_len : (out_size - 1);
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
