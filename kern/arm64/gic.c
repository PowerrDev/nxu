#include <kern/arm64/gic.h>
#include <kern/cpuset.h>
#include <kern/machine/smp.h>
#include <vm/vmm.h>

#include <stdbool.h>
#include <stdint.h>

/*
 * Temporary QEMU virt platform addresses.
 *
 * TODO:: discover these addresses through the Device Tree
 * instead of hard-coding them.
 */
#define GICD_EARLY_BASE 0x08000000ULL
#define GICR_EARLY_BASE 0x080A0000ULL

/*
 * A GICv3 Redistributor has:
 *
 *   RD_base  = Redistributor control frame
 *   SGI_base = SGI/PPI configuration frame
 */
#define GICR_SGI_OFFSET 0x00010000ULL

/* Distributor registers. */
#define GICD_CTLR         0x0000UL
#define GICD_IGROUPR      0x0080UL
#define GICD_ISENABLER    0x0100UL
#define GICD_ICPENDR      0x0280UL
#define GICD_IPRIORITYR   0x0400UL
#define GICD_ICFGR        0x0C00UL
#define GICD_IROUTER      0x6100UL

/* Redistributor control-frame registers. */
#define GICR_TYPER 0x0008UL
#define GICR_WAKER 0x0014UL

/*
 * GICR_TYPER: bits [63:32] are the owning CPU's affinity (Aff3:Aff2:Aff1:Aff0,
 * one byte each), bit 4 marks the last redistributor of a region, and bit 1
 * (VLPIS) means each redistributor has two more 64 KiB frames (GICv4 layout).
 */
#define GICR_TYPER_VLPIS (1ULL << 1U)
#define GICR_TYPER_LAST  (1ULL << 4U)
#define GICR_FRAME_STRIDE_V3 0x20000ULL
#define GICR_FRAME_STRIDE_V4 0x40000ULL

/* ICC_SGI1R_EL1 fields. */
#define ICC_SGI1R_IRM (1ULL << 40U)

/* Redistributor SGI/PPI-frame registers. */
#define GICR_IGROUPR0     0x0080UL
#define GICR_ISENABLER0   0x0100UL
#define GICR_ICPENDR0     0x0280UL
#define GICR_IPRIORITYR0  0x0400UL
#define GICR_ICFGR1       0x0C04UL

#define GICD_CTLR_ENABLE_GRP1 (1U << 1)
#define GICD_CTLR_ARE_NS      (1U << 5)
#define GICD_CTLR_RWP         (1U << 31)

#define GICR_WAKER_PROCESSOR_SLEEP (1U << 1)
#define GICR_WAKER_CHILDREN_ASLEEP (1U << 2)

static uint64_t g_gicd_base = GICD_EARLY_BASE;
static uint64_t g_gicr_base = GICR_EARLY_BASE;
static bool g_gic_higher_half;

/*
 * Each CPU has its own redistributor frame: the one that is banked per CPU,
 * where PPIs and SGIs (the timer, IPIs) are configured. Logical CPU 0 uses
 * g_gicr_base, which tracks the early/higher-half alias switch; the others are
 * found by matching GICR_TYPER's affinity against their MPIDR and cached here.
 * Each slot is written only by its own CPU, before that CPU takes interrupts.
 */
static uint64_t g_gicr_frames[NXU_MAX_CPUS];

static uint64_t gic_local_redistributor(void)
{
	uint32_t cpu = machine_cpu_id();

	if (cpu == 0U || cpu >= NXU_MAX_CPUS) return g_gicr_base;
	return g_gicr_frames[cpu];
}

static uint64_t gicr_sgi_base(void)
{
	return gic_local_redistributor() + GICR_SGI_OFFSET;
}

bool gic_enter_higher_half(void)
{
	if (g_gic_higher_half || !vmm_higher_half_direct_map_enabled()) {
		return false;
	}

	uint64_t distributor;
	uint64_t redistributor;

	if (!vmm_physical_to_higher_half(
		GICD_EARLY_BASE,
		&distributor
	)) {
		return false;
	}

	if (!vmm_physical_to_higher_half(
		GICR_EARLY_BASE,
		&redistributor
	)) {
		return false;
	}

	g_gicd_base = distributor;
	g_gicr_base = redistributor;
	g_gic_higher_half = true;

	return true;
}

