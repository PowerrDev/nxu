/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/i386/smp.c
 *
 * Secondary CPU bring-up: the MP table scan, the real-mode trampoline copy
 * and the parameter block it reads (smp_trampoline.S), and the C entry each
 * new CPU runs.
 *
 * Called after the "threads" phase, not the earlier "vm" phase arm64's
 * equivalent runs from: a secondary needs a real kernel stack
 * (vm_kern_allocate), a processor_t and idle thread (processor_register,
 * sched_cpu_prepare) and somewhere to actually go once it reports online
 * (sched_cpu_idle) -- all threads-area machinery that does not exist before
 * that phase runs, unlike arm64 where PSCI CPU_ON's parameter block is
 * self-contained enough to prepare during the vm phase and only *use* once
 * threads are up.
 *
 * Sequence, per secondary CPU, on the boot CPU (smp_start_cpu):
 *
 *   1. give it a logical id and a processor object (its Local APIC id stays
 *      the identity the MP table and the APIC know it by; the two are never
 *      assumed equal to the logical id),
 *   2. allocate its kernel stack,
 *   3. fill its parameter block (GDTR for the boot CPU's own GDT -- every
 *      CPU starts through that one, real per-CPU GDTs come later, from
 *      i386_gdt_init_secondary -- CR3, stack top, cpu_id, processor_t),
 *   4. apic_start_cpu(): INIT-SIPI-SIPI,
 *   5. wait for the CPU to report ONLINE.
 *
 * and on the new CPU (i386_smp_secondary_main, called by the trampoline's
 * tail once paging and the higher half are live): install this CPU's own
 * GDT/TSS/percpu cell and IDT, enable its Local APIC, report ONLINE, and
 * join the scheduler's idle loop.
 *
 * PDE[0] (the master kernel page directory's temporary identity map of
 * physical [0, 4 MiB), see start.S and pmap.c's pmap_init) is restored for
 * exactly the span of this whole function and cleared again once every
 * secondary this call is starting has either come online or timed out: the
 * trampoline's real-mode and early-protected-mode code addresses SMP_
 * TRAMPOLINE_PHYS/PARAMS physically the whole way through, including after
 * paging is enabled but before the jump to the higher half.
 */

#include <kern/i386/apic.h>
#include <kern/i386/boot_info.h>
#include <kern/i386/gdt.h>
#include <kern/i386/idt.h>
#include <kern/i386/mp_table.h>
#include <kern/i386/pmap.h>
#include <kern/i386/smp.h>
#include <kern/i386/timer.h>
#include <kern/i386/trap.h>
#include <kern/i386/vm_param.h>

#include <kern/console/console.h>
#include <kern/cpuset.h>
#include <kern/ipi.h>
#include <kern/machine/cpu.h>
#include <kern/sched_prism/processor.h>
#include <kern/sched_prism/sched.h>
#include <vm/vm_kern.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SMP_STACK_SIZE 16384U

/* How long the boot CPU waits for one secondary before giving up on it. */
#define SMP_ONLINE_TIMEOUT_US 2000000ULL

#define SMP_TRAMPOLINE_PHYS 0x8000U
#define SMP_TRAMPOLINE_PARAMS 0x8800U

/* What smp_trampoline.S reads, at a fixed physical address; see its own file comment. Packed, no padding: the .S file's PARAMS_* offsets assume exactly this layout. */
typedef struct __attribute__((packed)) {
	uint16_t gdt_limit;
	uint32_t gdt_base;
	uint32_t cr3;
	uint32_t stack_top;
	uint32_t cpu_id;
	uint32_t processor_ptr;
} smp_trampoline_params_t;

_Static_assert(sizeof(smp_trampoline_params_t) == 22U, "smp_trampoline.S: PARAMS_* offsets");
_Static_assert(offsetof(smp_trampoline_params_t, gdt_limit) == 0U, "smp_trampoline.S: PARAMS_GDT_LIMIT");
_Static_assert(offsetof(smp_trampoline_params_t, cr3) == 6U, "smp_trampoline.S: PARAMS_CR3");
_Static_assert(offsetof(smp_trampoline_params_t, stack_top) == 10U, "smp_trampoline.S: PARAMS_STACK_TOP");
_Static_assert(offsetof(smp_trampoline_params_t, cpu_id) == 14U, "smp_trampoline.S: PARAMS_CPU_ID");
_Static_assert(offsetof(smp_trampoline_params_t, processor_ptr) == 18U, "smp_trampoline.S: PARAMS_PROCESSOR_PTR");

extern uint8_t i386_smp_trampoline_start[];
extern uint8_t i386_smp_trampoline_end[];

static uint32_t g_cpu_total = 1U;
static uint32_t g_cpu_online = 1U;

/* Logical id -> Local APIC id. Entry 0 is the boot CPU. */
static uint8_t g_cpu_apic_id[NXU_MAX_CPUS];

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
	return cpu < g_cpu_total ? (uint64_t)g_cpu_apic_id[cpu] : 0ULL;
}

