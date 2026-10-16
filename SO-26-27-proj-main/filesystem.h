#ifndef FILESYSTEM__H
#define FILESYSTEM__H

#include <stddef.h>

/**
 * Checks whether a path exists and is a directory.
 *
 * @param path Directory path.
 *
 * @return 1 if it exists and is a directory, 0 otherwise.
 */
int path_exists(const char *path);

/**
 * Checks whether a path exists and is a regular file.
 *
 * @param path File path.
 *
 * @return 1 if it exists and is a regular file, 0 otherwise.
 */
int file_exists(const char *path);

/**
 * Converts the given path into an absolute path, resolving symbolic links,
 * relative components ('.' and '..'), and redundant separators. The resolved
 * path is copied into the provided buffer.
 *
 * @param path Path to resolve.
 * @param buffer Destination buffer where the absolute path will be stored.
 * @param size Size of the destination buffer, in bytes.
 *
 * @return 0 if the path was successfully resolved and copied to the buffer
 * @return 1 if the path could not be resolved or the buffer is too small.
 */
int absolute_path(const char *path, char *buffer, size_t size);

/**
 * Lists the regular files with extension ".conf" inside a directory, sorted
 * alphabetically. Every entry is the full path ("<dir>/<name>").
 *
 * @param dir Directory to scan.
 * @param files Output: malloc'd array of malloc'd strings.
 * @param count Output: number of entries in the array.
 *
 * @return 0 on success (free with free_file_list()), 1 on error.
 */
int list_conf_files(const char *dir, char ***files, size_t *count);

/**
 * Frees an array created by list_conf_files().
 */
void free_file_list(char **files, size_t count);

/**
 * Creates a directory and all missing parents (like "mkdir -p").
 *
 * @return 0 on success (also if it already exists), 1 on error.
 */
int make_dirs(const char *path);

/**
 * Recursively copies the content of directory src into directory dst
 * (created if needed). Subdirectories and regular files are preserved.
 * Only POSIX file descriptors are used (open/read/write), never stdio.
 *
 * @return 0 on success, 1 on error.
 */
int copy_dir_recursive(const char *src, const char *dst);

#endif // FILESYSTEM__H