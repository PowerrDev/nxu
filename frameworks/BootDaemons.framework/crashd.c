#include <nxu/syscall.h>
#include <nxu/string.h>
#include <stdbool.h>
#include <stdint.h>

#define SYSTEM_REPORTS "/disk/Library/Logs/DiagnosticReports/"
#define USER_REPORTS "/disk/Users/Shared/Library/Logs/DiagnosticReports/"
#define OUTPUT_SIZE 4096U

static nxu_crash_report_t g_report;
static char g_output[OUTPUT_SIZE], g_path[512];
static uint32_t g_used;
static int64_t g_fd;
static bool g_failed;

/* The symbol map's fixed records permit binary search without loading an image
 * or parsing untrusted DWARF in the reporter. Generated at staging time. */
typedef struct {
	uint64_t start, end, function_start;
	char function[96], source[128];
} symbol_t;

typedef struct {
	char magic[8];
	uint8_t identity[32];
	uint64_t count;
} symbols_header_t;

static int64_t g_symbols;
static uint64_t g_symbol_count, g_symbol_bias;
static int32_t g_symbol_image;

static void log_message(const char *s)
{
	(void)nxu_write(1U, s, nxu_strlen(s));
}

static bool write_all(uint64_t fd, const char *s, uint64_t size)
{
	while (size != 0) {
		int64_t n = nxu_write(fd, s, size);

		if (n <= 0 || (uint64_t)n > size)
			return false;

		s += n;
		size -= (uint64_t)n;
	}

	return true;
}

static void flush(void)
{
	if (g_used && !g_failed && !write_all((uint64_t)g_fd, g_output, g_used))
		g_failed = true;
	g_used = 0;
}

static void put(char c)
{
	if (g_used == OUTPUT_SIZE)
		flush();
	g_output[g_used++] = c;
}

static void text(const char *s)
{
	while (*s)
		put(*s++);
}

static void number(uint64_t n, uint32_t base)
{
	char digits[24];
	uint32_t count = 0;

  do {
		digits[count++] = "0123456789abcdef"[n % base];
		n /= base;
	} while (n);
	while (count)
		put(digits[--count]);
}

static void hex(uint64_t n)
{
	text("0x");
	number(n, 16);
}

static bool is_app(const char *s)
{
	for (; *s; s++)
		if (s[0] == '.' && s[1] == 'a' && s[2] == 'p' && s[3] == 'p' && s[4] == '/')
			return true;

	return false;
}

static void append(char *out, uint32_t capacity, const char *s)
{
	uint32_t n = (uint32_t)nxu_strlen(out);

	while (*s && n + 1U < capacity)
		out[n++] = *s++;
	out[n] = 0;
}

static void path_number(uint64_t n)
{
	char digits[24], ordered[24];
	uint32_t count = 0, length;

  do {
		digits[count++] = (char)('0' + n % 10);
		n /= 10;
	} while (n);

  length = count;

  for (uint32_t i = 0; i < length; i++)
		ordered[i] = digits[--count];
	ordered[length] = 0;
	append(g_path, sizeof(g_path), ordered);
}

static bool read_exact(uint64_t fd, void *out, uint64_t size)
{
	uint8_t *p = out;

	while (size) {
		int64_t n = nxu_read(fd, p, size);

		if (n <= 0 || (uint64_t)n > size)
			return false;

		p += n;
		size -= (uint64_t)n;
	}

	return true;
}

static void open_symbols(const char *executable, const uint8_t identity[32])
{
	char path[512] = {0};

  symbols_header_t header = {0};
	g_symbols = -1;
	g_symbol_count = 0;
	append(path, sizeof(path), executable);
	append(path, sizeof(path), ".crashsym");

  if (!executable[0])
		return;

	int64_t fd = nxu_open(path, NXU_O_READ);

	if (fd < 0)
		return;

	bool valid = read_exact((uint64_t)fd, &header, sizeof(header));
	const char magic[8] = "NXUSYM1";

	for (uint32_t i = 0; i < 8; i++)
		if (header.magic[i] != magic[i])
			valid = false;

  for (uint32_t i = 0; i < 32; i++)
		if (header.identity[i] != identity[i])
			valid = false;

	nxu_stat_t stat = {0};

	if (nxu_stat(path, &stat) < 0 || header.count > 1000000ULL ||
		header.count > (UINT64_MAX - sizeof(header)) / sizeof(symbol_t) ||
		stat.size != sizeof(header) + header.count * sizeof(symbol_t))
		valid = false;

  if (!valid) {
		(void)nxu_close((uint64_t)fd);
		return;
	}

	g_symbols = fd;
	g_symbol_count = header.count;
}