/* ---- the new CPU -------------------------------------------------------- */

/*
 * The first C code a secondary CPU runs: higher-half address, its own stack,
 * paging on with the shared kernel directory, interrupts still masked (the
 * trampoline never touched IF, and it starts clear the way every CPU's does
 * on reset).
 */
__attribute__((noreturn))
void i386_smp_secondary_main(uint32_t cpu_id, processor_t cpu)
{
	/*
	 * i386_gdt_init_secondary() first: it is what actually gives this CPU
	 * a live GDT_PERCPU_SEL (its own g_gdt[cpu_id] slot 8, %fs loaded to
	 * point at it) -- before this runs, %fs is still the null selector the
	 * trampoline's 16-bit portion loaded (see smp_trampoline.S), and
	 * machine_cpu_local_set()'s %fs:0 write would be a #GP through a null
	 * segment, not the establishing store its own "before anything asks
	 * which CPU am I" contract assumes.
	 */
	i386_gdt_init_secondary(cpu_id, (uint32_t)(uintptr_t)cpu->boot_stack + SMP_STACK_SIZE);
	machine_cpu_local_set(cpu);
	i386_idt_load_secondary();

	apic_enable_local();

	/*
	 * Every CPU resets with IF clear, and nothing on the path here (the
	 * trampoline, machine_cpu_local_set, i386_gdt_init_secondary,
	 * i386_idt_load_secondary, apic_enable_local) sets it: without this,
	 * this CPU would sit online but permanently unable to take the very
	 * IPIs and timer-tick broadcasts the rest of this file exists to
	 * deliver to it.
	 */
	ml_irq_enable();

	kprintf("cpu%u: online\n", cpu_id);

	__atomic_add_fetch(&g_cpu_online, 1U, __ATOMIC_ACQ_REL);

	/*
	 * From here the CPU is the scheduler's: sched_cpu_idle() publishes it as
	 * schedulable (a release store the boot CPU's acquire load of `state`
	 * pairs with, so everything above is visible first) and idles. The
	 * "online" line is printed before that because the boot CPU prints as
	 * soon as it sees the CPU online and the console has one writer at a
	 * time.
	 */
	sched_cpu_idle();

	for (;;) cpu_wait_for_interrupt();
}

/* ---- inter-processor interrupts ----------------------------------------- */

void machine_ipi_raise(uint32_t cpu, uint32_t vector)
{
	if (cpu >= g_cpu_total || cpu == machine_cpu_id()) return;

	uint8_t raw_vector = vector == IPI_RESCHEDULE ? T_IPI_RESCHEDULE : T_IPI_CPU_STOP;

	apic_send_ipi(g_cpu_apic_id[cpu], raw_vector);
}

void machine_ipi_raise_others(uint32_t vector)
{
	if (g_cpu_total <= 1U) return;

	uint8_t raw_vector = vector == IPI_RESCHEDULE ? T_IPI_RESCHEDULE : T_IPI_CPU_STOP;

	apic_send_ipi_all_but_self(raw_vector);
}

