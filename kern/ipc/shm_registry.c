/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/ipc/shm_registry.c
 */

#include <kern/ipc/shm_registry.h>

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
		__asm__ volatile("yield");
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

	shm_registry_lock();
	shm_registry_entry_t entry = g_shm_registry[id - 1U];
	shm_registry_unlock();

	if (!entry.valid) return false;
	if (!vm_shm_reference(entry.region)) return false;

	*out_region = entry.region;
	return true;
}
