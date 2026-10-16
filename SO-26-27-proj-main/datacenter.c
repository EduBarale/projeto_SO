#include "datacenter.h"
#include "datacenter_utils.h"

#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

void datacenter_init(DataCenter *dc) {
  dc->servers = NULL;
  dc->num_servers = 0;

  dc->num_vm_types = 0;

  dc->num_reservations = 0;
  dc->running_vms = 0;

  pthread_rwlock_init(&dc->types_lock, NULL);
  pthread_mutex_init(&dc->res_lock, NULL);
  pthread_cond_init(&dc->idle_cond, NULL);

  dc->configured = 0;
}

void datacenter_destroy(DataCenter *dc) {
  // Reservations still in the table (never started with 'A') own their VMs.
  for (size_t i = 0; i < dc->num_reservations; i++) {
    for (size_t j = 0; j < dc->reservations[i]->num_vms; j++)
      free(dc->reservations[i]->vms[j]);
    free(dc->reservations[i]);
  }
  dc->num_reservations = 0;

  for (size_t i = 0; i < dc->num_servers; i++)
    pthread_mutex_destroy(&dc->servers[i].lock);

  free(dc->servers);
  dc->servers = NULL;

  pthread_rwlock_destroy(&dc->types_lock);
  pthread_mutex_destroy(&dc->res_lock);
  pthread_cond_destroy(&dc->idle_cond);
}

int datacenter_configure(DataCenter *dc, size_t num_servers, Resources *resources){
  if(dc->configured){
    fprintf(stderr, "Data Center is already configured!\n");
    return 1;
  }

  if (num_servers == 0 || resources->ram == 0 || resources->disk == 0 || resources->cpu <= 0.0) {
    fprintf(stderr, "Invalid configuration values.\n");
    return 1;
  }

  dc->servers = (Server*)malloc(num_servers*sizeof(Server));

  if(dc->servers == NULL){
    fprintf(stderr, "Failed to allocate memory for servers.\n");
    return 1;
  }

  for (size_t i = 0; i < num_servers; i++) {
    dc->servers[i].id = i+1;
    dc->servers[i].total = *resources;
    dc->servers[i].available = *resources;
    dc->servers[i].num_hosted_vms = 0;
    pthread_mutex_init(&dc->servers[i].lock, NULL);   // one lock PER server
  }

  dc->num_servers = num_servers;
  dc->configured = 1;

  return 0;
}

int datacenter_define_VM(DataCenter *dc, VMType *type){
  if (!dc->configured) {
    fprintf(stderr, "Data Center needs to be configured!\n");
    return 1;
  }

  // Filesystem checks need no lock.
  if (!file_exists(type->exec_path)) {
    fprintf(stderr, "Invalid executable path.\n");
    return 1;
  }

  if (!path_exists(type->input_folder)) {
    fprintf(stderr, "Invalid input folder.\n");
    return 1;
  }

  int rc = 0;

  pthread_rwlock_wrlock(&dc->types_lock);

  VMType *existing = VMType_exists(dc, type->id);

  if (existing != NULL) {
    // Several .conf files running in parallel may define the same type with
    // the same values: accept that silently. Different values are an error.
    if (strcmp(existing->input_folder, type->input_folder) != 0 ||
        strcmp(existing->exec_path, type->exec_path) != 0 ||
        !resources_equal(existing->required, type->required)) {
      fprintf(stderr, "VM type with id %s already exists.\n", type->id);
      rc = 1;
    }
  } else if (dc->num_vm_types >= MAX_VM_TYPES) {
    fprintf(stderr, "Maximum of VM types reached.\n");
    rc = 1;
  } else {
    dc->vm_types[dc->num_vm_types++] = *type;
  }

  pthread_rwlock_unlock(&dc->types_lock);

  return rc;
}

int datacenter_reserve(DataCenter *dc, Reservation *reservation) {
  if (!dc->configured) {
    fprintf(stderr, "Data Center needs to be configured!\n");
    return 1;
  }

  // 1) Checks that do not depend on server state (no server lock needed).
  if (reservation_resolve(dc, reservation) != 0)
    return 1;

  Reservation *res = malloc(sizeof(Reservation));
  if (res == NULL) {
    fprintf(stderr, "Failed to allocate reservation.\n");
    return 1;
  }
  *res = *reservation;

  // 2) Lock ONLY the servers this reservation touches (ascending order).
  //    Reservations on disjoint servers proceed in parallel.
  unsigned char mask[dc->num_servers];
  memset(mask, 0, sizeof(mask));
  for (size_t i = 0; i < res->items_count; i++)
    mask[res->items[i].server_id - 1] = 1;

  servers_lock(dc, mask);
  pthread_mutex_lock(&dc->res_lock);

  // 3) Validate + commit in the SAME critical section (otherwise two threads
  //    could validate the same free space and over-book the server).
  int rc = reservation_validate(dc, res);

  if (rc == 0)
    rc = reservation_commit(dc, res);

  if (rc == 0)
    dc->reservations[dc->num_reservations++] = res;

  pthread_mutex_unlock(&dc->res_lock);
  servers_unlock(dc, mask);

  if (rc != 0)
    free(res);

  return rc;
}

