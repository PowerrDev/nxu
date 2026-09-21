/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/arm64/smp.c
 *
 * Secondary CPU bring-up: PSCI CPU_ON, the parameter block the trampoline
 * (smp_entry.S) reads, and the C entry each new CPU runs.
 *
 * Sequence, per secondary CPU, on the boot CPU:
 *
 *   1. give it a logical id and a processor object (its MPIDR stays the
 *      identity firmware and the GIC know it by; the two are never assumed
 *      equal),
 *   2. allocate its kernel stack,
 *   3. fill its parameter block from the boot CPU's own live MMU
 *      configuration and clean it to the point of coherency (the CPU reads it
 *      with its MMU and caches off),
 *   4. PSCI CPU_ON with the trampoline's physical address,
 *   5. wait for the CPU to report ONLINE.
 *
 * and on the new CPU (smp_secondary_main): install the per-CPU pointer, drop
 * the temporary TTBR0 identity map, bring up its GIC redistributor and CPU
 * interface and its timer, report ONLINE, and idle in WFI.
 */

#include <kern/arm64/gic.h>
#include <kern/arm64/smp.h>
#include <kern/arm64/timer.h>
#include <kern/console/console.h>
#include <kern/cpuset.h>
#include <kern/ipi.h>
#include <kern/machine/cache.h>
#include <kern/machine/cpu.h>
#include <kern/machine/machine_routines.h>
#include <kern/sched_prism/processor.h>
#include <kern/sched_prism/sched.h>
#include <platform/platform.h>
#include <vm/vm_kern.h>
#include <vm/vmm.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SMP_STACK_SIZE 16384U

/* How long the boot CPU waits for one secondary before giving up on it. */
#define SMP_ONLINE_TIMEOUT_US 2000000ULL

/* PSCI (ARM DEN 0022): the SMC64 function id of CPU_ON, and its status codes. */
#define PSCI_CPU_ON_64 0xC4000003ULL
#define PSCI_SUCCESS 0
#define PSCI_NOT_SUPPORTED (-1)
#define PSCI_INVALID_PARAMETERS (-2)
#define PSCI_DENIED (-3)
#define PSCI_ALREADY_ON (-4)
#define PSCI_ON_PENDING (-5)
#define PSCI_INTERNAL_FAILURE (-6)

#define TCR_EL1_EPD0 (1ULL << 7U)

/*
 * What smp_entry.S reads. Offsets are fixed by the ARM64_SMP_* constants
 * there, and each block starts on its own cache line so cleaning one CPU's
 * block never touches another's.
 */
typedef struct {
	uint64_t mair;
	uint64_t tcr;
	uint64_t ttbr0;
	uint64_t ttbr1;
	uint64_t sctlr;
	uint64_t stack_top;
	uint64_t entry;
	uint64_t vbar;
	uint64_t cpu;
	uint64_t reserved[7];
} __attribute__((aligned(64))) smp_boot_params_t;

_Static_assert(offsetof(smp_boot_params_t, mair) == 0, "smp_entry.S: ARM64_SMP_MAIR");
_Static_assert(offsetof(smp_boot_params_t, tcr) == 8, "smp_entry.S: ARM64_SMP_TCR");
_Static_assert(offsetof(smp_boot_params_t, ttbr0) == 16, "smp_entry.S: ARM64_SMP_TTBR0");
_Static_assert(offsetof(smp_boot_params_t, ttbr1) == 24, "smp_entry.S: ARM64_SMP_TTBR1");
_Static_assert(offsetof(smp_boot_params_t, sctlr) == 32, "smp_entry.S: ARM64_SMP_SCTLR");
_Static_assert(offsetof(smp_boot_params_t, stack_top) == 40, "smp_entry.S: ARM64_SMP_STACK");
_Static_assert(offsetof(smp_boot_params_t, entry) == 48, "smp_entry.S: ARM64_SMP_ENTRY");
_Static_assert(offsetof(smp_boot_params_t, vbar) == 56, "smp_entry.S: ARM64_SMP_VBAR");
_Static_assert(offsetof(smp_boot_params_t, cpu) == 64, "smp_entry.S: ARM64_SMP_CPU");

