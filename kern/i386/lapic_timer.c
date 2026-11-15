/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/lapic_timer.c
 *
 * See lapic_timer.h.
 *
 * The timer's registers are per CPU (each CPU's Local APIC window shows its
 * own), as is IA32_TSC_DEADLINE, so arming and disarming touch only the
 * calling CPU's timer. The event timer only ever calls them on the CPU
 * concerned, with interrupts masked.
 *
 * Deadlines arrive as microseconds on the monotonic clock, which is the TSC
 * scaled down (timer_get_microseconds), so the TSC value for a deadline is
 * the smallest one that reads back as that microsecond or later: the
 * interrupt can never come before its deadline.
 */

#include <kern/i386/lapic_timer.h>

#include <kern/clock/clock.h>
#include <kern/clock/etimer.h>
#include <kern/console/console.h>
#include <kern/i386/apic.h>
#include <kern/i386/boot_info.h>
#include <kern/i386/cpu.h>
#include <kern/i386/irq.h>
#include <kern/i386/pic.h>
#include <kern/i386/timer.h>
#include <kern/i386/trap.h>
#include <kern/machine/machine_routines.h>
#include <kern/machine/smp.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Local APIC timer registers (offsets in the APIC window). */
#define LAPIC_REG_LVT_TIMER 0x320U
#define LAPIC_REG_TIMER_INITIAL 0x380U
#define LAPIC_REG_TIMER_CURRENT 0x390U
#define LAPIC_REG_TIMER_DIVIDE 0x3E0U

#define LAPIC_LVT_MASKED (1U << 16U)
#define LAPIC_LVT_ONESHOT (0U << 17U)
#define LAPIC_LVT_TSC_DEADLINE (2U << 17U)

/* Divide configuration 0b0011: the timer counts at the bus clock / 16. */
#define LAPIC_TIMER_DIVIDE_16 0x3U

#define MSR_IA32_TSC_DEADLINE 0x6E0U

#define CPUID_1_ECX_TSC_DEADLINE (1U << 24U)
#define CPUID_80000007_EDX_INVARIANT_TSC (1U << 8U)

/* The mode check: a deadline this far ahead, which must arrive within the tolerance. */
#define LAPIC_TIMER_PROBE_DELAY_US 2000ULL
#define LAPIC_TIMER_PROBE_WAIT_US 100000ULL
#define LAPIC_TIMER_PROBE_LATE_US 20000ULL

/* One-shot calibration: three countdowns of 10 ms, keeping the median rate. */
#define LAPIC_TIMER_CALIBRATION_MS 10ULL
#define LAPIC_TIMER_CALIBRATION_ROUNDS 3U

/* One-shot arming: no further ahead than this in one go (the event timer re-arms at the end). */
#define LAPIC_TIMER_ONESHOT_MAX_US 10000000ULL

static lapic_timer_mode_t g_lapic_timer_mode = LAPIC_TIMER_NONE;
static uint64_t g_lapic_timer_frequency;
static uint32_t g_lapic_timer_cpus_ready;

/* The mode check in progress, and what it saw. */
static volatile bool g_lapic_timer_probing;
static volatile uint32_t g_lapic_timer_probe_hits;
static volatile uint64_t g_lapic_timer_probe_at;

lapic_timer_mode_t
lapic_timer_mode(void)
{
	return g_lapic_timer_mode;
}

static const char *
lapic_timer_name_of(lapic_timer_mode_t mode)
{
	switch (mode) {
	case LAPIC_TIMER_TSC_DEADLINE:
		return "TSC-deadline";
	case LAPIC_TIMER_ONESHOT:
		return "one-shot";
	default:
		return "none";
	}
}

const char *
lapic_timer_mode_name(void)
{
	return lapic_timer_name_of(g_lapic_timer_mode);
}

uint32_t
lapic_timer_cpus_ready(void)
{
	return __atomic_load_n(&g_lapic_timer_cpus_ready, __ATOMIC_ACQUIRE);
}

