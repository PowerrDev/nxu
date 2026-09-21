#include <kern/console/console.h>

#include <kern/lock.h>
#include <kern/machine/timer.h>
#include <platform/uart.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
	kconsole_sink_t sink;
	void *context;
	bool active;
} kconsole_sink_entry_t;

/*
 * One writer at a time: a kprintf, kputs or kputln from one CPU comes out as one
 * unbroken line. Recursive (a sink may print), taken with interrupts masked, and
 * a leaf apart from the sinks it calls (UART, display) which take nothing back.
 */
static nxu_rlock_t g_kconsole_lock = NXU_RLOCK_INIT;

static kconsole_sink_entry_t g_kconsole_sinks[KCONSOLE_MAX_SINKS];
static char g_kconsole_history[KCONSOLE_HISTORY_SIZE];
static uint32_t g_kconsole_history_head;
static uint32_t g_kconsole_history_count;
static uint64_t g_kconsole_history_sequence;
static bool g_kconsole_line_start = true;
static bool g_kconsole_verbose;
static bool g_kconsole_clock_started;
static uint64_t g_kconsole_boot_ticks;
static uint64_t g_kconsole_counter_frequency;

static void kconsole_history_append(char character)
{
	g_kconsole_history[g_kconsole_history_head] = character;
	g_kconsole_history_head = (g_kconsole_history_head + 1U) % KCONSOLE_HISTORY_SIZE;
	if (g_kconsole_history_count < KCONSOLE_HISTORY_SIZE) g_kconsole_history_count++;
	__atomic_add_fetch(&g_kconsole_history_sequence, 1ULL, __ATOMIC_RELEASE);
}

static void kconsole_replay(kconsole_sink_t sink, void *context)
{
	uint32_t start = (g_kconsole_history_head + KCONSOLE_HISTORY_SIZE - g_kconsole_history_count) % KCONSOLE_HISTORY_SIZE;
	for (uint32_t index = 0U; index < g_kconsole_history_count; index++) {
		sink(g_kconsole_history[(start + index) % KCONSOLE_HISTORY_SIZE], context);
	}
}

bool kconsole_register_sink(kconsole_sink_t sink, void *context, bool replay_history)
{
	if (sink == 0) return false;

	for (uint32_t index = 0U; index < KCONSOLE_MAX_SINKS; index++) {
		if (!g_kconsole_sinks[index].active) continue;

		if (g_kconsole_sinks[index].sink == sink && g_kconsole_sinks[index].context == context) return false;
	}

	for (uint32_t index = 0U; index < KCONSOLE_MAX_SINKS; index++) {
		if (g_kconsole_sinks[index].active) continue;
		g_kconsole_sinks[index].sink = sink;
		g_kconsole_sinks[index].context = context;
		g_kconsole_sinks[index].active = true;
		if (replay_history) kconsole_replay(sink, context);
		return true;
	}

	return false;
}

bool kconsole_unregister_sink(kconsole_sink_t sink, void *context)
{
	if (sink == 0) return false;

	for (uint32_t index = 0U; index < KCONSOLE_MAX_SINKS; index++) {
		if (!g_kconsole_sinks[index].active) continue;

		if (g_kconsole_sinks[index].sink != sink || g_kconsole_sinks[index].context != context) continue;
		g_kconsole_sinks[index].active = false;
		g_kconsole_sinks[index].sink = 0;
		g_kconsole_sinks[index].context = 0;
		return true;
	}

	return false;
}

/*
 * Emit one character without timestamp processing. Timestamp generation itself
 * uses this path so the console never recursively prefixes its own prefix.
 */
static void kconsole_emit_raw(char character)
{
	uart_putc(character);

	for (uint32_t index = 0U; index < KCONSOLE_MAX_SINKS; index++) {
		if (!g_kconsole_sinks[index].active) continue;
		g_kconsole_sinks[index].sink(character, g_kconsole_sinks[index].context);
	}

	kconsole_history_append(character);
}

/*
 * Print an unsigned decimal value directly to the raw console. This helper is
 * intentionally small because it is used before exception handling and the VM
 * subsystem are fully online.
 */
static void kconsole_emit_decimal_raw(uint64_t value)
{
	char digits[20];
	uint32_t length = 0U;

	do {
		digits[length++] = (char)('0' + value % 10ULL);
		value /= 10ULL;
	} while (value != 0ULL);

	while (length != 0U) {
		kconsole_emit_raw(digits[--length]);
	}
}

