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
 * Lists files ending in ".conf" inside a directory, sorted alphabetically
 * using strcmp to sort.
 * 
 * @param dir Directory to search.
 * @param files [out] Array of strings (names of files).
 * @param count [out] Number of files.
 * 
 * @return 0 if sucess, 1 if there was an error (cannot open directory, no memory).
 */
int list_conf_files(const char *dir, char ***files, size_t *count);

/**
 * Frees an array created by list_conf_files().
 * 
 * @param files Array from list_conf_files().
 * @param count Number of entries in the array.
 */
void free_file_list(char **files, size_t count);

/**
 * Creats a directory and can use the ones that already exist.
 * 
 * @param path Directory to be created.
 * 
 * @return 0 if sucess, 1 if error.
 */
int make_dirs(const char *path);

/**
 * Copies recursively the content from the starting directory (src) to the 
 * final one (dst).
 * @param src Inicial directory.
 * @param dst Destination directory.
 * 
 * @return 0 if sucess, 1 if error.
 */
int copy_dir_recursive(const char *src, const char *dst);

#endif // FILESYSTEM__H