int datacenter_execute(DataCenter *dc, const char *reservation_id) {
  if(!dc->configured){
    fprintf(stderr, "Data Center needs to be configured!\n");
    return 1;
  }

  pthread_mutex_lock(&dc->res_lock);

  Reservation *res = find_pending_reservation(dc, reservation_id);
  if (!res) {
    pthread_mutex_unlock(&dc->res_lock);
    return 1;
  }

  // Marking it RUNNING under the lock prevents a second 'A' on the same
  // reservation from another thread.
  res->state = RES_STATE_RUNNING;
  dc->running_vms += res->num_vms;

  pthread_mutex_unlock(&dc->res_lock);

  // Forks the VMs and returns immediately (monitor threads reap them).
  return spawn_all_vms(dc, res);
}

void datacenter_wait_all(DataCenter *dc) {
  pthread_mutex_lock(&dc->res_lock);
  while (dc->running_vms > 0)
    pthread_cond_wait(&dc->idle_cond, &dc->res_lock);
  pthread_mutex_unlock(&dc->res_lock);
}

int datacenter_list(DataCenter *dc) {
  if (!dc->configured) {
    fprintf(stderr, "Data Center needs to be configured!\n");
    return 1;
  }

  // Consistent snapshot: lock every server (ascending), then the reservation
  // table, then the VM types. Same global order as everywhere else.
  servers_lock(dc, NULL);
  pthread_mutex_lock(&dc->res_lock);
  pthread_rwlock_rdlock(&dc->types_lock);

  printf("=================================== DATA CENTER STATUS ===================================\n");
  printf("\n[ PHYSICAL SERVERS ] (%zu total)\n", dc->num_servers);
  printf("------------------------------------------------------------------------------------------\n");

  for (size_t i = 0; i < dc->num_servers; i++) {
    Server *server = &dc->servers[i];

    Resources used = server->total;
    resources_sub(&used, server->available);

    printf(" Server #%zu\n", server->id);
    printf("   ├─ Resources Total     : %6.2f vCPUs | %3zu GB RAM | %3zu GB Disk\n",
            server->total.cpu, server->total.ram, server->total.disk);
    printf("   ├─ Resources Used      : %6.2f vCPUs | %3zu GB RAM | %3zu GB Disk\n",
            used.cpu, used.ram, used.disk);
    printf("   ├─ Resources Available : %6.2f vCPUs | %3zu GB RAM | %3zu GB Disk\n",
            server->available.cpu, server->available.ram, server->available.disk);
    printf("   └─ Hosted VMs (%zu):\n", server->num_hosted_vms);

    if (server->num_hosted_vms == 0) {
      printf("        (None)\n");
    } else {
      for (size_t j = 0; j < server->num_hosted_vms; j++) {
        VM *vm = server->hosted_vms[j];
        const char *prefix = (j == server->num_hosted_vms - 1) ? "└──" : "├──";

        printf("        %s VM ID : %-16s | Type: %-10s | State: %-10s",
                prefix,
                vm->id,
                vm->type ? vm->type->id : "Unknown",
                vm_state_to_string(vm->state));

        if (vm->state == VM_STATE_RUNNING) {
          printf(" | PID: %d", (int)vm->pid);
        }
        printf("\n");
      }
    }
    printf("\n");
  }

  printf("[ DEFINED VM TYPES ] (%zu registered)\n", dc->num_vm_types);
  printf("------------------------------------------------------------------------------------------\n");
  if (dc->num_vm_types == 0) {
    printf(" (None)\n\n");
  } else {
      for (size_t i = 0; i < dc->num_vm_types; i++) {
        VMType vt = dc->vm_types[i];
        printf("  * Type ID: %-12s | Requirements: %.2f vCPU, %zu GB RAM, %zu GB Disk\n",
                vt.id, vt.required.cpu, vt.required.ram, vt.required.disk);
        printf("    ├─ Executable : %s\n", vt.exec_path);
        printf("    └─ Input Dir  : %s\n\n", vt.input_folder);
      }
      printf("\n");
  }

  printf("[ RESERVATIONS ] (%zu total)\n", dc->num_reservations);
  printf("------------------------------------------------------------------------------------------\n");
  if (dc->num_reservations == 0) {
    printf(" (None)\n\n");
  } else {
    for (size_t i = 0; i < dc->num_reservations; i++) {
      Reservation *res = dc->reservations[i];
      printf("  * Reservation ID: %-12s | State: %-8s | Total VMs: %zu\n",
              res->id, res_state_to_string(res->state), res->num_vms);

      printf("    ├─ Requested Pairs:\n");
      for (size_t k = 0; k < res->items_count; k++) {
        printf("    │    • %-12s : %zu unit(s)\n", res->items[k].vm_type_id, res->items[k].amount);
      }

      printf("    └─ Allocated VMs:\n");
      if (res->num_vms == 0) {
        printf("         (None)\n");
      } else {
          for (size_t k = 0; k < res->num_vms; k++) {
            VM *vm = res->vms[k];
            const char *prefix = (k == res->num_vms - 1) ? "└──" : "├──";
            printf("         %s ID: %-15s | Host Server: #%zu\n",
                    prefix,
                    vm->id,
                    vm->server->id);
          }
      }
      printf("\n");
    }
  }

  printf("================================================================--------------------------\n\n");

  pthread_rwlock_unlock(&dc->types_lock);
  pthread_mutex_unlock(&dc->res_lock);
  servers_unlock(dc, NULL);

  return 0;
}

void datacenter_wait(unsigned int delay_ms){
  struct timespec delay = (struct timespec){delay_ms / 1000, (delay_ms % 1000) * 1000000};
  nanosleep(&delay, NULL);
}