extern void arm64_secondary_entry(void);

static smp_boot_params_t g_boot_params[NXU_MAX_CPUS];

static uint32_t g_cpu_total = 1U;
static uint32_t g_cpu_online = 1U;

/* Logical id -> MPIDR affinity. Entry 0 is the boot CPU. */
static uint64_t g_cpu_mpidr[NXU_MAX_CPUS];

uint32_t smp_cpu_count(void)
{
	return g_cpu_total;
}

uint32_t smp_online_count(void)
{
	return __atomic_load_n(&g_cpu_online, __ATOMIC_ACQUIRE);
}

uint64_t smp_cpu_mpidr(uint32_t cpu)
{
	return cpu < g_cpu_total ? g_cpu_mpidr[cpu] : 0ULL;
}

/* ---- PSCI --------------------------------------------------------------- */

/*
 * One PSCI call. The SMC calling convention lets the callee use x0-x3 for
 * results and clobber x4-x17, so all of them are declared as such.
 */
static int64_t smp_psci_call(platform_psci_method_t method, uint64_t function, uint64_t arg0, uint64_t arg1, uint64_t arg2)
{
	register uint64_t x0 __asm__("x0") = function;
	register uint64_t x1 __asm__("x1") = arg0;
	register uint64_t x2 __asm__("x2") = arg1;
	register uint64_t x3 __asm__("x3") = arg2;

	if (method == PLATFORM_PSCI_HVC) {
		__asm__ volatile(
			"hvc #0"
			: "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			:
			: "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory"
		);
	} else {
		__asm__ volatile(
			"smc #0"
			: "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			:
			: "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory"
		);
	}

	return (int64_t)x0;
}

static const char *smp_psci_status_name(int64_t status)
{
	switch (status) {
	case PSCI_SUCCESS: return "success";
	case PSCI_NOT_SUPPORTED: return "not supported";
	case PSCI_INVALID_PARAMETERS: return "invalid parameters";
	case PSCI_DENIED: return "denied";
	case PSCI_ALREADY_ON: return "already on";
	case PSCI_ON_PENDING: return "on pending";
	case PSCI_INTERNAL_FAILURE: return "internal failure";
	default: return "unknown status";
	}
}

/* ---- cache and system register helpers ---------------------------------- */

/*
 * Clean and invalidate [address, address + size) to the point of coherency.
 * The new CPU reads its parameter block with its MMU and caches off, i.e.
 * straight from memory, so the boot CPU's cached copy must reach memory first.
 */
static void smp_clean_to_poc(const void *address, size_t size)
{
	uint64_t line = cache_data_line_size();

	if (line == 0ULL) line = 64ULL;

	uint64_t cursor = (uint64_t)address & ~(line - 1ULL);
	uint64_t end = (uint64_t)address + size;

	for (; cursor < end; cursor += line) {
		__asm__ volatile("dc civac, %0" : : "r"(cursor) : "memory");
	}

	__asm__ volatile("dsb sy" : : : "memory");
}

static uint64_t smp_read_mair(void)
{
	uint64_t value;

	__asm__ volatile("mrs %0, MAIR_EL1" : "=r"(value));
	return value;
}

static uint64_t smp_read_tcr(void)
{
	uint64_t value;

	__asm__ volatile("mrs %0, TCR_EL1" : "=r"(value));
	return value;
}

static uint64_t smp_read_ttbr1(void)
{
	uint64_t value;

	__asm__ volatile("mrs %0, TTBR1_EL1" : "=r"(value));
	return value;
}

