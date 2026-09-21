#ifndef NXU_GIC_H
#define NXU_GIC_H

#include <stdbool.h>
#include <stdint.h>

/**
 * gic_enter_higher_half - Switch GIC MMIO accesses to TTBR1 aliases.
 *
 * Return: true on the first successful switch, otherwise false.
 */
bool gic_enter_higher_half(void);
bool gic_higher_half_enabled(void);

/**
 * gic_init - Bring up the GICv3 distributor, redistributor and CPU interface.
 *
 * Configures the distributor for affinity routing and Group 1 interrupts,
 * wakes redistributor 0, and enables the ICC_* system-register CPU interface
 * with EOImode 0.
 *
 * The GIC MMIO frames must already be mapped as Device memory, so call this
 * after vmm_init(). Interrupts should remain masked at the CPU until every
 * source has also been configured; see arm64_enable_irqs().
 *
 * Assumes a single core and redistributor 0. The MMIO base addresses are
 * currently compile-time constants rather than the values recovered by
 * platform_discover().
 *
 * Cannot report failure. The distributor RWP poll and the redistributor
 * wake poll have no timeout and will spin indefinitely against unresponsive
 * hardware.
 *
 * Must be called exactly once, and not from interrupt context.
 */
void gic_init(void);

/**
 * gic_init_secondary - Bring up the calling CPU's GICv3 interface.
 * @cpu: The caller's logical CPU id (1 or more).
 * @mpidr: The caller's MPIDR affinity fields.
 *
 * Finds the CPU's redistributor frame by matching GICR_TYPER's affinity,
 * wakes it, and enables the ICC_* system-register CPU interface. Call it on
 * the CPU being brought up, after machine_cpu_local_set(). Everything shared
 * (the distributor, SPI routing) was configured once by gic_init().
 *
 * Return: false if the CPU has no redistributor (it cannot take interrupts).
 */
bool gic_init_secondary(uint32_t cpu, uint64_t mpidr);

/**
 * gic_enable_sgi - Enable one software-generated interrupt on this CPU.
 * @intid: SGI number, 0 through 15.
 * @priority: Priority byte; numerically lower means higher priority.
 *
 * Every CPU must enable the SGIs it is to receive in its own redistributor.
 */
void gic_enable_sgi(uint32_t intid, uint8_t priority);

/**
 * gic_send_sgi - Raise SGI @intid on the CPU with affinity @target_mpidr.
 * gic_send_sgi_others - Raise it on every CPU but the caller.
 *
 * The write is preceded by a DSB so anything the sender stored for the
 * receiver is visible before the interrupt is.
 */
void gic_send_sgi(uint64_t target_mpidr, uint32_t intid);
void gic_send_sgi_others(uint32_t intid);

/**
 * gic_enable_ppi - Configure and enable one private peripheral interrupt.
 * @intid: Interrupt ID. Must be in the range 16 through 31.
 * @priority: Priority value; numerically lower means higher priority.
 *
 * Places the PPI in Non-secure Group 1, sets its priority, configures it as
 * level-sensitive, clears any stale pending state and enables forwarding.
 *
 * Level-sensitive is required for the generic timer, whose signal stays
 * asserted until the timer is rearmed.
 *
 * Every register touched belongs to redistributor 0, so this affects only
 * the boot core. gic_init() must have completed first.
 *
 * Silently does nothing if @intid is outside 16 through 31. Performs
 * read-modify-write on shared registers, so it must not be called from
 * interrupt context.
 */
void gic_enable_ppi(uint32_t intid, uint8_t priority);

/**
 * gic_enable_spi - Configure and enable one shared peripheral interrupt.
 * @intid: Architectural GIC INTID in the SPI range 32 through 1019.
 * @priority: Priority byte; numerically smaller values are higher priority.
 * @edge_triggered: true for edge-triggered delivery, false for level.
 *
 * Routes the SPI to CPU affinity 0, places it in Non-secure Group 1, clears
 * stale pending state and enables forwarding through the distributor.
 *
 * Return: true when the INTID was valid and configured.
 */
bool gic_enable_spi(
	uint32_t intid,
	uint8_t priority,
	bool edge_triggered
);

/**
 * gic_acknowledge_interrupt - Acknowledge the highest-priority pending IRQ.
 *
 * Reads ICC_IAR1_EL1. This is not a passive query: it atomically
 * acknowledges the interrupt and moves it to the active state.
 *
 * Interrupt-context function. Every acknowledged INTID below 1020 must be
 * completed with gic_end_interrupt(), or the running priority stays elevated
 * and blocks further interrupts.
 *
 * Return: the 24-bit INTID. Values 1020 through 1023 are special or spurious
 * and must not be passed to gic_end_interrupt().
 */
uint32_t gic_acknowledge_interrupt(void);

/**
 * gic_end_interrupt - Signal completion of an acknowledged interrupt.
 * @intid: The INTID previously returned by gic_acknowledge_interrupt().
 *
 * Writes ICC_EOIR1_EL1. With EOImode 0 this performs both the priority drop
 * and the deactivation.
 *
 * Interrupt-context function. Must be called with the exact INTID that was
 * acknowledged, and must not be called for spurious INTIDs.
 */
void gic_end_interrupt(uint32_t intid);

/**
 * arm64_enable_irqs - Unmask IRQs at the CPU by clearing PSTATE.I.
 *
 * This is a PSTATE operation and touches no GIC register; it is the CPU-side
 * complement of the GIC's own masking. An interrupt is taken only when both
 * layers are open.
 *
 * Call this last during initialization, after the vector table, the GIC and
 * every interrupt source have been configured. Until it runs, control flow is
 * strictly sequential, which is what lets the rest of the kernel operate
 * without locking.
 *
 * PSTATE.D, PSTATE.A and PSTATE.F remain set.
 */
void arm64_enable_irqs(void);

/**
 * arm64_disable_irqs - Mask IRQs at the CPU by setting PSTATE.I.
 *
 * Does not save the previous state, so it cannot be nested.
 */
void arm64_disable_irqs(void);

#endif
