#ifndef NXU_BOOTD_REGISTRY_H
#define NXU_BOOTD_REGISTRY_H

#include <nxu/bootstrap_protocol.h>

#include <stdbool.h>
#include <stdint.h>

#define BOOTD_REGISTRY_MAX_ENTRIES 32U

typedef struct {
	char label[NXU_BOOTSTRAP_LABEL_MAX];
	uint32_t port_name;
} bootd_registry_entry_t;

/*
 * bootd's side of the bootstrap registry: a name -> local-port-name table,
 * served on the well-known port every process inherits a send right to at
 * spawn (see kern/loader/elf.c, frameworks/include/nxu/bootstrap_protocol.h).
 */
typedef struct {
	uint32_t port_name;
	bootd_registry_entry_t entries[BOOTD_REGISTRY_MAX_ENTRIES];
	uint32_t entry_count;
} bootd_registry_t;

bool bootd_registry_init(bootd_registry_t *registry);
void bootd_registry_poll(bootd_registry_t *registry);

#endif