static uint64_t smp_read_sctlr(void)
{
	uint64_t value;

	__asm__ volatile("mrs %0, SCTLR_EL1" : "=r"(value));
	return value;
}

static uint64_t smp_read_vbar(void)
{
	uint64_t value;

	__asm__ volatile("mrs %0, VBAR_EL1" : "=r"(value));
	return value;
}

/*
 * The boot CPU stops using TTBR0 once the kernel runs from TTBR1
 * (vmm_disable_ttbr0). A secondary borrows TTBR0 only for the trampoline and
 * lets it go the same way. This is the per-CPU register work only: it must not
 * touch the boot CPU's bookkeeping in g_vmm, which describes CPU 0.
 *
 * The TLB invalidate is local (non-shareable): the entries being dropped are
 * this CPU's own, made while it ran the trampoline.
 */
static void smp_disable_ttbr0_local(void)
{
	uint64_t tcr = smp_read_tcr() | TCR_EL1_EPD0;

	__asm__ volatile(
		"msr TTBR0_EL1, xzr\n"
		"msr TCR_EL1, %0\n"
		"isb\n"
		"tlbi vmalle1\n"
		"dsb nsh\n"
		"isb"
		:
		: "r"(tcr)
		: "memory"
	);
}

/* ---- the new CPU -------------------------------------------------------- */

/*
 * The first C code a secondary CPU runs: higher-half address, its own stack,
 * MMU on, TTBR0 still the trampoline's identity map, interrupts masked.
 */
__attribute__((noreturn))
void smp_secondary_main(processor_t cpu)
{
	/* Before anything asks "which CPU am I?": logging, the GIC and the timer all do. */
	machine_cpu_local_set(cpu);

	smp_disable_ttbr0_local();

	if (!gic_init_secondary(cpu->cpu_id, cpu->mpidr)) {
		kprintf("cpu%u: no GIC redistributor for MPIDR 0x%llx, not starting\n", cpu->cpu_id, (unsigned long long)cpu->mpidr);
		__atomic_store_n(&cpu->state, PROCESSOR_SHUTDOWN, __ATOMIC_RELEASE);

		for (;;) cpu_wait_for_interrupt();
	}

	/* The physical timer's PPI and the IPI SGIs are per CPU: each CPU enables its own and arms its own timer. */
	gic_enable_ppi(PHYSICAL_TIMER_INTID, 0x80U);
	smp_enable_local_ipis();
	timer_start_local();

	kprintf("cpu%u: online\n", cpu->cpu_id);

	__atomic_add_fetch(&g_cpu_online, 1U, __ATOMIC_ACQ_REL);

	/*
	 * From here the CPU is the scheduler's: sched_cpu_idle() publishes it as
	 * schedulable (a release store the boot CPU's acquire load of `state`
	 * pairs with, so everything above is visible first) and idles. The
	 * "online" line is printed before that because the boot CPU prints as
	 * soon as it sees the CPU online and the console has one writer at a time.
	 */
	sched_cpu_idle();
}

/* ---- inter-processor interrupts ----------------------------------------- */

/*
 * SGIs 0-15 are the IPI vectors (kern/ipi.h names the ones in use). Each CPU
 * enables them in its own redistributor: an SGI is delivered to the target
 * CPU's redistributor, so a CPU that never enabled it never takes it.
 */
void smp_enable_local_ipis(void)
{
	for (uint32_t vector = 0U; vector < IPI_TYPE_COUNT; vector++) gic_enable_sgi(vector, 0x80U);
}

void machine_ipi_raise(uint32_t cpu, uint32_t vector)
{
	if (cpu >= g_cpu_total || cpu == machine_cpu_id()) return;

	gic_send_sgi(g_cpu_mpidr[cpu], vector);
}

void machine_ipi_raise_others(uint32_t vector)
{
	if (g_cpu_total <= 1U) return;

	gic_send_sgi_others(vector);
}

