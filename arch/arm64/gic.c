#include <arch/arm64/gic.h>
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
#define GICR_WAKER 0x0014UL

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

static uint64_t gicr_sgi_base(void)
{
	return g_gicr_base + GICR_SGI_OFFSET;
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

static void gic_redistributor_init(void)
{
	uint64_t waker_address = g_gicr_base + GICR_WAKER;
	uint32_t waker = mmio_read32(waker_address);

	/*
	 * Tell the Redistributor that CPU 0 is awake.
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
	gic_redistributor_init();
	gic_cpu_interface_init();
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
