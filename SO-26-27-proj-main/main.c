#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

#include "parser.h"
#include "datacenter.h"
#include "constants.h"
#include "filesystem.h"

/**
 * Executes every command of one .conf file (already opened as `fd`).
 * This is the old command loop of main(), now reading from `fd` instead of
 * STDIN_FILENO, so it can run on several threads (one file per thread).
 */
static void process_conf(DataCenter *dc, int fd){
	while(1){
		switch (get_next_command(fd)){
			case CMD_DEFINE: {
				VMType vmtype;

				if (parse_define(fd, &vmtype) != 0) {
					fprintf(stderr, "Invalid define command. See H (help) for usage.\n");
					continue;
				}

				if(datacenter_define_VM(dc, &vmtype) != 0){
					fprintf(stderr, "Failed to define VM.\n");
					continue;
				}

				printf("VM successfully defined!\n");
				break;
			}

			case CMD_RESERVE: {
				Reservation reservation = {0}; 

				size_t num_items = parse_reserve(fd, &reservation, MAX_RESERVATIONS_ITEMS);

				if (num_items == 0) {
					fprintf(stderr, "Invalid reserve command. See H (help) for usage.\n");
					continue;
				}

				if (datacenter_reserve(dc, &reservation) != 0) {
					fprintf(stderr, "Failed to reserve VMs.\n");
					continue;
				}

				printf("Reservation made successfully!\n");
				break;
			}

			case CMD_EXECUTE: {
				char id[MAX_STRING_SIZE];

				if(parse_execute(fd, id) != 0){
					fprintf(stderr, "Invalid execute command. See H (help) for usage.\n");
					continue;
				}

				if (datacenter_execute(dc, id) != 0) {
					fprintf(stderr, "Failed to execute reservation.\n");
					continue;
				}

				// The VMs are now running in their own processes (not finished yet).
				printf("Reservation %s started!\n", id);
				break;
			}

			case CMD_LIST:
				if (datacenter_list(dc) != 0) {
					fprintf(stderr, "Failed to list VMs.\n");
					continue;
				}
				break;

			case CMD_WAIT: {
				unsigned int delay;

				if(parse_wait(fd, &delay) != 0){
					fprintf(stderr, "Invalid wait command. See H (help) for usage.\n");
					continue;
				}

				datacenter_wait(delay);
				break;
			}

			case CMD_INVALID:
				fprintf(stderr, "Invalid Command. See H (help) for usage.\n");
				break;

			case CMD_HELP:
				printf(
					"Spaces between arguments are allowed, but not after command end.\n"
					"Available commands:\n"
					" D <VM_TYPE_ID> <INPUT_FOLDER> <EXECUTABLE_PATH> <RAM_NEEDED> <DISK_NEEDED> <VCPU_NEEDED_COUNT>\n"
					" R <RESERVATION_ID> [<VM_TYPE_ID> <COUNT> <SERVER_ID>]+\n"
					" A <RESERVATION_ID>\n"
					" L\n"
					" E <DELAY_MS>\n"
					" H\n"
				);
				break;

			case CMD_EMPTY:
				break;

			case EOC:
				return;
		}
	}
}

/* ---------------- Exercício 3: pool de threads ---------------- */

typedef struct {
	DataCenter *dc;
	char **paths;           // caminhos dos ficheiros .conf
	size_t total;           // quantos ficheiros há
	size_t next_file;       // próximo ficheiro que ninguém pegou
	pthread_mutex_t lock;   // protege next_file
} Queue;

static void *worker(void *arg){
	Queue *queue = arg;

	while (1) {
		/* Pegar num ficheiro: ler e avançar o contador tem de ser atómico */
		pthread_mutex_lock(&queue->lock);
		size_t file = queue->next_file++;
		pthread_mutex_unlock(&queue->lock);

		if (file >= queue->total)
			break;   // já não há ficheiros

		/* O_CLOEXEC: as VMs (processos filhos) não herdam este fd */
		int fd = open(queue->paths[file], O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			perror(queue->paths[file]);
			continue;
		}

		process_conf(queue->dc, fd);
		close(fd);
	}

	return NULL;
}

int main(int argc, char **argv){
	DataCenter dc;
	datacenter_init(&dc);

	if (argc != 7) {
		fprintf(stderr, "Usage: %s <servers> <ram> <disk> <cpus> <input_dir> <max_threads>\n", argv[0]);
		return 1;
	}

	size_t servers;
	size_t ram;
	size_t disk;
	double cpu;
	size_t max_threads;

	if (parse_size_t_arg(argv[1], &servers) != 0 ||
			parse_size_t_arg(argv[2], &ram) != 0 ||
			parse_size_t_arg(argv[3], &disk) != 0 ||
			parse_double_arg(argv[4], &cpu) != 0 ||
			parse_size_t_arg(argv[6], &max_threads) != 0 ||
			max_threads == 0) {
		fprintf(stderr, "Invalid command line arguments.\n");
		return 1;
	}

	const char *input_dir = argv[5];

	if (!path_exists(input_dir)) {
		fprintf(stderr, "Invalid input directory.\n");
		return 1;
	}

	Resources resources = {
    .ram = ram,
    .disk = disk,
    .cpu = cpu
	};

	if(datacenter_configure(&dc, servers, &resources) != 0){
		fprintf(stderr, "Failed to configure Data Center.\n");
		return 1;
	}


	Queue queue = { .dc = &dc };
	pthread_mutex_init(&queue.lock, NULL);

	if (list_conf_files(input_dir, &queue.paths, &queue.total) != 0) {
		fprintf(stderr, "Failed to list .conf files in %s.\n", input_dir);
		datacenter_destroy(&dc);
		return 1;
	}

	/* Tem que ter threads do que ficheiros */
	size_t n_threads = max_threads < queue.total ? max_threads : queue.total;

	/* "+ 1" para o malloc nunca receber 0 */
	pthread_t *threads = malloc((n_threads + 1) * sizeof(pthread_t));
	if (threads == NULL) {
		fprintf(stderr, "Out of memory.\n");
		free_file_list(queue.paths, queue.total);
		datacenter_destroy(&dc);
		return 1;
	}

	/* Contar as que arrancaram, porque a criação pode falhar a meio */
	size_t started = 0;
	for (size_t i = 0; i < n_threads; i++) {
		if (pthread_create(&threads[started], NULL, worker, &queue) != 0) {
			fprintf(stderr, "Failed to create worker thread.\n");
			break;
		}
		started++;
	}

	/* Se nenhuma arrancou, o main faz o trabalho sozinho */
	if (started == 0)
		worker(&queue);

	for (size_t i = 0; i < started; i++)
		pthread_join(threads[i], NULL);

	/* Eesperar pelas VMs antes de destruir */
	datacenter_wait_all(&dc);

	free(threads);
	free_file_list(queue.paths, queue.total);
	pthread_mutex_destroy(&queue.lock);
	datacenter_destroy(&dc);

	return 0;
}
