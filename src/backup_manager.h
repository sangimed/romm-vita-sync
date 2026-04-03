#ifndef BACKUP_MANAGER_H
#define BACKUP_MANAGER_H

#include <stddef.h>
#include <stdint.h>

#define BACKUP_MANAGER_OK 0
#define BACKUP_MANAGER_ERR_INVALID_ARGUMENT -1
#define BACKUP_MANAGER_ERR_PATH_TOO_LONG -2
#define BACKUP_MANAGER_ERR_CREATE_DIRECTORY -3
#define BACKUP_MANAGER_ERR_OPEN_SOURCE -4
#define BACKUP_MANAGER_ERR_OPEN_DESTINATION -5
#define BACKUP_MANAGER_ERR_READ -6
#define BACKUP_MANAGER_ERR_WRITE -7

/*
 * Creates a directory tree if needed.
 */
int backup_manager_ensure_directory(const char *directory_path);

/*
 * Copies source file to backup directory with deterministic timestamp suffix.
 * out_backup_path can be NULL when caller does not need the resulting path.
 */
int backup_manager_backup_file(
    const char *source_path,
    const char *backup_directory,
    int64_t timestamp_unix,
    char *out_backup_path,
    size_t out_backup_path_size);

/*
 * Returns human-readable backup manager status.
 */
const char *backup_manager_status_str(int status);

#endif
