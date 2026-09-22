/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/ipc/shm_registry.c
 */

#include <kern/ipc/shm_registry.h>

#include <kern/machine/cpu.h>

#include <stdbool.h>
#include <stdint.h>

#define SHM_REGISTRY_MAX 64U

typedef struct {
	vm_shm_region_t region;
	bool valid;
} shm_registry_entry_t;

static shm_registry_entry_t g_shm_registry[SHM_REGISTRY_MAX];
static volatile uint32_t g_shm_registry_lock;

static void
shm_registry_lock(void)
{
	while (__atomic_exchange_n(&g_shm_registry_lock, 1U, __ATOMIC_ACQUIRE) != 0U) {
		cpu_relax();
	}
}

static void
shm_registry_unlock(void)
{
	__atomic_store_n(&g_shm_registry_lock, 0U, __ATOMIC_RELEASE);
}

bool
shm_registry_publish(vm_shm_region_t region, uint32_t *out_id)
{
	if (out_id != 0) *out_id = SHM_REGISTRY_ID_INVALID;
	if (region == VM_SHM_REGION_NULL || out_id == 0) return false;

	shm_registry_lock();

	for (uint32_t index = 0U; index < SHM_REGISTRY_MAX; index++) {
		if (g_shm_registry[index].valid) continue;

		g_shm_registry[index].region = region;
		g_shm_registry[index].valid = true;
		shm_registry_unlock();

		*out_id = index + 1U;
		return true;
	}

	shm_registry_unlock();
	return false;
}

bool
shm_registry_attach(uint32_t id, vm_shm_region_t *out_region)
{
	if (out_region != 0) *out_region = VM_SHM_REGION_NULL;
	if (id == SHM_REGISTRY_ID_INVALID || id > SHM_REGISTRY_MAX || out_region == 0) return false;

	/*
	 * The reference is taken while the entry is still held stable by the lock: a
	 * region looked up here and referenced afterwards could be released and
	 * destroyed by another CPU in between.
	 */
	shm_registry_lock();

	shm_registry_entry_t entry = g_shm_registry[id - 1U];
	bool referenced = entry.valid && vm_shm_reference(entry.region);

	shm_registry_unlock();

	if (!referenced) return false;

	*out_region = entry.region;
	return true;
}

bool
shm_registry_withdraw(uint32_t id)
{
	if (id == SHM_REGISTRY_ID_INVALID || id > SHM_REGISTRY_MAX) return false;

	/*
	 * Take the entry and invalidate the slot atomically with the lookup, the
	 * same way attach takes its reference under the lock: otherwise two CPUs
	 * withdrawing the same id race to double-release, and a third attaching
	 * it in between could be handed a reference to a region this call is
	 * about to drop its own reference on.
	 */
	shm_registry_lock();

	shm_registry_entry_t entry = g_shm_registry[id - 1U];

	if (entry.valid) {
		g_shm_registry[id - 1U].valid = false;
		g_shm_registry[id - 1U].region = VM_SHM_REGION_NULL;
	}

	shm_registry_unlock();

	if (!entry.valid) return false;

	vm_shm_release(entry.region);
	return true;
}