static void select_symbols(int32_t image)
{
	if (g_symbols >= 0)
		(void)nxu_close((uint64_t)g_symbols);

  g_symbol_image = image;
	g_symbol_bias = 0;
	g_symbols = -1;
	g_symbol_count = 0;

  if (g_report.image_count == 0)
		open_symbols(g_report.executable, g_report.image_id);
	else if (image >= 0) {
		nxu_dyrld_image_t *mapped = &g_report.images[image];
		g_symbol_bias = mapped->bias;
		open_symbols(mapped->path, mapped->identity);
	}
}

static int32_t frame_image(uint64_t pc)
{
	if (g_report.image_count == 0)
		return 0;

	for (uint32_t i = 0; i < g_report.image_count; i++)
		for (uint32_t j = 0; j < g_report.images[i].segment_count; j++) {
			nxu_dyrld_segment_t *s = &g_report.images[i].segments[j];

			if ((s->flags & 1) && pc >= s->address && pc - s->address < s->size)
				return i;
		}

	return -1;
}

static bool lookup(uint64_t pc, symbol_t *out)
{
	uint64_t low = 0, high = g_symbol_count;

	while (low < high) {
		uint64_t mid = low + (high - low) / 2;

		if (nxu_seek((uint64_t)g_symbols, sizeof(symbols_header_t) + mid * sizeof(*out)) < 0 ||
			!read_exact((uint64_t)g_symbols, out, sizeof(*out)))
			return false;

		if (out->start <= pc)
			low = mid + 1;
		else
			high = mid;
	}

	if (low == 0 || nxu_seek((uint64_t)g_symbols, sizeof(symbols_header_t) + (low - 1) * sizeof(*out)) < 0 ||
		!read_exact((uint64_t)g_symbols, out, sizeof(*out)))
		return false;

	out->function[95] = 0;
	out->source[127] = 0;
	return out->start <= pc && pc < out->end;
}

static const char *signal_name(uint32_t n)
{
	switch (n) {
	case NXU_SIGSEGV:
		return "SIGSEGV";
	case NXU_SIGBUS:
		return "SIGBUS";
	case NXU_SIGILL:
		return "SIGILL";
	case NXU_SIGABRT:
		return "SIGABRT";
	case NXU_SIGFPE:
		return "SIGFPE";
	case NXU_SIGTRAP:
		return "SIGTRAP";
	case NXU_SIGQUIT:
		return "SIGQUIT";
	default:
		return "unknown";
	}
}

static void render_report(void)
{
	text("Report Version: 2\nIncident Identifier: ");
	number(g_report.incident, 10);
	text("\nProcess: ");
	text(g_report.name);
	text(" [");
	number(g_report.pid, 10);
	text("]\nPath: ");
	text(g_report.executable);
	text("\nParent Process: ");
	number(g_report.ppid, 10);
	text("\nProcess Unique ID: ");
	number(g_report.uniqueid, 10);
	text("\nUptime (microseconds): ");
	number(g_report.uptime_us, 10);
	text("\nCode Type: ARM-64\nException Type: ");
	text(signal_name(g_report.signal));
	text("\nCrashed Thread: ");
	number(g_report.faulting_tid, 10);
	text("\nSnapshot Flags: ");
	hex(g_report.flags);
	text("\nPreviously Dropped Reports: ");
	number(g_report.dropped, 10);
	text("\nSymbolication: ");
	text(g_symbols >= 0 ? "matched image SHA-256" : "unavailable (missing or mismatched symbol map)");
	text("\nRegister Scope: general registers and exception state; SIMD/FP unavailable\n");
	for (uint32_t i = 0; i < g_report.thread_count; i++) {
		nxu_crash_thread_t *t = &g_report.threads[i];
		text("\nThread ");
		number(t->tid, 10);
		if (t->tid == g_report.faulting_tid)
			text(" Crashed");
		text(":\n");
		if (!(t->flags & NXU_CRASH_REGISTERS_VALID)) {
			text("Register state unavailable\n");
			continue;
		}

		for (uint32_t j = 0; j < t->frame_count; j++) {
			number(j, 10);
			text("  ");
			hex(t->frames[j]);

			/* Return addresses name the instruction after the call. */
			uint64_t address = t->frames[j] - (j != 0 && t->frames[j] >= 4 ? 4 : 0);
			int32_t image = frame_image(address);

			if (image != g_symbol_image)
				select_symbols(image);
			if (image >= 0 && g_report.image_count) {
				text("  ");
				text(g_report.images[image].path);
			}

			uint64_t relative = address - g_symbol_bias;
			symbol_t symbol;
			if (g_symbols >= 0 && lookup(relative, &symbol)) {
				text("  ");
				text(symbol.function);
				text(" + ");
				hex(relative - symbol.function_start);
				if (symbol.source[0]) {
					text("  (");
					text(symbol.source);
					put(')');
				}
			}

			put('\n');
		}

		if (t->flags & NXU_CRASH_STACK_INCOMPLETE)
			text("Backtrace incomplete (unreadable, invalid, or bounded frame chain)\n");
		text("Thread State:\n");
		for (uint32_t j = 0; j < 31; j++) {
			put('x');
			number(j, 10);
			text(": ");
			hex(t->registers[j]);
			put(j % 3 == 2 ? '\n' : ' ');
		}

		text("\npc: ");
		hex(t->pc);
		text(" sp: ");
		hex(t->sp);
		text(" fp: ");
		hex(t->fp);
		text("\npstate: ");
		hex(t->status);
		text(" far: ");
		hex(t->fault_address);
		text(" esr: ");
		hex(t->syndrome);
		put('\n');
	}

	text("\nBinary Images:\nSHA256 ");
	for (uint32_t i = 0; i < 32; i++) {
		put("0123456789abcdef"[g_report.image_id[i] >> 4]);
		put("0123456789abcdef"[g_report.image_id[i] & 15]);
	}

	text("  ");
	text(g_report.executable);
	if (g_report.image_count == 0)
		text("\nFrameworks are statically linked into this ELF image.\n");
	else
		for (uint32_t i = 0; i < g_report.image_count; i++) {
			nxu_dyrld_image_t *image = &g_report.images[i];
			text("\nSlide ");
			hex(image->bias);
			text(" SHA256 ");
			for (uint32_t j = 0; j < 32; j++) {
				put("0123456789abcdef"[image->identity[j] >> 4]);
				put("0123456789abcdef"[image->identity[j] & 15]);
			}

			text("  ");
			text(image->path);
		}

	put('\n');
	flush();
}

