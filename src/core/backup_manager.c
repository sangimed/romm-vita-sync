#include "backup_manager.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#define mkdir_native(path) _mkdir(path)
#else
#include <unistd.h>
#define mkdir_native(path) mkdir(path, 0777)
#endif

#define BACKUP_COPY_BUFFER_SIZE 4096U

/*
 * Returns non-zero when the character is considered a path separator.
 */
static int is_separator(char c) {
  return (c == '/') || (c == '\\');
}

/*
 * Detects Windows drive prefixes like "C:" or "D:\".
 * Used to avoid trying to create synthetic directories for drive roots.
 */
static int is_windows_drive_prefix(const char *path) {
  if (path == NULL) {
    return 0;
  }

  return (((path[0] >= 'A') && (path[0] <= 'Z')) ||
          ((path[0] >= 'a') && (path[0] <= 'z'))) &&
         (path[1] == ':') &&
         ((path[2] == '\0') || is_separator(path[2]));
}

/*
 * Checks whether a path currently exists and is a directory.
 */
static int is_existing_directory(const char *path) {
  if ((path == NULL) || (path[0] == '\0')) {
    return 0;
  }

  struct stat stat_info;
  if (stat(path, &stat_info) != 0) {
    return 0;
  }

  return S_ISDIR(stat_info.st_mode) != 0;
}

/*
 * Creates one directory level if it does not already exist.
 */
static int create_directory_one(const char *path) {
  if ((path == NULL) || (path[0] == '\0')) {
    return BACKUP_MANAGER_ERR_INVALID_ARGUMENT;
  }

  if (is_existing_directory(path)) {
    return BACKUP_MANAGER_OK;
  }

  if (mkdir_native(path) == 0) {
    return BACKUP_MANAGER_OK;
  }

  if (errno == EEXIST) {
    return BACKUP_MANAGER_OK;
  }

  return BACKUP_MANAGER_ERR_CREATE_DIRECTORY;
}

/*
 * Extracts the file name component from a full path.
 */
static const char *extract_filename(const char *path) {
  if ((path == NULL) || (path[0] == '\0')) {
    return NULL;
  }

  const char *last_forward = strrchr(path, '/');
  const char *last_backward = strrchr(path, '\\');
  const char *last_separator = last_forward;
  if ((last_backward != NULL) && ((last_separator == NULL) || (last_backward > last_separator))) {
    last_separator = last_backward;
  }

  if (last_separator == NULL) {
    return path;
  }

  return last_separator + 1;
}

/*
 * Performs a binary file copy from source to destination.
 * On failure, partially written output is removed.
 */
static int copy_file_contents(const char *source_path, const char *destination_path) {
  FILE *source = fopen(source_path, "rb");
  if (source == NULL) {
    return BACKUP_MANAGER_ERR_OPEN_SOURCE;
  }

  FILE *destination = fopen(destination_path, "wb");
  if (destination == NULL) {
    fclose(source);
    return BACKUP_MANAGER_ERR_OPEN_DESTINATION;
  }

  int status = BACKUP_MANAGER_OK;
  unsigned char buffer[BACKUP_COPY_BUFFER_SIZE];

  for (;;) {
    size_t bytes_read = fread(buffer, 1, sizeof(buffer), source);
    if (bytes_read > 0U) {
      size_t bytes_written = fwrite(buffer, 1, bytes_read, destination);
      if (bytes_written != bytes_read) {
        status = BACKUP_MANAGER_ERR_WRITE;
        break;
      }
    }

    if (bytes_read < sizeof(buffer)) {
      if (ferror(source)) {
        status = BACKUP_MANAGER_ERR_READ;
      }
      break;
    }
  }

  if (fflush(destination) != 0) {
    status = BACKUP_MANAGER_ERR_WRITE;
  }

  fclose(destination);
  fclose(source);

  if (status != BACKUP_MANAGER_OK) {
    remove(destination_path);
  }

  return status;
}

/*
 * Ensures that all directory levels in a path are created.
 */
