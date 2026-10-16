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

__attribute__((noreturn))
static void child_fail(const char *msg) {
	ssize_t r = write(STDERR_FILENO, msg, strlen(msg));
	(void)r;
	_exit(1);
}

void spawn_vm_child(VM *vm, const char *vm_dir, const char *exec_path, const char *cpu_pct) {
	struct rlimit lim;

	/* RAM (GB -> bytes) */
	lim.rlim_cur = (rlim_t)vm->type->required.ram << 30;
	lim.rlim_max = lim.rlim_cur;
	if (setrlimit(RLIMIT_AS, &lim) != 0)
		child_fail("VM: setrlimit(RLIMIT_AS) failed\n");

	/* Disk (GB -> bytes), maximum size of each file the VM can create */
	lim.rlim_cur = (rlim_t)vm->type->required.disk << 30;
	lim.rlim_max = lim.rlim_cur;
	if (setrlimit(RLIMIT_FSIZE, &lim) != 0)
		child_fail("VM: setrlimit(RLIMIT_FSIZE) failed\n");

	/* The application runs inside the VM directory (its copied inputs). */
	if (chdir(vm_dir) != 0)
		child_fail("VM: chdir to VM directory failed\n");

	execlp("cpulimit", "cpulimit", "-q", "-f", "-l", cpu_pct, "--", exec_path, (char *)NULL);

	child_fail("VM: exec of cpulimit failed (is it installed?)\n");
}

/**
 * Percentage to give to cpulimit, following the formula in the statement:
 *   vCPU-of-VM / CPUs-per-server x number-of-servers x 100
 * NOTE: the statement's parenthesization is ambiguous. If the teachers
 * clarify it differently, only this function has to change.
 */
static int cpu_percent(const DataCenter *dc, const VM *vm) {
	double cpus_per_server = dc->servers[0].total.cpu;   // all servers are equal
	double pct = vm->type->required.cpu / cpus_per_server
	             * (double)dc->num_servers * 100.0;

	long p = lround(pct);
	if (p < 1)
		p = 1;
	return (int)p;
}

typedef struct {
	DataCenter *dc;
	Reservation *res;
	VM *vm;
} MonitorArg;

/** Waits for the VM process and destroys the VM. */
static void wait_and_terminate(DataCenter *dc, Reservation *res, VM *vm) {
	pid_t pid = vm->pid;
	int status;

	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;

	vm_terminate(dc, res, vm);
}

static void *vm_monitor(void *arg) {
	MonitorArg m = *(MonitorArg *)arg;
	free(arg);

	wait_and_terminate(m.dc, m.res, m.vm);
	return NULL;
}

/**
 * Launches one VM. Returns 0 if the process was forked (from then on the
 * VM is owned by its monitor and must not be touched), 1 if it failed BEFORE
 * forking (the caller must then call vm_terminate()).
 */
static int launch_vm(DataCenter *dc, Reservation *res, VM *vm) {
	char vm_dir[MAX_PATH_SIZE];
	char exec_abs[MAX_PATH_SIZE];
	char cpu_pct[16];

	int n = snprintf(vm_dir, sizeof(vm_dir), "%s/%s/%s", VM_ROOT_DIR, res->id, vm->id);
	if (n < 0 || (size_t)n >= sizeof(vm_dir)) {
		fprintf(stderr, "VM directory path too long.\n");
		return 1;
	}

	if (copy_dir_recursive(vm->type->input_folder, vm_dir) != 0) {
		fprintf(stderr, "Failed to copy inputs of %s to %s.\n", vm->id, vm_dir);
		return 1;
	}

	/* The child does chdir(), so give it an absolute path to the executable. */
	if (absolute_path(vm->type->exec_path, exec_abs, sizeof(exec_abs)) != 0) {
		fprintf(stderr, "Failed to resolve executable path of %s.\n", vm->id);
		return 1;
	}

	snprintf(cpu_pct, sizeof(cpu_pct), "%d", cpu_percent(dc, vm));

	pid_t pid = fork();
	if (pid < 0) {
		perror("fork");
		return 1;
	}

	if (pid == 0) {
		spawn_vm_child(vm, vm_dir, exec_abs, cpu_pct);
		_exit(1);
	}

	/* Parent: record pid/state under the server lock (L reads them there). */
	pthread_mutex_lock(&vm->server->lock);
	vm->pid = pid;
	vm->state = VM_STATE_RUNNING;
	pthread_mutex_unlock(&vm->server->lock);

	/* One detached monitor thread per VM, so the caller (the thread that
	 * is reading the .conf file) is released immediately. */
	MonitorArg *m = malloc(sizeof(MonitorArg));
	pthread_t tid;

	if (m != NULL) {
		*m = (MonitorArg){dc, res, vm};
		if (pthread_create(&tid, NULL, vm_monitor, m) == 0) {
			pthread_detach(tid);
			return 0;
		}
		free(m);
	}

	/* Could not create the monitor: degrade to waiting in this thread. */
	wait_and_terminate(dc, res, vm);
	return 0;
}

int spawn_all_vms(DataCenter *dc, Reservation *res) {
	int rc = 0;
	size_t n = res->num_vms;   // read BEFORE launching: res may be freed after the last VM ends

	for (size_t i = 0; i < n; i++) {
		VM *vm = res->vms[i];  // still valid: this VM has not been launched, so alive_vms > 0

		if (launch_vm(dc, res, vm) != 0) {
			rc = 1;
			vm_terminate(dc, res, vm);   // release what it reserved
		}
	}

	return rc;
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
