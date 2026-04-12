#ifndef VMP_SIGNER_H
#define VMP_SIGNER_H

#include <stddef.h>

#define ROMM_VMP_SIGNER_OK 0
#define ROMM_VMP_SIGNER_ERR_INVALID_ARGUMENT -1
#define ROMM_VMP_SIGNER_ERR_OPEN_INPUT -2
#define ROMM_VMP_SIGNER_ERR_OPEN_OUTPUT -3
#define ROMM_VMP_SIGNER_ERR_READ -4
#define ROMM_VMP_SIGNER_ERR_WRITE -5
#define ROMM_VMP_SIGNER_ERR_UNSUPPORTED_SIZE -6
#define ROMM_VMP_SIGNER_ERR_UNSUPPORTED_FORMAT -7

/*
 * Recomputes and writes the VMP signature in-place.
 */
int vmp_sign_file_in_place(const char *vmp_path);

/*
 * Returns a short message for a signer status code.
 */
const char *vmp_signer_status_str(int status);

#endif
