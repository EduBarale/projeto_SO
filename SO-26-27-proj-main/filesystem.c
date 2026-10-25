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


/* Exercício 1: listar ficheiros .conf e copiar pastas */

/* Compara dois caminhos (para ordenar) */
static int compare_paths(const void *left, const void *right){
  return strcmp(*(char * const *)left, *(char * const *)right);
}

/* Apaga a lista */
void free_file_list(char **files, size_t count){
  for (size_t i = 0; i < count; i++)
    free(files[i]);
  free(files);
}

/* Lista os .conf de uma pasta (por ordem alfabética) */
int list_conf_files(const char *dir, char ***files, size_t *count){
  DIR *folder = opendir(dir);
  if (folder == NULL)
    return 1;

  char **list = NULL;
  size_t total = 0;
  struct dirent *item;

  while ((item = readdir(folder)) != NULL) {
    /* só nomes que acabam em ".conf" */
    size_t name_len = strlen(item->d_name);
    if (name_len <= 5 || strcmp(item->d_name + name_len - 5, ".conf") != 0)
      continue;

    char path[MAX_PATH_SIZE];
    snprintf(path, sizeof(path), "%s/%s", dir, item->d_name);

    /* mais um lugar na lista */
    list = realloc(list, (total + 1) * sizeof(char *));
    list[total] = strdup(path);
    total++;
  }

  closedir(folder);

  qsort(list, total, sizeof(char *), compare_paths);

  *files = list;
  *count = total;
  return 0;
}

/* Cria a pasta e as pastas de cima */
int make_dirs(const char *path){
  char copy[MAX_PATH_SIZE];
  snprintf(copy, sizeof(copy), "%s", path);

  for (char *letter = copy + 1; *letter != '\0'; letter++) {
    if (*letter == '/') {
      *letter = '\0';
      if (mkdir(copy, S_IRWXU | S_IRWXG | S_IRWXO) != 0 && errno != EEXIST)
        return 1;
      *letter = '/';
    }
  }

  if (mkdir(copy, S_IRWXU | S_IRWXG | S_IRWXO) != 0 && errno != EEXIST)
    return 1;

  return 0;
}

/* Copia um ficheiro */
static int copy_file(const char *from, const char *to, mode_t mode){
  int original = open(from, O_RDONLY);
  if (original < 0)
    return 1;

  int copy = open(to, O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (copy < 0) {
    close(original);
    return 1;
  }

  char piece[8192];
  ssize_t size;
  int status = 0;

  while ((size = read(original, piece, sizeof(piece))) > 0) {
    if (write(copy, piece, size) != size)
      status = 1;
  }
  if (size < 0)
    status = 1;

  close(original);
  close(copy);
  return status;
}

/* Copia uma pasta inteira */
int copy_dir_recursive(const char *from, const char *to){
  if (make_dirs(to) != 0)
    return 1;

  DIR *folder = opendir(from);
  if (folder == NULL)
    return 1;

  int status = 0;
  struct dirent *item;

  while (status == 0 && (item = readdir(folder)) != NULL) {
    if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0)
      continue;

    char from_path[MAX_PATH_SIZE], to_path[MAX_PATH_SIZE];
    snprintf(from_path, sizeof(from_path), "%s/%s", from, item->d_name);
    snprintf(to_path, sizeof(to_path), "%s/%s", to, item->d_name);

    struct stat info;
    if (lstat(from_path, &info) != 0) {
      status = 1;
      break;
    }

    if (S_ISDIR(info.st_mode))
      status = copy_dir_recursive(from_path, to_path);
    else if (S_ISREG(info.st_mode))
      status = copy_file(from_path, to_path, info.st_mode);
  }

  closedir(folder);
  return status;
}