int backup_manager_ensure_directory(const char *directory_path) {
  if ((directory_path == NULL) || (directory_path[0] == '\0')) {
    return BACKUP_MANAGER_ERR_INVALID_ARGUMENT;
  }

  char path_copy[1024];
  size_t length = strlen(directory_path);
  if (length >= sizeof(path_copy)) {
    return BACKUP_MANAGER_ERR_PATH_TOO_LONG;
  }

  memcpy(path_copy, directory_path, length + 1U);

  for (size_t i = 0; i < length; ++i) {
    if (!is_separator(path_copy[i])) {
      continue;
    }

    if (i == 0U) {
      continue;
    }

    char previous = path_copy[i];
    path_copy[i] = '\0';

    if (path_copy[0] != '\0' && !is_windows_drive_prefix(path_copy)) {
      int status = create_directory_one(path_copy);
      if (status != BACKUP_MANAGER_OK) {
        path_copy[i] = previous;
        return status;
      }
    }

    path_copy[i] = previous;
  }

  return create_directory_one(path_copy);
}

/*
 * Creates a deterministic backup copy:
 * <backup_directory>/<filename>.bak.<timestamp>.
 */
int backup_manager_backup_file(
    const char *source_path,
    const char *backup_directory,
    int64_t timestamp_unix,
    char *out_backup_path,
    size_t out_backup_path_size) {
  if ((source_path == NULL) || (backup_directory == NULL) ||
      (source_path[0] == '\0') || (backup_directory[0] == '\0')) {
    return BACKUP_MANAGER_ERR_INVALID_ARGUMENT;
  }

  char local_output_path[1024];
  char *destination_buffer = out_backup_path;
  size_t destination_size = out_backup_path_size;
  if (destination_buffer == NULL) {
    destination_buffer = local_output_path;
    destination_size = sizeof(local_output_path);
  }
  if (destination_size == 0U) {
    return BACKUP_MANAGER_ERR_INVALID_ARGUMENT;
  }

  int status = backup_manager_ensure_directory(backup_directory);
  if (status != BACKUP_MANAGER_OK) {
    return status;
  }

  const char *filename = extract_filename(source_path);
  if ((filename == NULL) || (filename[0] == '\0')) {
    return BACKUP_MANAGER_ERR_INVALID_ARGUMENT;
  }

  int64_t backup_timestamp = (timestamp_unix > 0) ? timestamp_unix : 0;
  char separator = '/';
  size_t directory_length = strlen(backup_directory);
  if ((directory_length > 0U) && is_separator(backup_directory[directory_length - 1U])) {
    separator = '\0';
  }

  int written = 0;
  if (separator == '\0') {
    written = snprintf(destination_buffer, destination_size, "%s%s.bak.%lld",
                       backup_directory, filename, (long long)backup_timestamp);
  } else {
    written = snprintf(destination_buffer, destination_size, "%s%c%s.bak.%lld",
                       backup_directory, separator, filename, (long long)backup_timestamp);
  }

  if ((written < 0) || ((size_t)written >= destination_size)) {
    return BACKUP_MANAGER_ERR_PATH_TOO_LONG;
  }

  return copy_file_contents(source_path, destination_buffer);
}

/*
 * Returns a user-facing message for backup manager status codes.
 */
const char *backup_manager_status_str(int status) {
  switch (status) {
    case BACKUP_MANAGER_OK:
      return "ok";
    case BACKUP_MANAGER_ERR_INVALID_ARGUMENT:
      return "invalid argument";
    case BACKUP_MANAGER_ERR_PATH_TOO_LONG:
      return "path too long";
    case BACKUP_MANAGER_ERR_CREATE_DIRECTORY:
      return "cannot create directory";
    case BACKUP_MANAGER_ERR_OPEN_SOURCE:
      return "cannot open source file";
    case BACKUP_MANAGER_ERR_OPEN_DESTINATION:
      return "cannot open destination file";
    case BACKUP_MANAGER_ERR_READ:
      return "read failure";
    case BACKUP_MANAGER_ERR_WRITE:
      return "write failure";
    default:
      return "unknown error";
  }
}
