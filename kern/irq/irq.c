#include <kern/irq/irq.h>

#include <mach/arm64/system.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct {
	irq_handler_t handler;
	void *context;
} irq_entry_t;

static irq_entry_t g_irq_table[IRQ_MAX_INTID + 1U];
static bool g_irq_initialized;

/*
 * irq_init:
 *
 * Initialize the fixed architectural interrupt dispatch table.
 * Registration state is reset before device drivers are attached.
 */
bool irq_init(void)
{
	if (g_irq_initialized) return true;
	memset(g_irq_table, 0, sizeof(g_irq_table));
	g_irq_initialized = true;
	return true;
}

/*
 * irq_register:
 *
 * Install one interrupt source binding. The slot is updated with local
 * IRQs masked so interrupt dispatch cannot observe a partially installed
 * handler.
 */
bool irq_register(uint32_t intid, irq_handler_t handler, void *context)
{
	if (!g_irq_initialized || intid > IRQ_MAX_INTID || handler == 0) return false;

	uint64_t irq_state = arm64_irq_save();
	irq_entry_t *entry = &g_irq_table[intid];

	if (entry->handler != 0) {
		arm64_irq_restore(irq_state);
		return false;
	}

	entry->context = context;
	__atomic_store_n(&entry->handler, handler, __ATOMIC_RELEASE);
	arm64_irq_restore(irq_state);
	return true;
}

/*
 * irq_unregister:
 *
 * Remove one interrupt source binding. The handler and context must still
 * match the current owner.
 */
bool irq_unregister(uint32_t intid, irq_handler_t handler, void *context)
{
	if (!g_irq_initialized || intid > IRQ_MAX_INTID || handler == 0) return false;

	uint64_t irq_state = arm64_irq_save();
	irq_entry_t *entry = &g_irq_table[intid];

	if (entry->handler != handler || entry->context != context) {
		arm64_irq_restore(irq_state);
		return false;
	}

	__atomic_store_n(&entry->handler, 0, __ATOMIC_RELEASE);
	entry->context = 0;
	arm64_irq_restore(irq_state);
	return true;
}

/*
 * irq_dispatch:
 *
 * Invoke the driver routine for one already-acknowledged INTID. The caller
 * retains responsibility for GIC end-of-interrupt.
 */
bool irq_dispatch(uint32_t intid)
{
	if (!g_irq_initialized || intid > IRQ_MAX_INTID) return false;

	irq_entry_t *entry = &g_irq_table[intid];
	irq_handler_t handler = __atomic_load_n(&entry->handler, __ATOMIC_ACQUIRE);

	if (handler == 0) return false;
	handler(intid, entry->context);
	return true;
}
