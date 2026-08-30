#ifndef NXU_KERN_IRQ_H
#define NXU_KERN_IRQ_H

#include <stdbool.h>
#include <stdint.h>

#define IRQ_MAX_INTID 1019U

typedef void (*irq_handler_t)(uint32_t intid, void *context);

/*
 * irq_init:
 *
 * Initialize the architectural interrupt dispatch table.
 *
 * The exception path owns interrupt-controller acknowledge and EOI. This
 * layer owns only INTID-to-handler dispatch and never manipulates GIC state
 * on behalf of a driver.
 *
 * Returns true after successful initialization. Repeated initialization is
 * harmless.
 */
bool irq_init(void);

/*
 * irq_register:
 *
 * Bind one architectural interrupt ID to a driver handler and context.
 *
 * Registration is serialized by masking IRQs on the current processor.
 * The installed routine executes in interrupt context and must not block,
 * sleep, or perform an operation which can wait for the scheduler.
 *
 * Returns true when the slot was free and the binding was installed.
 */
bool irq_register(uint32_t intid, irq_handler_t handler, void *context);

/*
 * irq_unregister:
 *
 * Remove an interrupt binding when the handler and context still match the
 * current owner of the slot.
 *
 * Returns true when the exact binding was removed.
 */
bool irq_unregister(uint32_t intid, irq_handler_t handler, void *context);

/*
 * irq_dispatch:
 *
 * Dispatch one interrupt which has already been acknowledged by exception
 * entry. GIC EOI remains the caller's responsibility.
 *
 * Called with IRQs masked. The handler must obey interrupt-context rules.
 *
 * Returns true when a registered driver handled the interrupt.
 */
bool irq_dispatch(uint32_t intid);

#endif
