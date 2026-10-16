#define _XOPEN_SOURCE 700

#include "filesystem.h"
#include "constants.h"

#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

int path_exists(const char *path){
  struct stat st;

  if (stat(path, &st) != 0)
    return 0;

  return S_ISDIR(st.st_mode);
}

int file_exists(const char *path){
  struct stat st;

  if (stat(path, &st) != 0)
    return 0;

  return S_ISREG(st.st_mode);
}

int absolute_path(const char *path, char *buffer, size_t size){
  char *resolved = realpath(path, NULL);

  if (resolved == NULL)
    return 1;

  if (strlen(resolved) >= size) {
    free(resolved);
    return 1;
  }

  strcpy(buffer, resolved);

  free(resolved);
  return 0;
}


/* ------------------------------------------------------------------ */
/* Exercise 1: listing .conf files and copying directories             */
/* ------------------------------------------------------------------ */

static int cmp_paths(const void *a, const void *b){
  return strcmp(*(char * const *)a, *(char * const *)b);
}

void free_file_list(char **files, size_t count){
  for (size_t i = 0; i < count; i++)
    free(files[i]);
  free(files);
}

int list_conf_files(const char *dir, char ***files, size_t *count){
  DIR *d = opendir(dir);
  if (d == NULL)
    return 1;

  size_t cap = 8, n = 0;
  char **arr = malloc(cap * sizeof(char *));
  if (arr == NULL) {
    closedir(d);
    return 1;
  }

  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    size_t len = strlen(e->d_name);
    if (len <= 5 || strcmp(e->d_name + len - 5, ".conf") != 0)
      continue;

    char path[MAX_PATH_SIZE];
    int w = snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
    if (w < 0 || (size_t)w >= sizeof(path))
      continue;

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
      continue;

    if (n == cap) {
      char **bigger = realloc(arr, cap * 2 * sizeof(char *));
      if (bigger == NULL) {
        free_file_list(arr, n);
        closedir(d);
        return 1;
      }
      arr = bigger;
      cap *= 2;
    }

    arr[n] = strdup(path);
    if (arr[n] == NULL) {
      free_file_list(arr, n);
      closedir(d);
      return 1;
    }
    n++;
  }

  closedir(d);

  // Same directory prefix everywhere, so sorting full paths == sorting names.
  qsort(arr, n, sizeof(char *), cmp_paths);

  *files = arr;
  *count = n;
  return 0;
}

int make_dirs(const char *path){
  char tmp[MAX_PATH_SIZE];
  size_t len = strlen(path);

  if (len == 0 || len >= sizeof(tmp))
    return 1;

  memcpy(tmp, path, len + 1);

  for (char *p = tmp + 1; *p != '\0'; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return 1;
      *p = '/';
    }
  }

  if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
    return 1;

  return 0;
}

static int copy_file(const char *src, const char *dst, mode_t mode){
  int in = open(src, O_RDONLY);
  if (in < 0)
    return 1;

  int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (out < 0) {
    close(in);
    return 1;
  }

  char buf[8192];
  int rc = 0;

  while (rc == 0) {
    ssize_t r = read(in, buf, sizeof(buf));

    if (r == 0)
      break;                     // EOF
    if (r < 0) {
      if (errno == EINTR)
        continue;
      rc = 1;
      break;
    }

    size_t off = 0;
    while (off < (size_t)r) {    // handle partial writes
      ssize_t w = write(out, buf + off, (size_t)r - off);
      if (w < 0) {
        if (errno == EINTR)
          continue;
        rc = 1;
        break;
      }
      off += (size_t)w;
    }
  }

  close(in);
  if (close(out) != 0)
    rc = 1;

  return rc;
}

int copy_dir_recursive(const char *src, const char *dst){
  if (make_dirs(dst) != 0)
    return 1;

  DIR *d = opendir(src);
  if (d == NULL)
    return 1;

  int rc = 0;
  struct dirent *e;

  while (rc == 0 && (e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
      continue;

    char s[MAX_PATH_SIZE], t[MAX_PATH_SIZE];
    int w1 = snprintf(s, sizeof(s), "%s/%s", src, e->d_name);
    int w2 = snprintf(t, sizeof(t), "%s/%s", dst, e->d_name);
    if (w1 < 0 || w2 < 0 || (size_t)w1 >= sizeof(s) || (size_t)w2 >= sizeof(t)) {
      rc = 1;
      break;
    }

    struct stat st;
    if (lstat(s, &st) != 0) {
      rc = 1;
      break;
    }

    if (S_ISDIR(st.st_mode))
      rc = copy_dir_recursive(s, t);
    else if (S_ISREG(st.st_mode))
      rc = copy_file(s, t, st.st_mode & 0777);
    // other types (symlinks, sockets, ...) are skipped
  }

  closedir(d);
  return rc;
}
