/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/console/console.h
 *
 * Declares the kernel console console interfaces and data structures.
 */
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

/* Enable or disable timestamps on subsequent console lines. */
void kconsole_set_timestamp(bool enabled);

/*
 * Two tiers of boot output, each in two flavours.
 *
 * kprintf() is the milestone log: what happened and whether it passed, one
 * fact per line. It is a macro: the name of the calling function is put in
 * front of every line it starts ("vfs_mount: devfs mounted at /dev"), so a
 * format never spells its own prefix -- write kprintf("devfs mounted at
 * /dev\n") -- and the prefix can never go stale when code moves. To log under
 * another name, log from that function.
 *
 * IOLog() prints exactly what it is given: for IOKit-style family tags
 * ("VirtIOFamily: ..."), banners, panic reports, tables and the rest of a
 * line something else started.
 *
 * kverbosef()/IOLogVerbose() are the same pair for detail -- register values,
 * per-slot tables, allocator statistics -- printed only with -v or
 * log.verbose=1.
 *
 * kvprintf() is the unprefixed va_list primitive under all of them.
 */
int kprintf_from(const char *function, const char *format, ...);
int kverbosef_from(const char *function, const char *format, ...);
int IOLog(const char *format, ...);
int IOLogVerbose(const char *format, ...);
int kvprintf(const char *format, va_list arguments);

#define kprintf(...) kprintf_from(__func__, __VA_ARGS__)

/*
 * kprintf_for(function, format, ...)
 *
 * Log under the name of a function other than the caller -- usually the one
 * just called, when it is the caller that reports how the call went:
 *
 *	if (!ipc_process_test()) boot_test_fail(...);
 *	kprintf_for(ipc_process_test, "passed\n");   -> "ipc_process_test: passed"
 *
 * `function` must name a real function: it is checked at compile time, so the
 * prefix cannot drift from the code when the function is renamed.
 */
/*
 * kfslog(fs, device, format, ...) / kfsverbose(...)
 *
 * A filesystem's log line, in the shape XNU's filesystems use
 * ("(apfs) nx_mount:1528: disk1s1 ..."): the filesystem, the function and
 * source line that printed it, then the device it is about:
 *
 *	kfslog("ext4", device->name, "recovering journal for %s\n", label);
 *	-> (ext4) ext4_mount:2785: disk0s2 recovering journal for NXU System
 *
 * A filesystem wraps it once (EXT4_LOG(device, ...)) so its tag is spelled
 * in one place. kfsverbose() prints only with -v / log.verbose=1.
 */
#define kfslog(fs, device, format, ...)                                                                                \
	IOLog("(" fs ") %s:%d: %s " format, __func__, __LINE__, (device), ##__VA_ARGS__)
#define kfsverbose(fs, device, format, ...)                                                                            \
	IOLogVerbose("(" fs ") %s:%d: %s " format, __func__, __LINE__, (device), ##__VA_ARGS__)

#define kprintf_for(function, ...) ((void)sizeof(&(function)), kprintf_from(#function, __VA_ARGS__))
#define kverbosef(...) kverbosef_from(__func__, __VA_ARGS__)

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

#endif
