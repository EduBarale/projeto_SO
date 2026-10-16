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

/* ---------------- Exercise 3: pool of worker threads ---------------- */

typedef struct {
	DataCenter *dc;
	char **files;            // .conf paths, already sorted alphabetically
	size_t count;
	size_t next;             // index of the next file to process
	pthread_mutex_t lock;    // protects `next`
} WorkQueue;

static void *worker_main(void *arg){
	WorkQueue *q = arg;

	while (1) {
		pthread_mutex_lock(&q->lock);
		size_t idx = q->next < q->count ? q->next++ : q->count;
		pthread_mutex_unlock(&q->lock);

		if (idx >= q->count)
			break;

		// O_CLOEXEC: VM children must not inherit the .conf descriptor.
		int fd = open(q->files[idx], O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			perror(q->files[idx]);
			continue;
		}

		process_conf(q->dc, fd);
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

	WorkQueue queue = { .dc = &dc, .files = NULL, .count = 0, .next = 0 };
	pthread_mutex_init(&queue.lock, NULL);

	if (list_conf_files(input_dir, &queue.files, &queue.count) != 0) {
		fprintf(stderr, "Failed to list .conf files in %s.\n", input_dir);
		datacenter_destroy(&dc);
		return 1;
	}

	// Never more threads than files.
	size_t nthreads = max_threads < queue.count ? max_threads : queue.count;
	pthread_t *threads = malloc((nthreads ? nthreads : 1) * sizeof(pthread_t));
	size_t created = 0;

	if (threads == NULL) {
		fprintf(stderr, "Out of memory.\n");
		free_file_list(queue.files, queue.count);
		datacenter_destroy(&dc);
		return 1;
	}

	for (size_t i = 0; i < nthreads; i++) {
		if (pthread_create(&threads[created], NULL, worker_main, &queue) != 0) {
			fprintf(stderr, "Failed to create worker thread.\n");
			break;   // the workers already created will consume the whole queue
		}
		created++;
	}

	if (created == 0 && queue.count > 0)
		worker_main(&queue);   // no thread could be created: run sequentially

	for (size_t i = 0; i < created; i++)
		pthread_join(threads[i], NULL);

	// All files processed. VMs may still be running in their own processes:
	// wait for them before tearing the data center down.
	datacenter_wait_all(&dc);

	free(threads);
	free_file_list(queue.files, queue.count);
	pthread_mutex_destroy(&queue.lock);
	datacenter_destroy(&dc);

	return 0;
}