uint64_t
lapic_timer_frequency(void)
{
	return g_lapic_timer_mode == LAPIC_TIMER_ONESHOT ? g_lapic_timer_frequency : 0ULL;
}

/* ---- programming ---------------------------------------------------------- */

/* The first TSC value that reads back as `deadline_us` or later. */
static uint64_t
lapic_timer_tsc_for(uint64_t deadline_us)
{
	uint64_t frequency = timer_get_frequency();

	return deadline_us / 1000000ULL * frequency + (deadline_us % 1000000ULL * frequency + 999999ULL) / 1000000ULL;
}

static void
lapic_timer_deadline_arm(uint64_t deadline_us)
{
	uint64_t tsc = lapic_timer_tsc_for(deadline_us);

	/* 0 disarms the timer; a deadline at the very start of time is due anyway. */
	cpu_wrmsr(MSR_IA32_TSC_DEADLINE, tsc != 0ULL ? tsc : 1ULL);
}

static void
lapic_timer_deadline_disarm(void)
{
	cpu_wrmsr(MSR_IA32_TSC_DEADLINE, 0ULL);
}

static void
lapic_timer_oneshot_arm(uint64_t deadline_us)
{
	uint64_t now = clock_uptime_us();
	uint64_t delta = deadline_us > now ? deadline_us - now : 0ULL;

	if (delta > LAPIC_TIMER_ONESHOT_MAX_US) delta = LAPIC_TIMER_ONESHOT_MAX_US;

	uint64_t count = (delta * g_lapic_timer_frequency + 999999ULL) / 1000000ULL;

	if (count == 0ULL) count = 1ULL;
	if (count > 0xFFFFFFFFULL) count = 0xFFFFFFFFULL;

	/* Writing the initial count starts (or restarts) the countdown. */
	apic_write_register(LAPIC_REG_TIMER_INITIAL, (uint32_t)count);
}

static void
lapic_timer_oneshot_disarm(void)
{
	apic_write_register(LAPIC_REG_TIMER_INITIAL, 0U);
}

static const etimer_hw_t g_lapic_timer_deadline_hw = {
	.name = "Local APIC timer (TSC-deadline)",
	.arm = lapic_timer_deadline_arm,
	.disarm = lapic_timer_deadline_disarm,
};

static const etimer_hw_t g_lapic_timer_oneshot_hw = {
	.name = "Local APIC timer (one-shot)",
	.arm = lapic_timer_oneshot_arm,
	.disarm = lapic_timer_oneshot_disarm,
};

/* Point the calling CPU's timer at T_LAPIC_TIMER in `mode`, disarmed. */
static void
lapic_timer_setup_local(lapic_timer_mode_t mode)
{
	apic_write_register(LAPIC_REG_LVT_TIMER, LAPIC_LVT_MASKED | T_LAPIC_TIMER);
	apic_write_register(LAPIC_REG_TIMER_INITIAL, 0U);

	if (mode == LAPIC_TIMER_ONESHOT) {
		apic_write_register(LAPIC_REG_TIMER_DIVIDE, LAPIC_TIMER_DIVIDE_16);
		apic_write_register(LAPIC_REG_LVT_TIMER, LAPIC_LVT_ONESHOT | T_LAPIC_TIMER);
		return;
	}

	apic_write_register(LAPIC_REG_LVT_TIMER, LAPIC_LVT_TSC_DEADLINE | T_LAPIC_TIMER);

	/*
	 * The SDM requires the switch to TSC-deadline mode (a memory-mapped LVT
	 * write) to be ordered before the first IA32_TSC_DEADLINE write, which
	 * is an MSR write and not ordered with it on its own.
	 */
	__asm__ volatile("mfence" : : : "memory");
	cpu_wrmsr(MSR_IA32_TSC_DEADLINE, 0ULL);
}

