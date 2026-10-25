#include "datacenter_utils.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>

#define VM_ROOT_DIR "/tmp/CloudIST"

VMType *VMType_exists(DataCenter *dc, const char* type_id){
	for(size_t i = 0; i < dc->num_vm_types; i++){
		if(strncmp(type_id, dc->vm_types[i].id, MAX_STRING_SIZE) == 0){
			return &dc->vm_types[i];
		}
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Locking helpers                                                     */
/*                                                                     */
/* GLOBAL LOCK ORDER (never violate it):                               */
/*   server locks (ascending id)  ->  dc->res_lock                     */
/* dc->types_lock is only ever taken on its own, or last.              */
/* ------------------------------------------------------------------ */

void servers_lock(DataCenter *dc, const unsigned char *mask){
	for (size_t i = 0; i < dc->num_servers; i++) {
		if (mask == NULL || mask[i])
			pthread_mutex_lock(&dc->servers[i].lock);
	}
}

void servers_unlock(DataCenter *dc, const unsigned char *mask){
	for (size_t i = dc->num_servers; i-- > 0; ) {
		if (mask == NULL || mask[i])
			pthread_mutex_unlock(&dc->servers[i].lock);
	}
}

/* ------------------------------------------------------------------ */
/* Reservations                                                        */
/* ------------------------------------------------------------------ */

/**
 * Checks if a reservation with the given id already exists.
 * Caller holds dc->res_lock.
 */
static int reservation_exists(DataCenter *dc, const char* reservation_id){
	for(size_t i = 0; i < dc->num_reservations; i++){
		if(strncmp(reservation_id, dc->reservations[i]->id, MAX_STRING_SIZE) == 0){
			return 1;
		}
	}
	return 0;
}

int reservation_resolve(DataCenter *dc, Reservation *reservation){
	int rc = 0;
	size_t vms_count = 0;

	pthread_rwlock_rdlock(&dc->types_lock);

	for (size_t i = 0; i < reservation->items_count && rc == 0; i++) {
		ReservationItem *item = &reservation->items[i];

		if (item->amount == 0) {
			fprintf(stderr, "Reservation item must request at least one VM.\n");
			rc = 1;
			break;
		}

		VMType *vm_type = VMType_exists(dc, item->vm_type_id);
		if (vm_type == NULL) {
			fprintf(stderr, "VM of type %s does not exist\n", item->vm_type_id);
			rc = 1;
			break;
		}

		if (item->server_id == 0 || item->server_id > dc->num_servers) {
			fprintf(stderr, "Invalid server in reservation.\n");
			rc = 1;
			break;
		}

		if (item->amount > MAX_RESERVATION_VMS - vms_count) {
			fprintf(stderr, "Reached max of VMS for a reservation.\n");
			rc = 1;
			break;
		}

		vms_count += item->amount;
		item->vm_type = vm_type;   // Attach vm_type to item for later use.
	}

	pthread_rwlock_unlock(&dc->types_lock);
	return rc;
}

int reservation_validate(DataCenter *dc, Reservation *reservation) {
	if (reservation_exists(dc, reservation->id)) {
		fprintf(stderr, "Reservation with id %s already exists.\n", reservation->id);
		return 1;
	}

	if (dc->num_reservations >= MAX_RESERVATIONS) {
		fprintf(stderr, "Max number of reservations reached\n");
		return 1;
	}

	/* Copy ONLY the servers used by this reservation (the others are not
	 * locked by us, so reading them would be a data race). */
	Resources available[dc->num_servers];
	size_t hosted_count[dc->num_servers];
	unsigned char seen[dc->num_servers];
	memset(available, 0, sizeof(available));
	memset(hosted_count, 0, sizeof(hosted_count));
	memset(seen, 0, sizeof(seen));

	for (size_t i = 0; i < reservation->items_count; i++) {
		ReservationItem *item = &reservation->items[i];
		size_t s = item->server_id - 1;

		if (!seen[s]) {
			available[s] = dc->servers[s].available;
			hosted_count[s] = dc->servers[s].num_hosted_vms;
			seen[s] = 1;
		}

		for (size_t j = 0; j < item->amount; j++) {
			if (hosted_count[s] >= MAX_HOSTED_VMS) {
				fprintf(stderr,
					"Server \"%zu\" reached the maximum number of hosted VMs.\n",
					item->server_id);
				return 1;
			}

			if (!resources_can_fit(available[s], item->vm_type->required)) {
				fprintf(stderr, "Server \"%zu\" does not have enough resources.\n", item->server_id);
				return 1;
			}

			resources_sub(&available[s], item->vm_type->required);
			hosted_count[s]++;
		}
	}

	return 0;
}

/**
 * Rolls back the changes made by a reservation operation.
 * Caller holds the locks of the servers involved.
 */
static void reservation_rollback(Reservation *reservation, size_t initial_vms){
	while (reservation->num_vms > initial_vms) {
		VM *vm = reservation->vms[reservation->num_vms - 1];
		Server *server = vm->server;

		resources_add(&server->available, vm->type->required);

		server->num_hosted_vms--;
		server->hosted_vms[server->num_hosted_vms] = NULL;

		reservation->num_vms--;
		reservation->vms[reservation->num_vms] = NULL;

		free(vm);
	}
}

int reservation_commit(DataCenter *dc, Reservation *reservation){
	size_t initial_vms = reservation->num_vms;

	for (size_t i = 0; i < reservation->items_count; i++) {
		ReservationItem *item = &reservation->items[i];
		Server *server = &dc->servers[item->server_id - 1];

		for (size_t j = 0; j < item->amount; j++) {
			VM *new_vm = calloc(1, sizeof(VM));
			if (new_vm == NULL) {
				fprintf(stderr, "Failed to allocate VM.\n");
				reservation_rollback(reservation, initial_vms);
				return 1;
			}

			snprintf(new_vm->id,
				MAX_VM_ID_STRING,
				"%s_%zu",
				reservation->id,
				reservation->num_vms + 1
			);

			new_vm->type = item->vm_type;
			new_vm->server = server;
			new_vm->state = VM_STATE_RESERVED;

			resources_sub(&server->available, item->vm_type->required);

			server->hosted_vms[server->num_hosted_vms++] = new_vm;
			reservation->vms[reservation->num_vms++] = new_vm;
		}
	}

	reservation->alive_vms = reservation->num_vms;
	reservation->state = RES_STATE_PENDING;

	return 0;
}

Reservation *find_pending_reservation(DataCenter *dc, const char *reservation_id) {
	Reservation *res = NULL;

	for (size_t i = 0; i < dc->num_reservations; i++) {
		if (strcmp(dc->reservations[i]->id, reservation_id) == 0) {
			res = dc->reservations[i];
			break;
		}
	}

	if (!res) {
		fprintf(stderr, "Reservation ID %s not found.\n", reservation_id);
		return NULL;
	}

	if (res->state != RES_STATE_PENDING) {
		fprintf(stderr, "Reservation ID %s is not in a pending state.\n", reservation_id);
		return NULL;
	}

	return res;
}

/* ------------------------------------------------------------------ */
/* VM destruction                                                      */
/* ------------------------------------------------------------------ */

void vm_terminate(DataCenter *dc, Reservation *res, VM *vm) {
	Server *server = vm->server;

	/* 1) Give resources back. Only this server's lock is needed, so VMs on
	 *    other servers can be reserved/destroyed in parallel. */
	pthread_mutex_lock(&server->lock);

	resources_add(&server->available, vm->type->required);

	for (size_t j = 0; j < server->num_hosted_vms; j++) {
		if (server->hosted_vms[j] == vm) {
			memmove(&server->hosted_vms[j],
			        &server->hosted_vms[j + 1],
			        (server->num_hosted_vms - j - 1) * sizeof(VM *));
			server->num_hosted_vms--;
			server->hosted_vms[server->num_hosted_vms] = NULL;
			break;
		}
	}

	vm->state = VM_STATE_TERMINATED;

	pthread_mutex_unlock(&server->lock);

	/* 2) Update the reservation; delete it when its last VM is gone. The VM
	 *    structs are only freed here (under res_lock) so that L never
	 *    dereferences a freed VM through res->vms[]. */
	pthread_mutex_lock(&dc->res_lock);

	if (--res->alive_vms == 0) {
		res->state = RES_STATE_FINISHED;

		for (size_t i = 0; i < dc->num_reservations; i++) {
			if (dc->reservations[i] == res) {
				memmove(&dc->reservations[i],
				        &dc->reservations[i + 1],
				        (dc->num_reservations - i - 1) * sizeof(Reservation *));
				dc->num_reservations--;
				dc->reservations[dc->num_reservations] = NULL;
				break;
			}
		}

		for (size_t i = 0; i < res->num_vms; i++)
			free(res->vms[i]);

		free(res);
	}

	if (--dc->running_vms == 0)
		pthread_cond_broadcast(&dc->idle_cond);

	pthread_mutex_unlock(&dc->res_lock);
}

/* ------------------------------------------------------------------ */
/* Launching VMs (Exercise 2)                                          */
/* ------------------------------------------------------------------ */

/* Used only inside the child process after fork(): print a message and bail out.
 * We use write() + _exit() on purpose (instead of fprintf/exit) because they are
 * safe to call in a freshly forked child and don't flush the parent's buffers twice. */
__attribute__((noreturn))
static void die_in_child(const char *message) {
	ssize_t written = write(STDERR_FILENO, message, strlen(message));
	(void)written;   /* nothing useful to do if even this write fails */
	_exit(1);
}

/* Runs in the child right after fork(): applies the resource limits of the VM,
 * moves into its folder and replaces itself with the program (wrapped by cpulimit). */
void spawn_vm_child(VM *vm, const char *vm_dir, const char *exec_path, const char *cpu_limit) {
	struct rlimit limit;

	/* Memory limit: the type gives it in GB, setrlimit wants bytes (<< 30 = * 2^30) */
	limit.rlim_cur = (rlim_t)vm->type->required.ram << 30;
	limit.rlim_max = limit.rlim_cur;
	if (setrlimit(RLIMIT_AS, &limit) != 0)
		die_in_child("VM: setrlimit(RLIMIT_AS) failed\n");

	/* Disk limit, also GB -> bytes. Heads up: RLIMIT_FSIZE caps the size of
	 * EACH file the VM creates, not the total space it uses. */
	limit.rlim_cur = (rlim_t)vm->type->required.disk << 30;
	limit.rlim_max = limit.rlim_cur;
	if (setrlimit(RLIMIT_FSIZE, &limit) != 0)
		die_in_child("VM: setrlimit(RLIMIT_FSIZE) failed\n");

	/* The program runs from inside the VM's own folder, where its input files were copied. */
	if (chdir(vm_dir) != 0)
		die_in_child("VM: chdir to VM directory failed\n");

	/* cpulimit throttles the program to the CPU percentage we computed.
	 * If exec works, this process becomes cpulimit and never comes back here. */
	execlp("cpulimit", "cpulimit", "-q", "-f", "-l", cpu_limit, "--", exec_path, (char *)NULL);

	/* Only reached if exec failed. */
	die_in_child("VM: exec of cpulimit failed (is it installed?)\n");
}

/**
 * Works out the percentage we hand to cpulimit, using the formula from the statement:
 *   vCPUs-of-the-VM / CPUs-per-server x number-of-servers x 100
 * NOTE: the way the statement places the parentheses is ambiguous. If the
 * teachers clarify it differently, this is the only function that needs to change.
 * (The operations are kept in the same left-to-right order as the formula, so
 * splitting it into steps below doesn't change the result.)
 */
static int cpu_limit_percent(const DataCenter *dc, const VM *vm) {
	double cpus_per_server = dc->servers[0].total.cpu;   /* all servers are identical, so any one will do */
	double vm_share_of_server = vm->type->required.cpu / cpus_per_server;
	double percent = vm_share_of_server * (double)dc->num_servers * 100.0;

	long rounded = lround(percent);
	if (rounded < 1)
		rounded = 1;   /* cpulimit doesn't accept 0, so never go below 1% */
	return (int)rounded;
}

/* Everything the monitor thread needs, bundled so we can pass it as a single pointer. */
typedef struct {
	DataCenter *dc;
	Reservation *res;
	VM *vm;
} MonitorArgs;

/** Blocks until the VM's process ends, then cleans the VM up. */
static void wait_for_vm_then_terminate(DataCenter *dc, Reservation *res, VM *vm) {
	pid_t vm_pid = vm->pid;
	int exit_status;
	pid_t reaped_pid;

	/* If a signal interrupts waitpid we just try again. */
	while ((reaped_pid = waitpid(vm_pid, &exit_status, 0)) < 0 && errno == EINTR)
		;

	/* Any other waitpid error is only reported: we still terminate the VM below,
	 * otherwise the resources it reserved would never be released. */
	if (reaped_pid < 0)
		perror("waitpid");

	vm_terminate(dc, res, vm);
}

/* Entry point of the per-VM monitor thread. */
static void *monitor_thread_main(void *raw_args) {
	/* Copy the arguments to the stack and free the heap block right away,
	 * so we can't forget to do it later. */
	MonitorArgs args = *(MonitorArgs *)raw_args;
	free(raw_args);

	wait_for_vm_then_terminate(args.dc, args.res, args.vm);
	return NULL;
}

/**
 * Starts a detached thread that waits for the VM to finish and cleans it up.
 * Returns 0 if the thread is running, 1 if it could not be created
 * (nothing is left allocated in that case).
 */
static int start_monitor_thread(DataCenter *dc, Reservation *res, VM *vm) {
	MonitorArgs *args = malloc(sizeof(MonitorArgs));
	pthread_t monitor_id;

	if (args == NULL)
		return 1;

	*args = (MonitorArgs){dc, res, vm};
	if (pthread_create(&monitor_id, NULL, monitor_thread_main, args) != 0) {
		free(args);   /* thread was not created, so nobody else will free this */
		return 1;
	}

	pthread_detach(monitor_id);
	return 0;
}

/**
 * Launches a single VM. Returns 0 if the process was forked (from that moment on
 * the monitor owns the VM and nobody else should touch it), or 1 if something
 * failed BEFORE the fork (in that case the caller has to call vm_terminate()).
 */
static int start_vm(DataCenter *dc, Reservation *res, VM *vm) {
	char vm_dir[MAX_PATH_SIZE];
	char exec_full_path[MAX_PATH_SIZE];
	char cpu_limit_str[16];

	/* Folder where this VM will live: <root>/<reservation id>/<vm id> */
	int path_len = snprintf(vm_dir, sizeof(vm_dir), "%s/%s/%s", VM_ROOT_DIR, res->id, vm->id);
	if (path_len < 0 || (size_t)path_len >= sizeof(vm_dir)) {
		fprintf(stderr, "VM directory path too long.\n");
		return 1;
	}

	/* Give the VM its own copy of the input files. */
	if (copy_dir_recursive(vm->type->input_folder, vm_dir) != 0) {
		fprintf(stderr, "Failed to copy inputs of %s to %s.\n", vm->id, vm_dir);
		return 1;
	}

	/* The child will chdir() into the VM folder, so a relative path to the
	 * executable would break. Resolve it to an absolute one first. */
	if (absolute_path(vm->type->exec_path, exec_full_path, sizeof(exec_full_path)) != 0) {
		fprintf(stderr, "Failed to resolve executable path of %s.\n", vm->id);
		return 1;
	}

	snprintf(cpu_limit_str, sizeof(cpu_limit_str), "%d", cpu_limit_percent(dc, vm));

	pid_t child_pid = fork();
	if (child_pid < 0) {
		perror("fork");
		return 1;
	}

	if (child_pid == 0) {
		/* Child: this call only returns if something went wrong, hence the _exit. */
		spawn_vm_child(vm, vm_dir, exec_full_path, cpu_limit_str);
		_exit(1);
	}

	/* Parent: save the pid and state while holding the server lock,
	 * because the L command reads these fields under the same lock. */
	pthread_mutex_lock(&vm->server->lock);
	vm->pid = child_pid;
	vm->state = VM_STATE_RUNNING;
	pthread_mutex_unlock(&vm->server->lock);

	/* One detached monitor thread per VM, so whoever called us (the thread
	 * reading the .conf file) is free to carry on immediately.
	 * If we can't get one, fall back to waiting right here in the current thread. */
	if (start_monitor_thread(dc, res, vm) != 0)
		wait_for_vm_then_terminate(dc, res, vm);

	return 0;
}

/* Starts every VM of a reservation. Returns 0 if all went well, 1 if at least one failed. */
int spawn_all_vms(DataCenter *dc, Reservation *res) {
	int result = 0;
	/* Read the count BEFORE launching anything: once the last VM finishes,
	 * the reservation itself may already have been freed. */
	size_t vm_count = res->num_vms;

	for (size_t i = 0; i < vm_count; i++) {
		VM *vm = res->vms[i];  /* safe to touch: this VM hasn't started yet, so the reservation still has living VMs */

		if (start_vm(dc, res, vm) != 0) {
			result = 1;
			vm_terminate(dc, res, vm);   /* give back the resources this VM had reserved */
		}
	}

	return result;
}

const char *vm_state_to_string(VMState state) {
	switch (state) {
		case VM_STATE_RESERVED:   return "RESERVED";
		case VM_STATE_RUNNING:    return "RUNNING";
		case VM_STATE_TERMINATED: return "TERMINATED";
		default:                  return "UNKNOWN";
	}
}

const char *res_state_to_string(ReservationState state) {
	switch (state) {
		case RES_STATE_PENDING:  return "PENDING";
		case RES_STATE_RUNNING:  return "RUNNING";
		case RES_STATE_FINISHED: return "FINISHED";
		default:                 return "UNKNOWN";
	}
}