void smp_handle_ipi(uint32_t vector)
{
	processor_t self = current_processor();

	switch (vector) {
	case IPI_RESCHEDULE:
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

void smp_tick_others(void)
{
	if (g_cpu_total <= 1U) return;

	apic_send_ipi_all_but_self(T_IPI_TICK);
}

/*
 * The trap dispatcher's entry for T_IPI_RESCHEDULE / T_IPI_CPU_STOP /
 * T_IPI_TICK / T_LAPIC_SPURIOUS (kern/i386/trap.c). Strong-overrides trap.c's
 * weak default.
 */
void i386_trap_ipi(x86_saved_state_t *state)
{
	uint32_t vector = state->trapno;

	if (vector == T_LAPIC_SPURIOUS) return; /* Architecturally not acknowledged. */

	if (vector == T_IPI_TICK) {
		sched_tick();
	} else {
		smp_handle_ipi(vector == T_IPI_RESCHEDULE ? IPI_RESCHEDULE : IPI_CPU_STOP);
	}

	apic_eoi();
}

void smp_stop_other_cpus(void)
{
	if (g_cpu_total <= 1U) return;

	ipi_send_others(IPI_CPU_STOP);

	/* Bounded: a CPU with interrupts masked for ever cannot be stopped, and the panic must go on. */
	uint64_t deadline = timer_get_microseconds() + 20000ULL;

	while (timer_get_microseconds() < deadline) cpu_relax();

	kconsole_break_lock();
}

/* ---- the boot CPU -------------------------------------------------------- */

extern uint32_t i386_boot_pd[];

/*
 * The boot CPU's own GDTR (its complete GDT, slots 1/2 of which are the same
 * flat kernel code/data descriptors every CPU's own eventual GDT also has):
 * every secondary starts through this one before i386_gdt_init_secondary
 * gives it its own. i386_gdt_init built it; there is no accessor for the raw
 * pointer value today, so this reads it back from the CPU the same way
 * pmap_current_directory_physical() reads CR3 back, with sgdt.
 */
static void smp_read_boot_gdtr(uint16_t *limit, uint32_t *base)
{
	uint8_t raw[6];

	__asm__ volatile("sgdt %0" : "=m"(raw));

	*limit = (uint16_t)(raw[0] | ((uint16_t)raw[1] << 8U));
	*base = (uint32_t)raw[2] | ((uint32_t)raw[3] << 8U) | ((uint32_t)raw[4] << 16U) | ((uint32_t)raw[5] << 24U);
}

/* Start one secondary CPU. Returns whether it reported ONLINE. */
static bool smp_start_cpu(uint32_t cpu_id, uint8_t apic_id_value)
{
	processor_t cpu = processor_register(cpu_id, (uint64_t)apic_id_value);

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

	smp_trampoline_params_t *params = (smp_trampoline_params_t *)pmap_table_pointer(SMP_TRAMPOLINE_PARAMS);

	/* Packed struct members: read into locals first, never take &params->field directly (it may be unaligned). */
	uint16_t gdt_limit;
	uint32_t gdt_base;

	smp_read_boot_gdtr(&gdt_limit, &gdt_base);
	params->gdt_limit = gdt_limit;
	/*
	 * sgdt reads back the boot CPU's own GDTR, whose base is g_gdt[0]'s
	 * virtual (higher-half) address -- correct for the boot CPU, which has
	 * paging on. The secondary reads this same field with paging off, in
	 * real mode: it needs the physical address, or lgdt loads a GDT that
	 * does not exist from its point of view (a #GP on the very next far
	 * jump, since the code descriptor it just "loaded" was read from
	 * whatever unrelated physical memory the virtual address's raw bits
	 * happen to name).
	 */
	params->gdt_base = gdt_base - (uint32_t)VMM_HIGHER_HALF_BASE;
	params->cr3 = pmap_kernel_directory_physical();
	params->stack_top = (uint32_t)(uintptr_t)stack + SMP_STACK_SIZE;
	params->cpu_id = cpu_id;
	params->processor_ptr = (uint32_t)(uintptr_t)cpu;

	kprintf("SMP: starting CPU%u Local APIC id %u\n", cpu_id, apic_id_value);

	apic_start_cpu(apic_id_value, SMP_TRAMPOLINE_PHYS);

	uint64_t deadline = timer_get_microseconds() + SMP_ONLINE_TIMEOUT_US;

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
	mp_table_t table;

	if (!mp_table_scan(&table) || table.cpu_count <= 1U) {
		kputln("SMP: one CPU (no MP table, or it lists only the boot CPU)");
		return 1U;
	}

	if (!apic_init(table.lapic_physical)) {
		kputln("SMP: Local APIC unavailable, staying uniprocessor");
		return 1U;
	}

	uint32_t possible = table.cpu_count < NXU_MAX_CPUS ? table.cpu_count : NXU_MAX_CPUS;
	uint32_t boot_apic_id = apic_id();
	uint32_t boot_index = table.cpu_count;

	for (uint32_t index = 0U; index < table.cpu_count; index++) {
		if (table.cpus[index].apic_id == boot_apic_id) boot_index = index;
	}

	if (boot_index == table.cpu_count) {
		kprintf("SMP: boot CPU Local APIC id %u is not in the MP table\n", boot_apic_id);
		return 1U;
	}

	g_cpu_apic_id[0] = (uint8_t)boot_apic_id;
	g_cpu_total = 1U;

	for (uint32_t index = 0U; index < table.cpu_count && g_cpu_total < possible; index++) {
		if (index != boot_index) g_cpu_apic_id[g_cpu_total++] = table.cpus[index].apic_id;
	}

	kprintf("SMP: boot CPU0 Local APIC id %u, %u CPU(s) in the MP table\n", boot_apic_id, table.cpu_count);

	/*
	 * Copy the trampoline to its fixed physical home and restore PDE[0]'s
	 * temporary identity map of physical [0, 4 MiB) for the whole bring-up
	 * (see this file's own comment): every secondary's trampoline addresses
	 * SMP_TRAMPOLINE_PHYS/PARAMS physically, both before and immediately
	 * after enabling paging, before it reaches the higher half.
	 */
	size_t trampoline_size = (size_t)(i386_smp_trampoline_end - i386_smp_trampoline_start);

	if (trampoline_size > 0x800U) {
		kputln("SMP: trampoline is larger than its reserved page allows, staying uniprocessor");
		g_cpu_total = 1U;
		return 1U;
	}

	memcpy(pmap_table_pointer(SMP_TRAMPOLINE_PHYS), i386_smp_trampoline_start, trampoline_size);

	uint32_t direct_map_pde = (uint32_t)(VMM_HIGHER_HALF_BASE >> 22U);

	i386_boot_pd[0] = i386_boot_pd[direct_map_pde];
	pmap_flush_tlb();

	/* The boot CPU takes IPIs too (a secondary queueing work on it, a panic elsewhere). */
	for (uint32_t cpu = 1U; cpu < g_cpu_total; cpu++) {
		(void)smp_start_cpu(cpu, g_cpu_apic_id[cpu]);
	}

	i386_boot_pd[0] = 0U;
	pmap_flush_tlb();

	uint32_t online = smp_online_count();

	if (online == g_cpu_total) {
		kprintf("SMP: %u CPUs online\n", online);
	} else {
		kprintf("SMP: %u of %u CPUs online\n", online, g_cpu_total);
	}

	if (online > 1U) kprintf("sched: SMP scheduling enabled, %u run queues\n", online);
	return online;
}

/* ---- boot phase ---------------------------------------------------------- */

bool i386_init_smp(const i386_boot_info_t *boot)
{
	(void)boot;

	(void)smp_boot_secondaries();
	return true;
}

/*
 * devices_arg_or-shaped: "expect-cpus=<n>" requires at least n CPUs online
 * (default 0, so a plain boot never fails here); this is the one phase where
 * "fewer than expected" and "the MP table only ever listed one" look the
 * same from outside, which is why the count is what is checked, not whether
 * bring-up itself reported an error.
 */
static bool smp_selftest_expected_cpus(const i386_boot_info_t *boot)
{
	char value[16];

	if (!i386_boot_arg("expect-cpus", value, sizeof(value)) || value[0] == '\0') return true;

	(void)boot;

	uint32_t expected = 0U;

	for (uint32_t index = 0U; value[index] != '\0'; index++) {
		if (value[index] < '0' || value[index] > '9') return true;

		expected = expected * 10U + (uint32_t)(value[index] - '0');
	}

	uint32_t online = smp_online_count();

	kprintf("i386_init_smp_selftest: %u CPU(s) online, expecting at least %u\n", online, expected);

	if (online < expected) {
		kprintf("i386_init_smp_selftest: FAIL only %u of %u expected CPUs online\n", online, expected);
		return false;
	}

	return true;
}

/*
 * A reschedule IPI round trip: every secondary CPU is idling in
 * sched_cpu_idle() by this point, so an IPI_RESCHEDULE to each in turn must
 * set its preemption_pending flag. Proves the whole path (machine_ipi_raise
 * -> apic_send_ipi -> the target's IDT gate -> i386_trap_ipi ->
 * smp_handle_ipi), not just that bring-up reported the CPU online.
 */
static bool smp_selftest_ipi_round_trip(void)
{
	uint32_t online = smp_online_count();

	if (online <= 1U) return true;

	bool ok = true;

	for (uint32_t cpu = 1U; cpu < online; cpu++) {
		processor_t target = processor_by_id(cpu);

		if (target == 0) continue;

		__atomic_store_n(&target->preemption_pending, false, __ATOMIC_RELEASE);
		ipi_send(cpu, IPI_RESCHEDULE);

		/*
		 * Generous on purpose: under TCG (no host hardware acceleration for
		 * this guest architecture, see doc/i386/port.md) a busy host can
		 * stall any one virtual CPU well past a tight deadline without the
		 * delivery itself being broken -- the same tolerance this port's
		 * other timing-sensitive tests already give PIT ticks dropping on a
		 * busy host.
		 */
		uint64_t deadline = timer_get_microseconds() + 500000ULL;
		bool seen = false;

		while (timer_get_microseconds() < deadline) {
			if (__atomic_load_n(&target->preemption_pending, __ATOMIC_ACQUIRE)) {
				seen = true;
				break;
			}

			cpu_relax();
		}

		kprintf("i386_init_smp_selftest: IPI round trip to cpu%u %s\n", cpu, seen ? "ok" : "FAILED");
		ok &= seen;
	}

	return ok;
}

bool i386_init_smp_selftest(const i386_boot_info_t *boot)
{
	bool ok = smp_selftest_expected_cpus(boot);

	ok &= smp_selftest_ipi_round_trip();

	if (ok) kputln("i386_init_smp_selftest: cpu count and IPI round trip ok");

	return ok;
}
