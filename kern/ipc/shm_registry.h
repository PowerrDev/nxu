/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/ipc/shm_registry.h
 *
 * Global id-based lookup for vm_shm regions, so a small-integer id (not a
 * kernel pointer) can travel as plain data inside an NXPC message payload
 * and let the receiver attach to the very same region. A simpler stand-in
 * for routing shared memory through ipc_kmsg's single transferable-name
 * slot (IPC_KMSG_XFER_MEMORY, declared but not yet wired in
 * kern/ipc/ipc_types.h) -- no per-name access control, any process that
 * learns an id can attach to it. Acceptable for this milestone, matching
 * the other "no general capability lattice" simplifications already
 * accepted elsewhere in the IPC work.
 */

#ifndef NXU_KERN_IPC_SHM_REGISTRY_H
#define NXU_KERN_IPC_SHM_REGISTRY_H

#include <vm/vm_shm.h>

#include <stdbool.h>
#include <stdint.h>

#define SHM_REGISTRY_ID_INVALID 0U

/*
 * Publishes region under a new id, taking ownership of the caller's handle
 * reference (mirrors vm_shm_release's ownership-transfer convention).
 */
bool shm_registry_publish(vm_shm_region_t region, uint32_t *out_id);

/*
 * Returns a new handle reference (vm_shm_reference) to the region
 * registered under id, or false if id is unknown. The caller owns the
 * returned reference.
 */
bool shm_registry_attach(uint32_t id, vm_shm_region_t *out_region);

/*
 * Removes id from the registry and releases the reference publish gave it
 * (vm_shm_release): false if id is unknown (already withdrawn, or never
 * valid). Any reference a concurrent or earlier shm_registry_attach handed
 * out is untouched and keeps the region alive for that caller regardless of
 * this call -- see vm/vm_shm.h's usage protocol. Once withdrawn, id is free
 * to be handed out again by a future shm_registry_publish.
 */
bool shm_registry_withdraw(uint32_t id);

#endif