/*
 * Emit the fractional portion of one second as exactly eight decimal digits.
 *
 * Do not route this through a generic field-width formatter. Early bootstrap
 * executes with only the physical stack and raw UART available, so keeping the
 * timestamp path free of padding buffers and optimizer-generated bulk stores
 * makes the first diagnostic line safe even as the linked kernel layout grows.
 */
static void kconsole_emit_fraction_raw(uint64_t fraction)
{
	uint64_t divisor = 10000000ULL;

	for (uint32_t digit = 0U; digit < 8U; digit++) {
		uint64_t value = fraction / divisor;
		kconsole_emit_raw((char)('0' + (value % 10ULL)));
		divisor /= 10ULL;
	}
}

/*
 * Prefix each logical kernel log line with monotonic time since the first
 * console message. Eight fractional digits match NXU's XNU-inspired boot log
 * format while still deriving directly from the architectural counter.
 */
static void kconsole_emit_timestamp(void)
{
	if (!g_kconsole_clock_started) {
		g_kconsole_counter_frequency = timer_get_frequency();
		g_kconsole_boot_ticks = timer_get_ticks();
		g_kconsole_clock_started = true;
	}

	uint64_t seconds = 0ULL;
	uint64_t fraction = 0ULL;

	if (g_kconsole_counter_frequency != 0ULL) {
		uint64_t elapsed = timer_get_ticks() - g_kconsole_boot_ticks;
		seconds = elapsed / g_kconsole_counter_frequency;
		uint64_t remainder = elapsed % g_kconsole_counter_frequency;
		fraction = remainder * 100000000ULL / g_kconsole_counter_frequency;
	}

	kconsole_emit_raw('[');
	kconsole_emit_raw(' ');
	kconsole_emit_decimal_raw(seconds);
	kconsole_emit_raw('.');
	kconsole_emit_fraction_raw(fraction);
	kconsole_emit_raw(' ');
	kconsole_emit_raw(']');
	kconsole_emit_raw(' ');
}

void kputc(char character)
{
	NXU_RLOCK_GUARD(&g_kconsole_lock);

	if (g_kconsole_line_start && character != '\n') {
		kconsole_emit_timestamp();
		g_kconsole_line_start = false;
	}

	kconsole_emit_raw(character);

	if (character == '\n') {
		g_kconsole_line_start = true;
	}
}

void kputs(const char *string)
{
	NXU_RLOCK_GUARD(&g_kconsole_lock);

	if (string == 0) string = "(null)";
	while (*string != '\0') kputc(*string++);
}

void kputln(const char *string)
{
	NXU_RLOCK_GUARD(&g_kconsole_lock);

	kputs(string);
	kputc('\n');
}

void kputhex_digit(uint8_t value)
{
	if (value > 15U) return;
	kputc(value < 10U ? (char)('0' + value) : (char)('A' + value - 10U));
}

void kputhex_byte(uint8_t byte)
{
	kputhex_digit((uint8_t)((byte >> 4U) & 0x0FU));
	kputhex_digit((uint8_t)(byte & 0x0FU));
}

static void kputhex_value(uint64_t value, uint32_t digits, bool uppercase)
{
	for (uint32_t index = 0U; index < digits; index++) {
		uint32_t shift = (digits - index - 1U) * 4U;
		uint8_t digit = (uint8_t)((value >> shift) & 0x0FU);
		if (digit < 10U) kputc((char)('0' + digit));
		else kputc((char)((uppercase ? 'A' : 'a') + digit - 10U));
	}
}

void kputhex32(uint32_t value)
{
	kputs("0x");
	kputhex_value(value, 8U, true);
}

void kputhex64(uint64_t value)
{
	kputs("0x");
	kputhex_value(value, 16U, true);
}

void kputu64(uint64_t value)
{
	char digits[20];
	uint32_t length = 0U;
	if (value == 0ULL) {
		kputc('0');
		return;
	}

	while (value != 0ULL) {
		digits[length++] = (char)('0' + value % 10ULL);
		value /= 10ULL;
	}

	while (length != 0U) kputc(digits[--length]);
}

void kputi64(int64_t value)
{
	if (value < 0) {
		kputc('-');
		uint64_t magnitude = (uint64_t)(-(value + 1LL)) + 1ULL;
		kputu64(magnitude);
		return;
	}
	kputu64((uint64_t)value);
}

static int kprintf_unsigned(uint64_t value, uint32_t base, bool uppercase)
{
	char digits[64];
	uint32_t length = 0U;
	if (value == 0ULL) {
		kputc('0');
		return 1;
	}

	while (value != 0ULL) {
		uint8_t digit = (uint8_t)(value % base);
		digits[length++] = digit < 10U ? (char)('0' + digit) : (char)((uppercase ? 'A' : 'a') + digit - 10U);
		value /= base;
	}
	int written = (int)length;
	while (length != 0U) kputc(digits[--length]);
	return written;
}