bool gic_higher_half_enabled(void)
{
	return g_gic_higher_half;
}

static inline uint32_t mmio_read32(uint64_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void mmio_write32(uint64_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

static inline void mmio_write8(uint64_t address, uint8_t value)
{
	*(volatile uint8_t *)address = value;
}

static inline void mmio_write64(uint64_t address, uint64_t value)
{
	*(volatile uint64_t *)address = value;
}

static inline void arm64_dsb_sy(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
}

static inline uint64_t gic_read_sre(void)
{
	uint64_t value;

	__asm__ volatile(
		"mrs %0, ICC_SRE_EL1"
		: "=r"(value)
	);

	return value;
}

static inline void gic_write_sre(uint64_t value)
{
	__asm__ volatile(
		"msr ICC_SRE_EL1, %0\n"
		"isb"
		:
		: "r"(value)
		: "memory"
	);
}

static inline uint64_t gic_read_control(void)
{
	uint64_t value;

	__asm__ volatile(
		"mrs %0, ICC_CTLR_EL1"
		: "=r"(value)
	);

	return value;
}

static inline void gic_write_control(uint64_t value)
{
	__asm__ volatile(
		"msr ICC_CTLR_EL1, %0\n"
		"isb"
		:
		: "r"(value)
		: "memory"
	);
}

static void gic_wait_for_distributor(void)
{
	while (
		(mmio_read32(g_gicd_base + GICD_CTLR) & GICD_CTLR_RWP)
		!= 0U
	) {
		__asm__ volatile("yield");
	}
}

static void gic_distributor_init(void)
{
	/*
	 * Disable distribution while changing the routing model.
	 */
	mmio_write32(g_gicd_base + GICD_CTLR, 0U);
	gic_wait_for_distributor();

	/*
	 * Enable GICv3 affinity routing and Group 1 interrupts.
	 */
	mmio_write32(
		g_gicd_base + GICD_CTLR,
		GICD_CTLR_ARE_NS | GICD_CTLR_ENABLE_GRP1
	);

	gic_wait_for_distributor();
}

static void gic_redistributor_init(uint64_t frame)
{
	uint64_t waker_address = frame + GICR_WAKER;
	uint32_t waker = mmio_read32(waker_address);

	/*
	 * Tell the Redistributor that its CPU is awake.
	 */
	waker &= ~GICR_WAKER_PROCESSOR_SLEEP;
	mmio_write32(waker_address, waker);

	arm64_dsb_sy();

	/*
	 * Wait until the Redistributor confirms that its child
	 * interfaces are awake.
	 */
	while (
		(mmio_read32(waker_address) & GICR_WAKER_CHILDREN_ASLEEP)
		!= 0U
	) {
		__asm__ volatile("yield");
	}
}

static void gic_cpu_interface_init(void)
{
	uint64_t sre = gic_read_sre();

	/*
	 * Enable access to the GICv3 ICC_* system-register interface.
	 */
	gic_write_sre(sre | 1UL);

	uint64_t control = gic_read_control();

	/*
	 * EOImode = 0:
	 * writing ICC_EOIR1_EL1 performs both priority drop and
	 * interrupt deactivation.
	 */
	gic_write_control(control & ~(1UL << 1));

	/*
	 * Permit all normal interrupt priorities.
	 */
	__asm__ volatile(
		"msr ICC_PMR_EL1, %0"
		:
		: "r"((uint64_t)0xFFU)
	);

	/*
	 * Do not use priority grouping yet.
	 */
	__asm__ volatile(
		"msr ICC_BPR1_EL1, %0"
		:
		: "r"((uint64_t)0U)
	);

	/*
	 * Enable Group 1 interrupt signaling to this CPU.
	 */
	__asm__ volatile(
		"msr ICC_IGRPEN1_EL1, %0\n"
		"isb"
		:
		: "r"((uint64_t)1U)
		: "memory"
	);
}

void gic_init(void)
{
	gic_distributor_init();
	gic_redistributor_init(g_gicr_base);
	gic_cpu_interface_init();
}

static inline uint64_t mmio_read64(uint64_t address)
{
	return *(volatile uint64_t *)address;
}

/*
 * The affinity value GICR_TYPER[63:32] holds for a CPU whose MPIDR affinity is
 * `mpidr`: Aff3 in the top byte, then Aff2, Aff1, Aff0 (MPIDR keeps Aff3
 * apart from the other three, at bits 39:32).
 */
static uint32_t gic_typer_affinity(uint64_t mpidr)
{
	return (uint32_t)(((mpidr >> 32U) & 0xFFU) << 24U) | (uint32_t)(mpidr & 0xFFFFFFU);
}

/*
 * Find the redistributor frame belonging to the CPU with affinity `mpidr`.
 * The region is an array of frames, each 128 KiB (256 KiB with vLPI support),
 * ended by the one whose TYPER.Last is set. Returns 0 if none matches.
 */
static uint64_t gic_find_redistributor(uint64_t mpidr)
{
	uint32_t wanted = gic_typer_affinity(mpidr);
	uint64_t frame = g_gicr_base;

	for (uint32_t index = 0U; index < 1024U; index++) {
		uint64_t typer = mmio_read64(frame + GICR_TYPER);

		if ((uint32_t)(typer >> 32U) == wanted) return frame;
		if ((typer & GICR_TYPER_LAST) != 0ULL) return 0ULL;

		frame += (typer & GICR_TYPER_VLPIS) != 0ULL ? GICR_FRAME_STRIDE_V4 : GICR_FRAME_STRIDE_V3;
	}

	return 0ULL;
}

bool gic_init_secondary(uint32_t cpu, uint64_t mpidr)
{
	if (cpu == 0U || cpu >= NXU_MAX_CPUS) return false;

	uint64_t frame = gic_find_redistributor(mpidr);

	if (frame == 0ULL) return false;

	g_gicr_frames[cpu] = frame;

	/*
	 * The distributor is shared and was brought up by the boot CPU. What is
	 * per CPU is its redistributor (woken here) and its ICC_* CPU interface;
	 * neither is touched by anything the boot CPU did.
	 */
	gic_redistributor_init(frame);
	gic_cpu_interface_init();
	return true;
}

/*
 * SGIs (interrupt IDs 0-15) are how one CPU interrupts another. Each CPU
 * enables them in its own redistributor; whether a CPU takes one is decided
 * by the sender, not by anything in the distributor. SGIs are always
 * edge-triggered (their ICFGR bits are read-only).
 */
void gic_enable_sgi(uint32_t intid, uint8_t priority)
{
	if (intid > 15U) return;

	uint32_t mask = 1U << intid;
	uint64_t base = gicr_sgi_base();

	mmio_write32(base + GICR_IGROUPR0, mmio_read32(base + GICR_IGROUPR0) | mask);
	mmio_write8(base + GICR_IPRIORITYR0 + intid, priority);
	mmio_write32(base + GICR_ICPENDR0, mask);
	mmio_write32(base + GICR_ISENABLER0, mask);

	arm64_dsb_sy();
}

/*
 * ICC_SGI1R_EL1 names the target by affinity: Aff3, Aff2, Aff1 fields plus a
 * 16-bit target list of Aff0 values within that cluster.
 *
 * The DSB publishes whatever the sender wrote to memory (the state the
 * receiver is being told about) before the interrupt can be observed; the ISB
 * makes the system-register write take effect before later instructions.
 */
static void gic_write_sgi1r(uint64_t value)
{
	__asm__ volatile(
		"dsb ishst\n"
		"msr ICC_SGI1R_EL1, %0\n"
		"isb"
		:
		: "r"(value)
		: "memory"
	);
}

void gic_send_sgi(uint64_t target_mpidr, uint32_t intid)
{
	if (intid > 15U) return;

	uint64_t aff0 = target_mpidr & 0xFFULL;

	/* A target list holds Aff0 values 0-15; a cluster with more CPUs would need a range selector. */
	if (aff0 > 15U) return;

	uint64_t value =
		(((target_mpidr >> 32U) & 0xFFULL) << 48U) |	/* Aff3 */
		(((target_mpidr >> 16U) & 0xFFULL) << 32U) |	/* Aff2 */
		((uint64_t)intid << 24U) |
		(((target_mpidr >> 8U) & 0xFFULL) << 16U) |	/* Aff1 */
		(1ULL << aff0);					/* target list */

	gic_write_sgi1r(value);
}

void gic_send_sgi_others(uint32_t intid)
{
	if (intid > 15U) return;

	/* IRM = 1: every CPU but the sender. */
	gic_write_sgi1r(ICC_SGI1R_IRM | ((uint64_t)intid << 24U));
}

void gic_enable_ppi(uint32_t intid, uint8_t priority)
{
	if (intid < 16U || intid > 31U) {
		return;
	}

	uint32_t mask = 1U << intid;

	/*
	 * Place this PPI into Non-secure Group 1.
	 */
	uint32_t group = mmio_read32(
		gicr_sgi_base() + GICR_IGROUPR0
	);

	mmio_write32(
		gicr_sgi_base() + GICR_IGROUPR0,
		group | mask
	);

	/*
	 * Each interrupt has one priority byte.
	 * Smaller values mean higher priority.
	 */
	mmio_write8(
		gicr_sgi_base() + GICR_IPRIORITYR0 + intid,
		priority
	);

	/*
	 * PPIs 16-31 are configured by GICR_ICFGR1.
	 * Two bits belong to each interrupt.
	 *
	 * 00 = level-sensitive
	 * 10 = edge-triggered
	 *
	 * The physical timer is level-sensitive.
	 */
	uint32_t shift = (intid - 16U) * 2U;
	uint32_t config = mmio_read32(
		gicr_sgi_base() + GICR_ICFGR1
	);

	config &= ~(3U << shift);

	mmio_write32(
		gicr_sgi_base() + GICR_ICFGR1,
		config
	);

	/* Remove any old pending state. */
	mmio_write32(
		gicr_sgi_base() + GICR_ICPENDR0,
		mask
	);

	/* Enable forwarding for the PPI. */
	mmio_write32(
		gicr_sgi_base() + GICR_ISENABLER0,
		mask
	);

	arm64_dsb_sy();
}

uint32_t gic_acknowledge_interrupt(void)
{
	uint64_t value;

	/*
	 * Reading ICC_IAR1_EL1 acknowledges the highest-priority
	 * pending Group 1 interrupt and returns its INTID.
	 */
	__asm__ volatile(
		"mrs %0, ICC_IAR1_EL1"
		: "=r"(value)
	);

	return (uint32_t)(value & 0x00FFFFFFUL);
}

void gic_end_interrupt(uint32_t intid)
{
	/*
	 * Inform the GIC that handling of this INTID is complete.
	 */
	__asm__ volatile(
		"msr ICC_EOIR1_EL1, %0\n"
		"isb"
		:
		: "r"((uint64_t)intid)
		: "memory"
	);
}

bool gic_enable_spi(
	uint32_t intid,
	uint8_t priority,
	bool edge_triggered
)
{
	if (intid < 32U || intid > 1019U) return false;

	uint32_t register_index = intid / 32U;
	uint32_t bit = intid % 32U;
	uint32_t mask = 1U << bit;

	uint64_t group_address = g_gicd_base + GICD_IGROUPR + register_index * 4ULL;
	uint32_t group = mmio_read32(group_address);
	mmio_write32(group_address, group | mask);

	mmio_write8(g_gicd_base + GICD_IPRIORITYR + intid, priority);

	uint32_t config_index = intid / 16U;
	uint32_t config_shift = (intid % 16U) * 2U;
	uint64_t config_address = g_gicd_base + GICD_ICFGR + config_index * 4ULL;
	uint32_t config = mmio_read32(config_address);

	config &= ~(3U << config_shift);
	if (edge_triggered) config |= 2U << config_shift;
	mmio_write32(config_address, config);

	/* Route to the boot processor (Affinity 0). */
	mmio_write64(g_gicd_base + GICD_IROUTER + (uint64_t)intid * 8ULL, 0ULL);

	mmio_write32(
		g_gicd_base + GICD_ICPENDR + register_index * 4ULL,
		mask
	);

	arm64_dsb_sy();

	mmio_write32(
		g_gicd_base + GICD_ISENABLER + register_index * 4ULL,
		mask
	);

	arm64_dsb_sy();
	return true;
}

void arm64_enable_irqs(void)
{
	/*
	 * Clear only PSTATE.I, the IRQ mask.
	 */
	__asm__ volatile(
		"msr DAIFClr, #2\n"
		"isb"
		:
		:
		: "memory"
	);
}

void arm64_disable_irqs(void)
{
	__asm__ volatile(
		"msr DAIFSet, #2\n"
		"isb"
		:
		:
		: "memory"
	);
}
