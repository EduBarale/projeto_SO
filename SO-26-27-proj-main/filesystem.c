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

/* Compara dois caminhos para ordenar
qsort não dá as strings mas sim ponteiros para elas por isso o uso de 
const * char * como ponteiro para char *. o strcmp devolve negativo, 0 ou
positivo que o qsort usa depois para ordenar. */
static int compare_paths(const void *left, const void *right){
  return strcmp(*(char * const *)left, *(char * const *)right);
}

/* Apaga a lista
os caminhos são criados usando strdup (malloc por string) e o array de 
ponteiros por realloc. Primeiro libertamos as string e depois o array para
evitar memory leaks. */
void free_file_list(char **files, size_t count){
  for (size_t i = 0; i < count; i++)
    free(files[i]);
  free(files);
}

/* Lista dos .conf de uma pasta por ordem alfabética */
int list_conf_files(const char *dir, char ***files, size_t *count){
  /* abre se existir, se não retorna */
  DIR *folder = opendir(dir);
  if (folder == NULL)
    return 1;

  char **list = NULL; /* array de strings vazio */
  size_t total = 0; /* nº de ficheiros .conf guardados */
  struct dirent *item; /* entrada da pasta */

  /* lê uma entrada de cada vez até terminar */
  while ((item = readdir(folder)) != NULL) {
    /* só nomes que acabam em ".conf" */
    size_t name_len = strlen(item->d_name);
    /* se tiver menos que 5 caracteres, ignora logo pois não cabe o .conf 
    vai para os últimos 5 caracteres verificar se termina em .conf*/
    if (name_len <= 5 || strcmp(item->d_name + name_len - 5, ".conf") != 0)
      continue;

    /* o readdir só dá o nome, temos de ir buscar o ficheiro ao diretorio */
    char path[MAX_PATH_SIZE];
    snprintf(path, sizeof(path), "%s/%s", dir, item->d_name);

    /* aumenta um lugar na lista */
    list = realloc(list, (total + 1) * sizeof(char *));
    list[total] = strdup(path);
    total++;
  }

  closedir(folder);

  /* ordena */
  qsort(list, total, sizeof(char *), compare_paths);

  /* devolve a lista e o numero de elementos */
  *files = list;
  *count = total;
  return 0;
}

/* Cria a pasta e as pastas de cima 
o mkdir só cria o ultimo nivel do caminho entao criamos cada parte do 
caminho por ordem, por exemplo : 
/inicio, /inicio/seguinte, /inicio/seguinte/ultima ... */
int make_dirs(const char *path){
  char copy[MAX_PATH_SIZE];
  snprintf(copy, sizeof(copy), "%s", path);

  /* começa em copy + 1 para ignorar o "/" inicial, para não fazer mkdir("") */
  for (char *letter = copy + 1; *letter != '\0'; letter++) {
    if (*letter == '/') {
      /* separa a cada "/" */
      *letter = '\0';
      if (mkdir(copy, S_IRWXU | S_IRWXG | S_IRWXO) != 0 && errno != EEXIST)
        return 1;
      *letter = '/';
    }
  }

  /* para o ultimo que não é abrangido pelo ciclo for */
  if (mkdir(copy, S_IRWXU | S_IRWXG | S_IRWXO) != 0 && errno != EEXIST)
    return 1;

  return 0;
}

/* Copia um ficheiro 
O_RDONLY : read only 
O_WRONLY : write only
O_CREAT : cria se não existir
O_TRUNC : esvazia se já existir */
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
  int status = 0; /* 0 se ok e 1 se erro */

  /* read devolve positivo (bytes lidos), 0 no fim do ficheiro e -1 se erro
  pode devolver menos do que a constante 8192 */
  while ((size = read(original, piece, sizeof(piece))) > 0) {
    /* o write pode escrever menos do que 8192, trata se com erro esse caso */
    if (write(copy, piece, (size_t)size) != size)
      status = 1;
  }
  if (size < 0) /* termina com erro de leitura e não EOF*/
    status = 1;

  close(original);
  close(copy);
  return status;
}

/* Copia uma pasta inteira */
int copy_dir_recursive(const char *from, const char *to){
  if (make_dirs(to) != 0) /* garante que o destino existe */
    return 1;

  DIR *folder = opendir(from);
  if (folder == NULL)
    return 1;

  int status = 0;
  struct dirent *item;

  /* para quando falha ou não há mais entradas */
  while (status == 0 && (item = readdir(folder)) != NULL) {
    /* "." é a própria pasta e ".." é a pasta anterior */
    if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0)
      continue;

      /* caminhos completos da origem e do destino */
    char from_path[MAX_PATH_SIZE], to_path[MAX_PATH_SIZE];
    snprintf(from_path, sizeof(from_path), "%s/%s", from, item->d_name);
    snprintf(to_path, sizeof(to_path), "%s/%s", to, item->d_name);

    /* stuct stat é onde sao escritos os metadados (tipo, permissoes..)*/
    struct stat info;
    if (lstat(from_path, &info) != 0) {
      status = 1;
      break; /* para chegar ao closedir */
    }

    if (S_ISDIR(info.st_mode))
      status = copy_dir_recursive(from_path, to_path);
    else if (S_ISREG(info.st_mode))
      status = copy_file(from_path, to_path, info.st_mode);
  }

  closedir(folder);
  return status;
}
