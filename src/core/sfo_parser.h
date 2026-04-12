#ifndef SFO_PARSER_H
#define SFO_PARSER_H

#include <stddef.h>

/*
 * Reads a PARAM.SFO file and extracts the value for a given key.
 * Returns 0 on success, negative on error.
 */
int sfo_read_key(const char *sfo_path, const char *key_name, char *out_value, size_t out_size);

/*
 * Reads a PARAM.SFO file and extracts the value of the TITLE key.
 * Returns 0 on success, negative on error.
 */
int sfo_read_title(const char *sfo_path, char *out_title, size_t out_size);

#endif
