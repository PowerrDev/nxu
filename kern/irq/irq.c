#include <kern/irq/irq.h>

#include <mach/machine/machine_routines.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(__i386__) || defined(__x86_64__)

/*
 * x86 flavour. The interrupt IDs are the 16 lines of the legacy PIC pair,
 * and unlike a GIC INTID a line is routinely shared: every PCI INTx pin is
 * wired to one of a few lines, so several devices answer the same interrupt.
 * Each line therefore holds a short chain of handlers. All of them run on
 * every interrupt and each is expected to check its own device and return
 * quietly when the interrupt is not its own.
 *
 * The chain is a fixed array per line so registration works before any
 * allocator exists. A slot is free while its handler is null.
 */
#define IRQ_LINE_COUNT 16U
#define IRQ_CHAIN_DEPTH 8U

typedef struct {
	irq_handler_t handler;
	void *context;
} irq_entry_t;

static irq_entry_t g_irq_table[IRQ_LINE_COUNT][IRQ_CHAIN_DEPTH];
static bool g_irq_initialized;

/*
 * irq_init:
 *
 * Reset every chain. Repeated initialisation is harmless.
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
 * Append one handler to the chain of a line. The same handler and context
 * pair cannot be registered twice on a line, and a full chain is refused.
 * The slot is filled with local IRQs masked so dispatch cannot observe a
 * partially installed entry.
 */
bool irq_register(uint32_t intid, irq_handler_t handler, void *context)
{
	if (!g_irq_initialized || intid >= IRQ_LINE_COUNT || handler == 0) return false;

	uint64_t irq_state = ml_irq_save();
	irq_entry_t *chain = g_irq_table[intid];
	irq_entry_t *free_entry = 0;

	for (uint32_t index = 0U; index < IRQ_CHAIN_DEPTH; index++) {
		if (chain[index].handler == 0) {
			if (free_entry == 0) free_entry = &chain[index];
		} else if (chain[index].handler == handler && chain[index].context == context) {
			ml_irq_restore(irq_state);
			return false;
		}
	}

	if (free_entry == 0) {
		ml_irq_restore(irq_state);
		return false;
	}

	free_entry->context = context;
	__atomic_store_n(&free_entry->handler, handler, __ATOMIC_RELEASE);
	ml_irq_restore(irq_state);
	return true;
}

/*
 * irq_unregister:
 *
 * Remove the chain entry that matches both handler and context exactly.
 */
bool irq_unregister(uint32_t intid, irq_handler_t handler, void *context)
{
	if (!g_irq_initialized || intid >= IRQ_LINE_COUNT || handler == 0) return false;

	uint64_t irq_state = ml_irq_save();
	irq_entry_t *chain = g_irq_table[intid];

	for (uint32_t index = 0U; index < IRQ_CHAIN_DEPTH; index++) {
		if (chain[index].handler != handler || chain[index].context != context) continue;

		__atomic_store_n(&chain[index].handler, 0, __ATOMIC_RELEASE);
		chain[index].context = 0;
		ml_irq_restore(irq_state);
		return true;
	}

	ml_irq_restore(irq_state);
	return false;
}

/*
 * irq_dispatch:
 *
 * Invoke every handler chained on a line, in slot order. Returns true if at
 * least one handler was present. A handler cannot tell the dispatcher whether
 * the interrupt was its own, so "handled" means "someone was listening".
 */
bool irq_dispatch(uint32_t intid)
{
	if (!g_irq_initialized || intid >= IRQ_LINE_COUNT) return false;

	irq_entry_t *chain = g_irq_table[intid];
	bool handled = false;

	for (uint32_t index = 0U; index < IRQ_CHAIN_DEPTH; index++) {
		irq_handler_t handler = __atomic_load_n(&chain[index].handler, __ATOMIC_ACQUIRE);

		if (handler == 0) continue;
		handler(intid, chain[index].context);
		handled = true;
	}

	return handled;
}

#else /* arm64: one handler per INTID */

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

	uint64_t irq_state = ml_irq_save();
	irq_entry_t *entry = &g_irq_table[intid];

	if (entry->handler != 0) {
		ml_irq_restore(irq_state);
		return false;
	}

	entry->context = context;
	__atomic_store_n(&entry->handler, handler, __ATOMIC_RELEASE);
	ml_irq_restore(irq_state);
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

	uint64_t irq_state = ml_irq_save();
	irq_entry_t *entry = &g_irq_table[intid];

	if (entry->handler != handler || entry->context != context) {
		ml_irq_restore(irq_state);
		return false;
	}

	__atomic_store_n(&entry->handler, 0, __ATOMIC_RELEASE);
	entry->context = 0;
	ml_irq_restore(irq_state);
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

#endif
