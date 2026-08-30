#ifndef NXU_KERN_CONSOLE_H
#define NXU_KERN_CONSOLE_H

#include <stdbool.h>
#include <stdint.h>

#define KCONSOLE_MAX_SINKS 4U
#define KCONSOLE_HISTORY_SIZE 32768U

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

int kprintf(const char *format, ...);

#endif
