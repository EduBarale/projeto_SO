#ifndef DATACENTER_UTILS__H
#define DATACENTER_UTILS__H

#include "datacenter.h"

/**
 * Checks if a VMType with the given id exists.
 *
 * The caller must hold dc->types_lock (read or write).
 *
 * @param dc Pointer to Data Center.
 * @param type_id Id of VM type being checked.
 *
 * @return Pointer to VMType if it exists.
 * @return NULL if VMType does not exist.
 */
VMType *VMType_exists(DataCenter *dc, const char* type_id);

/**
 * Locks servers in ascending id order (always this order, to avoid deadlocks).
 *
 * @param dc Pointer to Data Center.
 * @param mask Array with dc->num_servers entries; server i is locked if
 *             mask[i] != 0. NULL means "lock every server".
 */
void servers_lock(DataCenter *dc, const unsigned char *mask);

/**
 * Unlocks the servers locked by servers_lock() with the same mask.
 */
void servers_unlock(DataCenter *dc, const unsigned char *mask);

/**
 * Static checks that do not touch server state: item amounts, VM types exist,
 * server ids are valid, max VMs per reservation. Attaches the VMType to every
 * item. Takes dc->types_lock (read) internally.
 *
 * @return 0 if valid, 1 otherwise.
 */
int reservation_resolve(DataCenter *dc, Reservation *reservation);

/**
 * Simulates the placement of every VM requested in the reservation using a
 * temporary copy of the available resources of the servers involved, and
 * checks that the id is not in use and that the table is not full.
 * The Data Center state is left unchanged regardless of the outcome.
 *
 * PRECONDITIONS: reservation_resolve() succeeded, the caller holds the locks
 * of every server used by the reservation AND dc->res_lock.
 *
 * @return 0 if the reservation can be fully allocated.
 * @return 1 otherwise.
 */
int reservation_validate(DataCenter *dc, Reservation *reservation);

/**
 * Creates all VMs requested by the reservation, assigns them to their
 * designated servers, updates the server resource availability, and stores
 * references to the created VMs in both the reservation and the corresponding
 * server hosted VM list. Rolls back on error.
 *
 * PRECONDITIONS: same locks as reservation_validate(), which must have
 * succeeded in the same critical section.
 *
 * @return 0 on success, 1 on error (changes rolled back).
 */
int reservation_commit(DataCenter *dc, Reservation *reservation);

/**
 * Looks up a reservation by id and checks that it is pending.
 * The caller must hold dc->res_lock.
 *
 * @return Pointer to the Reservation if found and RES_STATE_PENDING.
 * @return NULL if not found or not pending (error is printed).
 */
Reservation *find_pending_reservation(DataCenter *dc, const char *reservation_id);

/**
 * Runs in the child process after fork(). Only async-signal-safe calls are
 * used: everything that needs malloc/stdio is prepared by the parent.
 * Sets RLIMIT_AS (RAM), RLIMIT_FSIZE (disk), chdir()s to the VM directory and
 * execs "cpulimit -q -f -l <cpu_pct> -- <exec_path>". Never returns.
 */
void spawn_vm_child(VM *vm, const char *vm_dir, const char *exec_path, const char *cpu_pct);

/**
 * For every VM of the reservation: copies the input folder to
 * /tmp/CloudIST/<reservation>/<vm>, forks, records pid/state and starts a
 * monitor thread that waits for the process. Does NOT wait for the VMs.
 *
 * WARNING: once the last VM terminates the reservation is freed, so the
 * caller must not use `res` after this function returns.
 *
 * @return 0 on success, 1 if any VM could not be launched.
 */
int spawn_all_vms(DataCenter *dc, Reservation *res);

/**
 * Destroys one VM that has finished (or failed to launch): returns its
 * resources to the server, removes it from the server's hosted list and,
 * if it was the last VM alive of its reservation, deletes the reservation.
 * Wakes datacenter_wait_all() when no VM is left running.
 */
void vm_terminate(DataCenter *dc, Reservation *res, VM *vm);

/**
 * Converts a VM state to a human-readable string. Used by datacenter_list().
 */
const char *vm_state_to_string(VMState state);

/**
 * Converts a reservation state to a human-readable string. Used by
 * datacenter_list().
 */
const char *res_state_to_string(ReservationState state);

#endif // DATACENTER_UTILS__H