static void
lapic_timer_mask_local(void)
{
	apic_write_register(LAPIC_REG_LVT_TIMER, LAPIC_LVT_MASKED | T_LAPIC_TIMER);
	apic_write_register(LAPIC_REG_TIMER_INITIAL, 0U);
}

/* ---- the interrupt -------------------------------------------------------- */

void
i386_trap_lapic_timer(x86_saved_state_t *state)
{
	(void)state;

	/* Acknowledged first: a deadline programmed while the expired work runs must be able to interrupt again. */
	apic_eoi();

	if (g_lapic_timer_probing) {
		g_lapic_timer_probe_at = clock_uptime_us();
		g_lapic_timer_probe_hits++;
		return;
	}

	etimer_intr();
}

/* ---- choosing a mode ------------------------------------------------------ */

/*
 * Arm `mode` on the boot CPU for a deadline LAPIC_TIMER_PROBE_DELAY_US ahead
 * and check exactly one interrupt comes, not before the deadline (one-shot:
 * not by more than the calibration can be off), and not very late.
 */
static bool
lapic_timer_probe(lapic_timer_mode_t mode)
{
	const char *name = lapic_timer_name_of(mode);
	bool enabled = ml_irq_enabled();

	kprintf("checking %s mode\n", name);

	lapic_timer_setup_local(mode);

	g_lapic_timer_probe_hits = 0U;
	g_lapic_timer_probe_at = 0ULL;
	__atomic_store_n(&g_lapic_timer_probing, true, __ATOMIC_RELEASE);

	uint64_t start = clock_uptime_us();
	uint64_t deadline = start + LAPIC_TIMER_PROBE_DELAY_US;

	ml_irq_enable();

	uint64_t irq_state = ml_irq_save();

	if (mode == LAPIC_TIMER_TSC_DEADLINE) {
		lapic_timer_deadline_arm(deadline);
	} else {
		lapic_timer_oneshot_arm(deadline);
	}

	ml_irq_restore(irq_state);

	while (g_lapic_timer_probe_hits == 0U && clock_uptime_us() - start < LAPIC_TIMER_PROBE_WAIT_US)
		cpu_relax();

	/* Long enough for a second, wrong, interrupt to show up as well. */
	timer_delay_ms(5ULL);

	irq_state = ml_irq_save();
	lapic_timer_mask_local();
	if (mode == LAPIC_TIMER_TSC_DEADLINE) lapic_timer_deadline_disarm();
	__atomic_store_n(&g_lapic_timer_probing, false, __ATOMIC_RELEASE);
	ml_irq_restore(irq_state);

	if (!enabled) ml_irq_disable();

	uint32_t hits = g_lapic_timer_probe_hits;
	uint64_t at = g_lapic_timer_probe_at;

	if (hits == 0U) {
		kprintf("%s mode never interrupted (waited %llu ms for a %llu us deadline)\n", name,
				(unsigned long long)(LAPIC_TIMER_PROBE_WAIT_US / 1000ULL),
				(unsigned long long)LAPIC_TIMER_PROBE_DELAY_US);
		return false;
	}

	if (hits != 1U) {
		kprintf("%s mode interrupted %u times for one deadline\n", name, hits);
		return false;
	}

	/* One-shot rests on a measured rate: allow it to be a little early, which the event timer absorbs. */
	uint64_t early_allowed = mode == LAPIC_TIMER_ONESHOT ? LAPIC_TIMER_PROBE_DELAY_US / 8ULL : 0ULL;

	if (at + early_allowed < deadline) {
		kprintf("%s mode interrupted %llu us before its deadline\n", name, (unsigned long long)(deadline - at));
		return false;
	}

	if (at > deadline + LAPIC_TIMER_PROBE_LATE_US) {
		kprintf("%s mode interrupted %llu us after its deadline\n", name, (unsigned long long)(at - deadline));
		return false;
	}

	if (at >= deadline) {
		kprintf("%s mode works: interrupted %llu us after a %llu us deadline\n", name,
				(unsigned long long)(at - deadline), (unsigned long long)LAPIC_TIMER_PROBE_DELAY_US);
	} else {
		kprintf("%s mode works: interrupted %llu us before a %llu us deadline (within the calibration's error)\n",
				name, (unsigned long long)(deadline - at), (unsigned long long)LAPIC_TIMER_PROBE_DELAY_US);
	}

	return true;
}