void smp_handle_ipi(uint32_t vector)
{
	processor_t self = current_processor();

	switch (vector) {
	case IPI_RESCHEDULE:
		/*
		 * Nothing to do but note it: the interrupt itself woke the CPU, and
		 * the flag makes the return path (exception_dispatch) enter the
		 * scheduler. The sender queued the thread under this CPU's runq_lock
		 * before it raised the SGI, so the queue is already visible.
		 */
		self->ipi_reschedule_count++;
		__atomic_store_n(&self->preemption_pending, true, __ATOMIC_RELEASE);
		break;

	case IPI_CPU_STOP:
		/* Never returns: this CPU takes no further interrupts and stops touching memory. */
		ml_irq_disable();
		__atomic_store_n(&self->state, PROCESSOR_SHUTDOWN, __ATOMIC_RELEASE);

		for (;;) cpu_wait_for_event();

	default:
		break;
	}
}

void smp_stop_other_cpus(void)
{
	if (g_cpu_total <= 1U) return;

	ipi_send_others(IPI_CPU_STOP);

	/* Bounded: a CPU with interrupts masked for ever cannot be stopped, and the panic must go on. */
	uint64_t deadline = timer_get_microseconds() + 20000ULL;

	while (timer_get_microseconds() < deadline) cpu_relax();
}

/* ---- the boot CPU ------------------------------------------------------- */

/*
 * Start one secondary CPU. Returns whether it reported ONLINE.
 */
static bool smp_start_cpu(
	uint32_t cpu_id,
	uint64_t mpidr,
	platform_psci_method_t method,
	uint64_t entry_physical,
	uint64_t trampoline_root
)
{
	processor_t cpu = processor_register(cpu_id, mpidr);

	if (cpu == 0) {
		kprintf("SMP: cannot register CPU%u\n", cpu_id);
		return false;
	}

	if (!sched_cpu_prepare(cpu_id)) {
		kprintf("SMP: cannot create the CPU%u idle thread\n", cpu_id);
		return false;
	}

	void *stack;

	if (!vm_kern_allocate(SMP_STACK_SIZE, VMM_PROTECTION_READ_WRITE, &stack)) {
		kprintf("SMP: no memory for the CPU%u boot stack\n", cpu_id);
		return false;
	}

	cpu->boot_stack = stack;
	__atomic_store_n(&cpu->state, PROCESSOR_STARTING, __ATOMIC_RELEASE);

	/*
	 * The boot CPU's own translation regime, one register at a time, so every
	 * CPU runs the same one. TCR has EPD0 set here (TTBR0 is disabled after
	 * boot); the trampoline needs TTBR0 for its identity map, so it is clear
	 * in the copy the new CPU starts with.
	 */
	smp_boot_params_t *params = &g_boot_params[cpu_id];

	params->mair = smp_read_mair();
	params->tcr = smp_read_tcr() & ~TCR_EL1_EPD0;
	params->ttbr0 = trampoline_root;
	params->ttbr1 = smp_read_ttbr1();
	params->sctlr = smp_read_sctlr();
	params->stack_top = (uint64_t)stack + SMP_STACK_SIZE;
	params->entry = (uint64_t)&smp_secondary_main;
	params->vbar = smp_read_vbar();
	params->cpu = (uint64_t)cpu;

	smp_clean_to_poc(params, sizeof(*params));

	uint64_t params_physical;

	if (!vmm_kernel_address_to_physical((uint64_t)params, &params_physical)) {
		kprintf("SMP: CPU%u boot parameters have no physical address\n", cpu_id);
		return false;
	}

	kprintf("SMP: starting CPU%u MPIDR 0x%llx\n", cpu_id, (unsigned long long)mpidr);

	int64_t status = smp_psci_call(method, PSCI_CPU_ON_64, mpidr, entry_physical, params_physical);

	if (status != PSCI_SUCCESS) {
		kprintf("SMP: PSCI CPU_ON for CPU%u failed: %s (%d)\n", cpu_id, smp_psci_status_name(status), (int)status);
		__atomic_store_n(&cpu->state, PROCESSOR_OFFLINE, __ATOMIC_RELEASE);
		return false;
	}

	uint64_t deadline = timer_get_microseconds() + SMP_ONLINE_TIMEOUT_US;

	/* Online is IDLE, or already RUNNING if the scheduler gave it work first. */
	while (!processor_is_online(cpu)) {
		if (__atomic_load_n(&cpu->state, __ATOMIC_ACQUIRE) == PROCESSOR_SHUTDOWN || timer_get_microseconds() > deadline) {
			kprintf("SMP: CPU%u did not come online\n", cpu_id);
			return false;
		}

		cpu_relax();
	}

	return true;
}

