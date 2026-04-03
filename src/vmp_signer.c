#include "vmp_signer.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "aes.h"
#include "sha1.h"
#include "vmp_srm_converter.h"

#define ROMM_VMP_SEED_OFFSET 0x0CU
#define ROMM_VMP_HASH_OFFSET 0x20U

static const uint8_t kVmpKey[16] = {
    0xAB, 0x5A, 0xBC, 0x9F, 0xC1, 0xF4, 0x9D, 0xE6,
    0xA0, 0x51, 0xDB, 0xAE, 0xFA, 0x51, 0x88, 0x59};

static const uint8_t kVmpIv[16] = {
    0xB3, 0x0F, 0xFE, 0xED, 0xB7, 0xDC, 0x5E, 0xB7,
    0x13, 0x3D, 0xA6, 0x0D, 0x1B, 0x6B, 0x2C, 0xDC};

/*
 * XORs each byte of a buffer with a constant byte value.
 */
static void xor_with_byte(uint8_t *buffer, uint8_t value, size_t length) {
  if (buffer == NULL) {
    return;
  }

  for (size_t i = 0U; i < length; ++i) {
    buffer[i] ^= value;
  }
}

/*
 * Returns non-zero when the VMP magic bytes are present.
 */
static int has_vmp_magic(const uint8_t *buffer, size_t length) {
  if ((buffer == NULL) || (length < 4U)) {
    return 0;
  }

  return (buffer[0] == 0x00U) &&
         (buffer[1] == 0x50U) &&
         (buffer[2] == 0x4DU) &&
         (buffer[3] == 0x56U);
}

/*
 * Recomputes signature bytes in the loaded VMP buffer.
 */
static int sign_vmp_buffer(uint8_t *buffer, size_t buffer_size) {
  if ((buffer == NULL) || (buffer_size != ROMM_PS1_VMP_SIZE)) {
    return ROMM_VMP_SIGNER_ERR_UNSUPPORTED_SIZE;
  }

  if (!has_vmp_magic(buffer, buffer_size)) {
    return ROMM_VMP_SIGNER_ERR_UNSUPPORTED_FORMAT;
  }

  struct AES_ctx aes_ctx;
  AES_init_ctx_iv(&aes_ctx, kVmpKey, kVmpIv);

  uint8_t salt[0x40];
  uint8_t work[0x14];
  uint8_t *seed = buffer + ROMM_VMP_SEED_OFFSET;

  memcpy(work, seed, 0x10U);
  AES_ECB_decrypt(&aes_ctx, work);
  memcpy(salt, work, 0x10U);

  memcpy(work, seed, 0x10U);
  AES_ECB_encrypt(&aes_ctx, work);
  memcpy(salt + 0x10U, work, 0x10U);

  XorWithIv(salt, kVmpIv);

  memset(work, 0xFF, sizeof(work));
  memcpy(work, seed + 0x10U, 0x4U);
  XorWithIv(salt + 0x10U, work);

  memset(salt + 0x14U, 0, sizeof(salt) - 0x14U);
  xor_with_byte(salt, 0x36U, sizeof(salt));

  SHA1_CTX sha1_ctx_1;
  SHA1Init(&sha1_ctx_1);
  SHA1Update(&sha1_ctx_1, salt, sizeof(salt));

  memset(buffer + ROMM_VMP_HASH_OFFSET, 0, 0x14U);
  SHA1Update(&sha1_ctx_1, buffer, (uint32_t)buffer_size);
  SHA1Final(work, &sha1_ctx_1);

  xor_with_byte(salt, 0x6AU, sizeof(salt));

  SHA1_CTX sha1_ctx_2;
  SHA1Init(&sha1_ctx_2);
  SHA1Update(&sha1_ctx_2, salt, sizeof(salt));
  SHA1Update(&sha1_ctx_2, work, sizeof(work));
  SHA1Final(buffer + ROMM_VMP_HASH_OFFSET, &sha1_ctx_2);

  return ROMM_VMP_SIGNER_OK;
}

/*
 * Recomputes and writes the VMP signature in-place.
 */
int vmp_sign_file_in_place(const char *vmp_path) {
  if ((vmp_path == NULL) || (vmp_path[0] == '\0')) {
    return ROMM_VMP_SIGNER_ERR_INVALID_ARGUMENT;
  }

  FILE *file = fopen(vmp_path, "rb+");
  if (file == NULL) {
    return ROMM_VMP_SIGNER_ERR_OPEN_INPUT;
  }

  int status = ROMM_VMP_SIGNER_OK;
  uint8_t buffer[ROMM_PS1_VMP_SIZE];

  do {
    if (fseek(file, 0, SEEK_END) != 0) {
      status = ROMM_VMP_SIGNER_ERR_READ;
      break;
    }

    long file_size = ftell(file);
    if (file_size < 0L) {
      status = ROMM_VMP_SIGNER_ERR_READ;
      break;
    }
    if ((unsigned long)file_size != (unsigned long)ROMM_PS1_VMP_SIZE) {
      status = ROMM_VMP_SIGNER_ERR_UNSUPPORTED_SIZE;
      break;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
      status = ROMM_VMP_SIGNER_ERR_READ;
      break;
    }

    size_t read_count = fread(buffer, 1, sizeof(buffer), file);
    if (read_count != sizeof(buffer)) {
      status = ROMM_VMP_SIGNER_ERR_READ;
      break;
    }

    status = sign_vmp_buffer(buffer, sizeof(buffer));
    if (status != ROMM_VMP_SIGNER_OK) {
      break;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
      status = ROMM_VMP_SIGNER_ERR_WRITE;
      break;
    }

    size_t write_count = fwrite(buffer, 1, sizeof(buffer), file);
    if (write_count != sizeof(buffer)) {
      status = ROMM_VMP_SIGNER_ERR_WRITE;
      break;
    }

    if (fflush(file) != 0) {
      status = ROMM_VMP_SIGNER_ERR_WRITE;
      break;
    }
  } while (0);

  fclose(file);
  return status;
}

/*
 * Returns a short message for a signer status code.
 */
const char *vmp_signer_status_str(int status) {
  switch (status) {
    case ROMM_VMP_SIGNER_OK:
      return "ok";
    case ROMM_VMP_SIGNER_ERR_INVALID_ARGUMENT:
      return "invalid argument";
    case ROMM_VMP_SIGNER_ERR_OPEN_INPUT:
      return "cannot open input file";
    case ROMM_VMP_SIGNER_ERR_OPEN_OUTPUT:
      return "cannot open output file";
    case ROMM_VMP_SIGNER_ERR_READ:
      return "read failure";
    case ROMM_VMP_SIGNER_ERR_WRITE:
      return "write failure";
    case ROMM_VMP_SIGNER_ERR_UNSUPPORTED_SIZE:
      return "unsupported file size";
    case ROMM_VMP_SIGNER_ERR_UNSUPPORTED_FORMAT:
      return "unsupported VMP format";
    default:
      return "unknown error";
  }
}