static int kprintf_signed(int64_t value)
{
	if (value >= 0) return kprintf_unsigned((uint64_t)value, 10U, false);
	kputc('-');
	uint64_t magnitude = (uint64_t)(-(value + 1LL)) + 1ULL;
	return 1 + kprintf_unsigned(magnitude, 10U, false);
}

int kvprintf(const char *format, va_list arguments)
{
	NXU_RLOCK_GUARD(&g_kconsole_lock);

	if (format == 0) return 0;

	int written = 0;

	while (*format != '\0') {
		if (*format != '%') {
			kputc(*format++);
			written++;
			continue;
		}

		format++;
		if (*format == '%') {
			kputc('%');
			format++;
			written++;
			continue;
		}

		uint32_t length = 0U;
		if (*format == 'l') {
			length = 1U;
			format++;
			if (*format == 'l') {
				length = 2U;
				format++;
			}
		}

		char conversion = *format;
		if (conversion == '\0') break;
		format++;

		switch (conversion) {
		case 'c':
			kputc((char)va_arg(arguments, int));
			written++;
			break;
		case 's': {
			const char *string = va_arg(arguments, const char *);
			if (string == 0) string = "(null)";
			while (*string != '\0') {
				kputc(*string++);
				written++;
			}
			break;
		}
		case 'd':
		case 'i': {
			int64_t value;
			if (length == 0U) value = (int64_t)va_arg(arguments, int);
			else if (length == 1U) value = (int64_t)va_arg(arguments, long);
			else value = (int64_t)va_arg(arguments, long long);
			written += kprintf_signed(value);
			break;
		}
		case 'u':
		case 'x':
		case 'X': {
			uint64_t value;
			if (length == 0U) value = (uint64_t)va_arg(arguments, unsigned int);
			else if (length == 1U) value = (uint64_t)va_arg(arguments, unsigned long);
			else value = (uint64_t)va_arg(arguments, unsigned long long);
			uint32_t base = conversion == 'u' ? 10U : 16U;
			written += kprintf_unsigned(value, base, conversion == 'X');
			break;
		}
		case 'p': {
			uint64_t value = (uint64_t)va_arg(arguments, void *);
			kputs("0x");
			written += 2;
			written += kprintf_unsigned(value, 16U, false);
			break;
		}
		default:
			kputc('%');
			kputc(conversion);
			written += 2;
			break;
		}
	}

	return written;
}

int kprintf(const char *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	int written = kvprintf(format, arguments);
	va_end(arguments);
	return written;
}

void kconsole_set_verbose(bool verbose)
{
	g_kconsole_verbose = verbose;
}

bool kconsole_verbose(void)
{
	return g_kconsole_verbose;
}

void kconsole_break_lock(void)
{
	if (__atomic_load_n(&g_kconsole_lock.owner, __ATOMIC_RELAXED) == machine_cpu_id() + 1U) return;

	g_kconsole_lock.owner = 0U;
	g_kconsole_lock.depth = 0U;
	__atomic_store_n(&g_kconsole_lock.lock.value, 0U, __ATOMIC_RELEASE);
}

int kverbosef(const char *format, ...)
{
	if (!g_kconsole_verbose) return 0;

	va_list arguments;
	va_start(arguments, format);
	int written = kvprintf(format, arguments);
	va_end(arguments);
	return written;
}


bool
kconsole_history_read(uint64_t *cursor, char *buffer, uint64_t capacity, uint64_t *read_size)
{
	if (cursor == 0 || read_size == 0) return false;
	if (capacity != 0ULL && buffer == 0) return false;

	*read_size = 0ULL;
	uint64_t sequence = __atomic_load_n(&g_kconsole_history_sequence, __ATOMIC_ACQUIRE);
	uint64_t retained = g_kconsole_history_count;
	uint64_t oldest = sequence > retained ? sequence - retained : 0ULL;

	if (*cursor < oldest) *cursor = oldest;
	if (*cursor > sequence) *cursor = sequence;

	uint64_t available = sequence - *cursor;
	uint64_t count = available < capacity ? available : capacity;

	for (uint64_t index = 0ULL; index < count; index++) {
		buffer[index] = g_kconsole_history[(*cursor + index) % KCONSOLE_HISTORY_SIZE];
	}

	*cursor += count;
	*read_size = count;
	return true;
}