uint32_t smp_boot_secondaries(void)
{
	const platform_t *platform = platform_get();

	if (platform == 0 || platform->cpu_count <= 1U) return 1U;

	uint32_t possible = platform->cpu_count < NXU_MAX_CPUS ? platform->cpu_count : NXU_MAX_CPUS;
	uint64_t boot_mpidr = machine_cpu_mpidr();

	/* Logical CPU 0 is whichever CPU is running this, wherever it sits in the Device Tree. */
	g_cpu_mpidr[0] = boot_mpidr;
	g_cpu_total = 1U;

	uint32_t boot_index = platform->cpu_count;

	for (uint32_t index = 0U; index < platform->cpu_count; index++) {
		if (platform->cpu_mpidr[index] == boot_mpidr) boot_index = index;
	}

	if (boot_index == platform->cpu_count) {
		kprintf("SMP: boot CPU MPIDR 0x%llx is not in the Device Tree CPU list\n", (unsigned long long)boot_mpidr);
		return 1U;
	}

	for (uint32_t index = 0U; index < platform->cpu_count && g_cpu_total < possible; index++) {
		if (index != boot_index) g_cpu_mpidr[g_cpu_total++] = platform->cpu_mpidr[index];
	}

	kprintf("SMP: boot CPU0 MPIDR 0x%llx, %u CPU(s) in the Device Tree\n", (unsigned long long)boot_mpidr, platform->cpu_count);

	if (platform->psci_method == PLATFORM_PSCI_NONE) {
		kputln("SMP: firmware offers no PSCI: secondary CPUs stay off");
		g_cpu_total = 1U;
		return 1U;
	}

	/*
	 * The trampoline's physical pages. It sits in .text.boot at the start of
	 * the image, so mapping the pages that hold it (two, so a trampoline that
	 * straddles a boundary is still covered) is enough.
	 */
	uint64_t entry_physical;

	if (!vmm_kernel_address_to_physical((uint64_t)&arm64_secondary_entry, &entry_physical)) {
		kputln("SMP: the secondary entry point has no physical address");
		g_cpu_total = 1U;
		return 1U;
	}

	uint64_t trampoline_root;

	if (!vmm_create_identity_stub(entry_physical & ~0xFFFULL, 2U * 4096U, &trampoline_root)) {
		kputln("SMP: cannot build the trampoline identity map");
		g_cpu_total = 1U;
		return 1U;
	}

	/* The boot CPU takes IPIs too (a secondary queueing work on it, a panic elsewhere). */
	smp_enable_local_ipis();

	for (uint32_t cpu = 1U; cpu < g_cpu_total; cpu++) {
		(void)smp_start_cpu(cpu, g_cpu_mpidr[cpu], platform->psci_method, entry_physical, trampoline_root);
	}

	uint32_t online = smp_online_count();

	if (online == g_cpu_total) {
		kprintf("SMP: %u CPUs online\n", online);
	} else {
		kprintf("SMP: %u of %u CPUs online\n", online, g_cpu_total);
	}

	if (online > 1U) kprintf("sched: SMP scheduling enabled, %u run queues\n", online);
	return online;
}
