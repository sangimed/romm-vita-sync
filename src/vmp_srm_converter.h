#ifndef VMP_SRM_CONVERTER_H
#define VMP_SRM_CONVERTER_H

#include <stddef.h>

#define ROMM_VMP_HEADER_SIZE 128U
#define ROMM_PS1_SRM_SIZE 131072U
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
 * Converts a VMP memory card file to its raw SRM payload.
 * Version 1 is intentionally strict and only accepts standard 131200-byte VMP files.
 */
int vmp_to_srm_file(const char *vmp_path, const char *srm_path);

#endif