/* Measure the one-shot count rate against the TSC: the median of a few countdowns. */
static bool
lapic_timer_calibrate(void)
{
	uint64_t rates[LAPIC_TIMER_CALIBRATION_ROUNDS];

	apic_write_register(LAPIC_REG_LVT_TIMER, LAPIC_LVT_MASKED | T_LAPIC_TIMER);
	apic_write_register(LAPIC_REG_TIMER_DIVIDE, LAPIC_TIMER_DIVIDE_16);

	for (uint32_t round = 0U; round < LAPIC_TIMER_CALIBRATION_ROUNDS; round++) {
		uint64_t irq_state = ml_irq_save();

		apic_write_register(LAPIC_REG_TIMER_INITIAL, 0xFFFFFFFFU);
		uint64_t start = timer_get_ticks();

		timer_delay_ms(LAPIC_TIMER_CALIBRATION_MS);

		uint32_t current = apic_read_register(LAPIC_REG_TIMER_CURRENT);
		uint64_t elapsed_us = timer_ticks_to_microseconds(timer_get_ticks() - start);

		ml_irq_restore(irq_state);

		uint64_t counted = 0xFFFFFFFFULL - current;

		rates[round] = elapsed_us != 0ULL ? counted * 1000000ULL / elapsed_us : 0ULL;
	}

	apic_write_register(LAPIC_REG_TIMER_INITIAL, 0U);

	/* Sort three. */
	for (uint32_t i = 0U; i < LAPIC_TIMER_CALIBRATION_ROUNDS; i++) {
		for (uint32_t j = i + 1U; j < LAPIC_TIMER_CALIBRATION_ROUNDS; j++) {
			if (rates[j] < rates[i]) {
				uint64_t swap = rates[i];

				rates[i] = rates[j];
				rates[j] = swap;
			}
		}
	}

	g_lapic_timer_frequency = rates[LAPIC_TIMER_CALIBRATION_ROUNDS / 2U];

	if (g_lapic_timer_frequency < 1000ULL) {
		kprintf("one-shot timer did not count (measured %llu Hz)\n", (unsigned long long)g_lapic_timer_frequency);
		g_lapic_timer_frequency = 0ULL;
		return false;
	}

	uint32_t khz = (uint32_t)(g_lapic_timer_frequency / 1000ULL % 1000ULL);

	kprintf("one-shot timer counts at %llu.%c%c%c MHz (bus clock / 16, median of %u countdowns)\n",
			(unsigned long long)(g_lapic_timer_frequency / 1000000ULL), (char)('0' + khz / 100U),
			(char)('0' + khz / 10U % 10U), (char)('0' + khz % 10U), (uint32_t)LAPIC_TIMER_CALIBRATION_ROUNDS);
	return true;
}

typedef enum { LAPIC_TIMER_REQUEST_AUTO, LAPIC_TIMER_REQUEST_DEADLINE, LAPIC_TIMER_REQUEST_ONESHOT, LAPIC_TIMER_REQUEST_PIT } lapic_timer_request_t;

static lapic_timer_request_t
lapic_timer_requested(void)
{
	char value[16];

	if (!i386_boot_arg("timer.mode", value, sizeof(value)) || strcmp(value, "auto") == 0)
		return LAPIC_TIMER_REQUEST_AUTO;

	if (strcmp(value, "deadline") == 0) return LAPIC_TIMER_REQUEST_DEADLINE;
	if (strcmp(value, "oneshot") == 0) return LAPIC_TIMER_REQUEST_ONESHOT;
	if (strcmp(value, "pit") == 0) return LAPIC_TIMER_REQUEST_PIT;

	kprintf("unknown timer.mode=%s (auto, deadline, oneshot or pit): choosing automatically\n", value);
	return LAPIC_TIMER_REQUEST_AUTO;
}

