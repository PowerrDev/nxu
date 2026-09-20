#ifndef NXU_CACHE_H
#define NXU_CACHE_H

#include <stdbool.h>
#include <stdint.h>

/**
 * cache_init - Invalidate stale cache contents and enable the EL1 caches.
 *
 * Discovers the cache topology through CLIDR_EL1 and CTR_EL0, invalidates
 * every data and unified cache level by set and way, invalidates the
 * instruction cache, and then sets SCTLR_EL1.C and SCTLR_EL1.I.
 *
 * The MMU must already be enabled: cacheability comes from the translation
 * tables, so this fails if SCTLR_EL1.M is clear. Call it after vmm_init().
 *
 * If both caches are already enabled the existing state is accepted and only
 * the discovery data is recorded. A partially enabled state is rejected,
 * because a live data cache cannot be safely invalidated.
 *
 * Must be called exactly once, from single-threaded initialization context.
 * Not reentrant; must not be called from interrupt context.
 *
 * Return: true if both caches are enabled on return, false if the MMU is
 * disabled, the CPU reports the extended CCSIDR_EL1 format, only one cache
 * was already enabled, or the SCTLR_EL1 readback did not confirm the write.
 */
bool cache_init(void);

/**
 * cache_instruction_enabled - Report SCTLR_EL1.I.
 *
 * Reads the live register rather than cached state.
 *
 * Return: true if the instruction cache is enabled.
 */
bool cache_instruction_enabled(void);

/**
 * cache_data_enabled - Report SCTLR_EL1.C.
 *
 * Reads the live register rather than cached state.
 *
 * Return: true if the data cache is enabled.
 */
bool cache_data_enabled(void);

/**
 * cache_instruction_line_size - Minimum instruction cache line size.
 *
 * Derived from CTR_EL0.IminLine. This is the smallest line size across the
 * hierarchy, which is the safe stride for instruction-cache maintenance
 * loops.
 *
 * Return: the size in bytes, or 0 if cache_init() has not succeeded.
 */
uint32_t cache_instruction_line_size(void);

/**
 * cache_data_line_size - Minimum data or unified cache line size.
 *
 * Derived from CTR_EL0.DminLine. This is the smallest line size across the
 * hierarchy, which is the safe stride for data-cache maintenance loops.
 *
 * Return: the size in bytes, or 0 if cache_init() has not succeeded.
 */
uint32_t cache_data_line_size(void);

/**
 * cache_dump - Print the discovered cache configuration to the console.
 */
void cache_dump(void);

/**
 * cache_sync_instruction_range - Publish newly written executable bytes.
 *
 * Cleans data-cache lines to the Point of Unification, invalidates matching
 * instruction-cache lines, and executes the architectural barriers required
 * before the CPU may execute the updated range.
 */
void cache_sync_instruction_range(uint64_t address, uint64_t size);

#endif