int main(void)
{
	log_message("crashd: listening for fatal process exceptions\n");
	for (;;) {
		int64_t result = nxu_crash(NXU_CRASH_NEXT, (uint64_t)(uintptr_t)&g_report);

		if (result == -NXU_SYS_E_INTERRUPTED)
			continue;

		if (result < 0)
			return 1;

		if (g_report.version != NXU_CRASH_VERSION || g_report.struct_size != sizeof(g_report) ||
			g_report.thread_count > NXU_CRASH_THREADS || g_report.image_count > NXU_CRASH_IMAGES)
			return 2;

		g_path[0] = 0;
		append(g_path, sizeof(g_path), is_app(g_report.executable) ? USER_REPORTS : SYSTEM_REPORTS);
		append(g_path, sizeof(g_path), "crash-");
		path_number(g_report.uptime_us);
		append(g_path, sizeof(g_path), "-");
		path_number(g_report.uniqueid);
		append(g_path, sizeof(g_path), "-");
		path_number(g_report.incident);
		append(g_path, sizeof(g_path), ".crash");
		g_fd = nxu_open(g_path, NXU_O_WRITE | NXU_O_CREATE | NXU_O_TRUNCATE);
		if (g_fd < 0) {
			log_message("crashd: unable to create report; retaining event for retry\n");
			(void)nxu_sleep_us(1000000);
			continue;
		}

		g_used = 0;
		g_failed = false;
		g_symbols = -1;
		select_symbols(0);
		render_report();
		if (g_symbols >= 0)
			(void)nxu_close((uint64_t)g_symbols);
		if (nxu_close((uint64_t)g_fd) < 0)
			g_failed = true;
		if (!g_failed && nxu_sync() < 0)
			g_failed = true;
		if (g_failed) {
			log_message("crashd: report write or sync failed; retaining event for retry\n");
			(void)nxu_unlink(g_path);
			(void)nxu_sleep_us(1000000);
			continue;
		}

		log_message("crashd: saved ");
		log_message(g_path);
		log_message("\n");

		nxu_crash_notification_t notification = {.incident = g_report.incident};
		append(notification.name, sizeof(notification.name), g_report.name);
		append(notification.report, sizeof(notification.report), g_path);
		append(notification.executable, sizeof(notification.executable), g_report.executable);

		/* Report persistence never depends on the desktop or notification queue. */
		if (nxu_crash(NXU_CRASH_NOTIFY, (uint64_t)(uintptr_t)&notification) < 0)
			log_message("crashd: report saved; desktop notification queue unavailable\n");
		if (nxu_crash(NXU_CRASH_ACK, g_report.incident) < 0)
			return 3;
	}
}
