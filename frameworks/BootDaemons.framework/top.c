#include <nxu/syscall.h>
#include <nxu/string.h>
#include <stdbool.h>
#include <stdint.h>

#define TOP_CPUS 8U
#define TOP_PROCESSES 64U

typedef struct {
	nxu_stats_system_t system;
	nxu_stats_cpu_t cpus[TOP_CPUS];
	nxu_stats_cpu_events_t events[TOP_CPUS];
	nxu_stats_process_t processes[TOP_PROCESSES];
	nxu_stats_process_memory_t memory[TOP_PROCESSES];
	nxu_stats_virtio_t virtio;
	uint32_t cpu_count, event_count, process_count, memory_count;
	bool truncated;
} top_sample_t;

static top_sample_t g_samples[2];
static uint32_t g_order[TOP_PROCESSES];
static uint64_t g_usage[TOP_PROCESSES];
static char g_output[8192];
static uint32_t g_length;
static bool g_write_failed;

static void flush(void)
{
	if (g_length != 0U && nxu_write(1ULL, g_output, g_length) != (int64_t)g_length)
		g_write_failed = true;
	g_length = 0U;
}

static void put(char c)
{
	if (g_length == sizeof(g_output))
		flush();
	g_output[g_length++] = c;
}

static void text(const char *s)
{
	while (*s != '\0')
		put(*s++);
}

static void number(uint64_t value, uint32_t width)
{
	char digits[24];
	uint32_t n = 0U;
	do {
		digits[n++] = (char)('0' + value % 10ULL);
		value /= 10ULL;
	} while (value != 0ULL);
	for (uint32_t i = n; i < width; i++)
		put(' ');
	while (n != 0U)
		put(digits[--n]);
}

static uint64_t delta(uint64_t now, uint64_t before)
{
	return now >= before ? now - before : 0ULL;
}

static void percent(uint64_t tenths)
{
	number(tenths / 10ULL, 4U);
	put('.');
	put((char)('0' + tenths % 10ULL));
}

static int64_t read_stats(uint32_t kind, void *out, uint32_t size, uint32_t capacity, bool *truncated)
{
	nxu_stats_request_t request = {.struct_size = sizeof(request),
								   .version = NXU_STATS_VERSION,
								   .kind = kind,
								   .record_size = size,
								   .records = (uint64_t)(uintptr_t)out,
								   .capacity = capacity};

	int64_t count = nxu_statistics(&request);

	if (count >= 0 && request.count < request.total)
		*truncated = true;
	return count;
}

static bool sample(top_sample_t *s)
{
	s->truncated = false;

	int64_t n;

	if (read_stats(NXU_STATS_SYSTEM, &s->system, sizeof(s->system), 1U, &s->truncated) != 1)
		return false;

	n = read_stats(NXU_STATS_CPU, s->cpus, sizeof(s->cpus[0]), TOP_CPUS, &s->truncated);
	if (n < 0)
		return false;

	s->cpu_count = (uint32_t)n;
	n = read_stats(NXU_STATS_CPU_EVENTS, s->events, sizeof(s->events[0]), TOP_CPUS, &s->truncated);
	if (n < 0)
		return false;

	s->event_count = (uint32_t)n;
	n = read_stats(NXU_STATS_PROCESS, s->processes, sizeof(s->processes[0]), TOP_PROCESSES, &s->truncated);
	if (n < 0)
		return false;

	s->process_count = (uint32_t)n;
	n = read_stats(NXU_STATS_PROCESS_MEMORY, s->memory, sizeof(s->memory[0]), TOP_PROCESSES, &s->truncated);
	if (n < 0)
		return false;

	s->memory_count = (uint32_t)n;
	return read_stats(NXU_STATS_VIRTIO, &s->virtio, sizeof(s->virtio), 1U, &s->truncated) == 1;
}

static const nxu_stats_process_memory_t *memory_for(const top_sample_t *s, uint64_t id)
{
	for (uint32_t i = 0U; i < s->memory_count; i++)
		if (s->memory[i].uniqueid == id)
			return &s->memory[i];

	return 0;
}

