#ifndef VMP_SRM_CONVERTER_H
#define VMP_SRM_CONVERTER_H

#include <stddef.h>

#define ROMM_VMP_HEADER_SIZE 128U
#define ROMM_PS1_SRM_SIZE 131072U
#define ROMM_PS1_DUAL_SRM_SIZE (ROMM_PS1_SRM_SIZE * 2U)
#define ROMM_PS1_VMP_SIZE (ROMM_VMP_HEADER_SIZE + ROMM_PS1_SRM_SIZE)

#define ROMM_VMP_SRM_OK 0
#define ROMM_VMP_SRM_ERR_INVALID_ARGUMENT -1
#define ROMM_VMP_SRM_ERR_OPEN_INPUT -2
#define ROMM_VMP_SRM_ERR_OPEN_OUTPUT -3
#define ROMM_VMP_SRM_ERR_READ -4
#define ROMM_VMP_SRM_ERR_WRITE -5
#define ROMM_VMP_SRM_ERR_UNSUPPORTED_SIZE -6
#define ROMM_VMP_SRM_ERR_PATH_TOO_LONG -7

/*
 * Returns a short human-readable description for a converter status code.
 */
const char *vmp_srm_status_str(int status);

/*
 * Builds a default .srm output path from an input VMP path.
 * Example: ux0:/foo/SCEVMC0.VMP -> ux0:/foo/SCEVMC0.srm
 */
int vmp_build_default_srm_path(const char *vmp_path, char *out_path, size_t out_path_size);

/*
 * Builds a default .vmp output path from an input SRM path.
 * Example: ./save.srm -> ./save.vmp
 */
int srm_build_default_vmp_path(const char *srm_path, char *out_path, size_t out_path_size);

/*
 * Converts a VMP memory card file to a standard 131072-byte SRM payload.
 * This is the raw PS1 memory card format expected by pcsx_rearmed, DuckStation,
 * and other PS1 emulators. Only accepts standard 131200-byte VMP files.
 */
int vmp_to_srm_file(const char *vmp_path, const char *srm_path);

/*
 * Returns the number of memory cards stored in an SRM payload.
 * Supported values are 1 (131072 bytes) and 2 (262144 bytes).
 */
int srm_get_card_count(const char *srm_path, int *out_card_count);

/*
 * Converts one card from a raw SRM payload back to a VMP memory card file by
 * reusing the first 128 bytes from a known-good template VMP file.
 * card_index is zero-based:
 * - 0 = memory card 1
 * - 1 = memory card 2
 * Accepts both 131072-byte (single-card) and 262144-byte (dual-card) SRM
 * files, but rejects card indexes that are not present in the payload.
 */
int srm_card_to_vmp_file(
    const char *srm_path,
    int card_index,
    const char *template_vmp_path,
    const char *vmp_path);

/*
 * Converts the first card from a raw SRM payload back to a VMP memory card
 * file by reusing the first 128 bytes from a known-good template VMP file.
 * This convenience wrapper behaves like srm_card_to_vmp_file(..., 0, ...).
 */
int srm_to_vmp_file(const char *srm_path, const char *template_vmp_path, const char *vmp_path);

#endif
