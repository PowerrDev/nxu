#ifndef NXU_KERNEL_STATISTICS_H
#define NXU_KERNEL_STATISTICS_H
#include <kern/syscall/statistics.h>
#include <stdint.h>
#include <stdbool.h>
uint32_t statistics_record_size(uint32_t kind);
uint32_t statistics_capacity(uint32_t kind);
/* Kernel buffers only. Returns copied count or -NXU_SYS_E_*. */
int64_t statistics_snapshot(uint32_t kind, void *records, uint32_t capacity, uint32_t *total);
void statistics_note_interrupt(void);
void statistics_note_page_fault(bool user);
void statistics_cpu_snapshot(uint32_t cpu, nxu_stats_cpu_t *out);
#endif