static void
lapic_timer_report_tsc(void)
{
	uint32_t registers[4];

	cpu_cpuid(0x80000000U, registers);

	bool invariant = false;

	if (registers[0] >= 0x80000007U) {
		cpu_cpuid(0x80000007U, registers);
		invariant = (registers[3] & CPUID_80000007_EDX_INVARIANT_TSC) != 0U;
	}

	if (invariant) {
		kprintf("TSC is invariant: deadlines hold across power states\n");
	} else {
		kprintf("TSC is not reported invariant: deadlines assume it keeps a constant rate\n");
	}
}

bool
lapic_timer_init(void)
{
	kprintf("initializing per-CPU timers\n");

	lapic_timer_request_t request = lapic_timer_requested();

	if (request == LAPIC_TIMER_REQUEST_PIT) {
		kprintf("timer.mode=pit: keeping the PIT\n");
		return false;
	}

	if (!clock_is_ready()) {
		kprintf("no monotonic clock to program deadlines on: keeping the PIT\n");
		return false;
	}

	if (!apic_map_local()) {
		kprintf("no Local APIC: keeping the PIT\n");
		return false;
	}

	lapic_timer_report_tsc();

	uint32_t registers[4];

	cpu_cpuid(1U, registers);

	bool has_deadline = (registers[2] & CPUID_1_ECX_TSC_DEADLINE) != 0U;
	lapic_timer_mode_t mode = LAPIC_TIMER_NONE;

	if (request != LAPIC_TIMER_REQUEST_ONESHOT) {
		if (!has_deadline) {
			kprintf("this CPU has no TSC-deadline mode\n");
		} else if (lapic_timer_probe(LAPIC_TIMER_TSC_DEADLINE)) {
			mode = LAPIC_TIMER_TSC_DEADLINE;
		}
	}

	if (mode == LAPIC_TIMER_NONE && request != LAPIC_TIMER_REQUEST_DEADLINE) {
		if (lapic_timer_calibrate() && lapic_timer_probe(LAPIC_TIMER_ONESHOT)) mode = LAPIC_TIMER_ONESHOT;
	}

	if (mode == LAPIC_TIMER_NONE) {
		kprintf("no usable Local APIC timer mode: keeping the PIT\n");
		return false;
	}

	g_lapic_timer_mode = mode;
	kprintf("%s mode selected\n", lapic_timer_mode_name());

	/*
	 * The hand-over, with interrupts masked so no tick lands half way: the
	 * PIT falls silent, the event timer goes to deadline mode, and the boot
	 * CPU's own timer takes over. Timer calls entered on the PIT's tick are
	 * on the boot CPU's queue already, which is where its timer looks.
	 */
	uint64_t irq_state = ml_irq_save();

	pic_mask(I386_TIMER_IRQ);
	etimer_init(mode == LAPIC_TIMER_TSC_DEADLINE ? &g_lapic_timer_deadline_hw : &g_lapic_timer_oneshot_hw);
	lapic_timer_cpu_start();

	ml_irq_restore(irq_state);

	kprintf("PIT stopped: the Local APIC timers drive every CPU from here\n");
	return true;
}

void
lapic_timer_cpu_start(void)
{
	if (g_lapic_timer_mode == LAPIC_TIMER_NONE) return;

	uint64_t irq_state = ml_irq_save();

	lapic_timer_setup_local(g_lapic_timer_mode);
	(void)__atomic_add_fetch(&g_lapic_timer_cpus_ready, 1U, __ATOMIC_ACQ_REL);
	etimer_cpu_start();

	ml_irq_restore(irq_state);

	kprintf("CPU %u timer ready\n", machine_cpu_id());
}