static void render(const top_sample_t *s, const top_sample_t *previous, bool plain)
{
	if (!plain)
		text("\033[H\033[2J");
	text("NXU top  uptime ");
	number(s->system.sampled_us / 1000000ULL, 0U);
	text("s  processes ");
	number(s->system.process_count, 0U);
	text("  threads ");
	number(s->system.thread_count, 0U);
	put('\n');

	uint64_t kb_per_page = s->system.page_size / 1024U;
	text("Memory KiB: used ");
	number((s->system.total_pages - s->system.free_pages) * kb_per_page, 0U);
	text("  free ");
	number(s->system.free_pages * kb_per_page, 0U);
	text("  heap ");
	number(s->system.heap_pages * kb_per_page, 0U);
	put('\n');

	uint64_t switches = 0ULL, faults = 0ULL, interrupts = 0ULL;

	for (uint32_t i = 0U; i < s->cpu_count; i++) {
		const nxu_stats_cpu_t *cpu = &s->cpus[i];
		text("CPU");
		number(cpu->cpu_id, 0U);
		put(' ');

		uint64_t usage = 0ULL;

		if (i < previous->cpu_count && cpu->cpu_id == previous->cpus[i].cpu_id) {
			uint64_t elapsed = delta(cpu->sampled_us, previous->cpus[i].sampled_us);

			if (elapsed != 0ULL)
				usage = delta(cpu->busy_us, previous->cpus[i].busy_us) * 1000ULL / elapsed;
		}

		percent(usage);
		text("%  queue ");
		number(cpu->run_queue_depth, 0U);
		if (i < s->event_count) {
			text("  faults ");
			number(s->events[i].page_faults, 0U);
			text("  IRQs ");
			number(s->events[i].interrupts, 0U);
		}

		if (!cpu->online)
			text(" offline");
		put('\n');
		switches += cpu->context_switches;
	}

	for (uint32_t i = 0U; i < s->event_count; i++) {
		faults += s->events[i].page_faults;
		interrupts += s->events[i].interrupts;
	}

	text("Totals: switches ");
	number(switches, 0U);
	text("  faults ");
	number(faults, 0U);
	text("  IRQs ");
	number(interrupts, 0U);
	put('\n');
	text("VirtIO totals: submitted ");
	number(s->virtio.submissions, 0U);
	text("  completed ");
	number(s->virtio.completions, 0U);
	text("  descriptor exhaustion ");
	number(s->virtio.descriptor_exhaustions, 0U);
	put('\n');
	text("VirtIO: used bytes ");
	number(s->virtio.used_bytes, 0U);
	text("  rejected ");
	number(s->virtio.rejected_submissions, 0U);
	text("  invalid completions ");
	number(s->virtio.invalid_completions, 0U);
	text("  queues ");
	number(s->virtio.live_queues, 0U);
	put('\n');
	text("   PID   PPID  CPU% THR   RES KiB   SHM KiB  ANON KiB   FAULTS NAME\n");

	uint64_t elapsed = delta(s->system.sampled_us, previous->system.sampled_us);

	for (uint32_t i = 0U; i < s->process_count; i++) {
		g_order[i] = i;
		g_usage[i] = 0ULL;
		for (uint32_t j = 0U; j < previous->process_count; j++) {
			if (s->processes[i].uniqueid == previous->processes[j].uniqueid && elapsed != 0ULL)
				g_usage[i] = delta(s->processes[i].cpu_us, previous->processes[j].cpu_us) * 1000ULL / elapsed;
		}
	}

	for (uint32_t i = 1U; i < s->process_count; i++) {
		uint32_t id = g_order[i], j = i;

		while (j > 0U && g_usage[g_order[j - 1U]] < g_usage[id]) {
			g_order[j] = g_order[j - 1U];
			j--;
		}

		g_order[j] = id;
	}

	for (uint32_t row = 0U; row < s->process_count; row++) {
		uint32_t i = g_order[row];
		const nxu_stats_process_t *p = &s->processes[i];
		const nxu_stats_process_memory_t *m = memory_for(s, p->uniqueid);
		number(p->pid, 6U);
		put(' ');
		number(p->ppid, 6U);
		put(' ');
		percent(g_usage[i]);
		put(' ');
		number(p->threads, 3U);
		if (m != 0 && (m->flags & NXU_STATS_MEMORY_KERNEL) == 0U) {
			put(' ');
			number(m->resident_pages * kb_per_page, 9U);
			put(' ');
			number(m->shared_pages * kb_per_page, 9U);
			put(' ');
			number(m->anon_reserved_pages * kb_per_page, 9U);
			put(' ');
			number(m->page_faults, 8U);
		} else
			text("         -         -         -        -");
		put(' ');

		/* Process names are untrusted; never emit control sequences. */
		for (uint32_t c = 0U; c < sizeof(p->name) && p->name[c] != '\0'; c++)
			put(p->name[c] >= ' ' && p->name[c] <= '~' ? p->name[c] : '?');
		put('\n');
	}

	text("RES counts mappings, including COW/shared pages; ANON is reservation size.\n");
	text("CPU% sums live threads (100% per core); thread exit may reduce totals.\n");
	if (s->truncated)
		text("Some tables were truncated; increase top's capacities.\n");
	flush();
}

#if defined(__i386__)
int main(void)
#else
int main(int argc, char **argv)
#endif
{
	char boot_args[1024];
	bool once = false, plain = false;

	if (nxu_get_boot_args(boot_args, sizeof(boot_args)) >= 0) {
		once = nxu_arg_present(boot_args, "top.once");
		plain = nxu_arg_present(boot_args, "top.plain");
	}
#if !defined(__i386__)
	for (int i = 1; i < argc && argc <= (int)NXU_EXEC_ARGV_MAX; i++) {
		if (nxu_streq(argv[i], "--once"))
			once = true;
		else if (nxu_streq(argv[i], "--plain"))
			plain = true;
		else {
			text("usage: top [--once] [--plain]\n");
			flush();
			return 2;
		}
	}
#endif

	uint32_t current = 0U;

	if (!sample(&g_samples[current])) {
		text("top: statistics query failed\n");
		flush();
		return 1;
	}

	do {
		if (nxu_sleep_us(1000000ULL) < 0)
			return 1;

		uint32_t next = current ^ 1U;

		if (!sample(&g_samples[next])) {
			text("top: statistics query failed\n");
			flush();
			return 1;
		}

		render(&g_samples[next], &g_samples[current], plain);
		if (g_write_failed)
			return 1;

		current = next;
	} while (!once);
	return 0;
}
