#ifndef NXU_KERN_CONSOLE_H
#define NXU_KERN_CONSOLE_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#define KCONSOLE_MAX_SINKS 4U
/*
 * Large enough to hold the full pre-splash boot log (PMM/VMM/heap/VFS/thread
 * self-tests and their dumps easily exceed the old 32KiB), so the boot splash
 * text overlay's history replay shows the log from the very first line
 * instead of only whatever tail still fit in a smaller ring buffer.
 */
#define KCONSOLE_HISTORY_SIZE 262144U

typedef void (*kconsole_sink_t)(char character, void *context);

/*
 * Register a secondary console sink. UART is always the primary early sink.
 * When replay_history is true, the sink receives retained boot output before
 * it starts receiving live characters.
 */
bool kconsole_register_sink(kconsole_sink_t sink, void *context, bool replay_history);
bool kconsole_unregister_sink(kconsole_sink_t sink, void *context);

/* Read retained console bytes using a monotonic sequence cursor. */
bool kconsole_history_read(uint64_t *cursor, char *buffer, uint64_t capacity, uint64_t *read_size);

void kputc(char character);
void kputs(const char *string);
void kputln(const char *string);
void kputhex_digit(uint8_t value);
void kputhex_byte(uint8_t byte);
void kputhex32(uint32_t value);
void kputhex64(uint64_t value);
void kputu64(uint64_t value);
void kputi64(int64_t value);

/*
 * Microseconds since the first log line: the clock the log's [ s.fraction ]
 * stamps read, and the closest thing to time since boot on every
 * architecture (i386's TSC under HVF counts from the host's power-on).
 */
uint64_t kconsole_uptime_us(void);

int kprintf(const char *format, ...);
int kvprintf(const char *format, va_list arguments);

/*
 * Two tiers of boot output. kprintf() is the milestone log: what happened
 * and whether it passed, one fact per line, prefixed with the function that
 * printed it. kverbosef() is the detail behind it -- register values, per-slot
 * tables, allocator statistics -- and prints only when the `-v` boot-arg is
 * set (`make run BOOT_ARGS=-v`), so a normal boot, and the boot splash that
 * mirrors it, is not buried in state dumps. Both take the same formats.
 */
void kconsole_set_verbose(bool verbose);
bool kconsole_verbose(void);

/*
 * kconsole_break_lock
 *
 * For a panic that has just stopped the other CPUs: one of them may have been
 * stopped holding the console lock, and the panic message must get out anyway.
 * Frees the lock unless the calling CPU itself holds it.
 */
void kconsole_break_lock(void);
int kverbosef(const char *format, ...);

#endif
