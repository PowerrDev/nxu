#include <kern/console/console.h>
#include <drivers/input/input.h>

#include <mach/machine/machine_routines.h>
#include <drivers/input/keyboard.h>
#include <drivers/input/mouse.h>
#include <platform/uart.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct {
	input_event_t events[INPUT_EVENT_QUEUE_CAPACITY];
	uint32_t head;
	uint32_t tail;
	uint32_t count;
	uint64_t published;
	uint64_t dropped;
	uint64_t last_dump_published;
	uint64_t last_dump_dropped;
	bool initialized;
} input_state_t;

static input_state_t g_input;

/*
 * input_init:
 *
 * Initialize the bounded raw input queue and accounting state.
 */
bool input_init(void)
{
	if (g_input.initialized) return true;
	memset(&g_input, 0, sizeof(g_input));
	g_input.initialized = true;
	return true;
}

/*
 * input_publish:
 *
 * Append one normalized input event without blocking or allocating. Queue
 * overflow drops the incoming event and preserves existing FIFO order.
 */
bool input_publish(const input_event_t *event)
{
	if (!g_input.initialized || event == 0) return false;

	uint64_t irq_state = ml_irq_save();

	if (g_input.count == INPUT_EVENT_QUEUE_CAPACITY) {
		g_input.dropped++;
		ml_irq_restore(irq_state);
		return false;
	}

	g_input.events[g_input.tail] = *event;
	g_input.tail = (g_input.tail + 1U) % INPUT_EVENT_QUEUE_CAPACITY;
	g_input.count++;
	g_input.published++;

	ml_irq_restore(irq_state);
	return true;
}

/*
 * input_read:
 *
 * Remove the oldest queued input event. This primitive is non-blocking and
 * is intended to sit below a future waitable userspace interface.
 */
bool input_read(input_event_t *event)
{
	if (!g_input.initialized || event == 0) return false;

	uint64_t irq_state = ml_irq_save();

	if (g_input.count == 0U) {
		ml_irq_restore(irq_state);
		return false;
	}

	*event = g_input.events[g_input.head];
	g_input.head = (g_input.head + 1U) % INPUT_EVENT_QUEUE_CAPACITY;
	g_input.count--;

	ml_irq_restore(irq_state);
	return true;
}

uint64_t input_event_count(void)
{
	return __atomic_load_n(&g_input.published, __ATOMIC_ACQUIRE);
}

uint64_t input_drop_count(void)
{
	return __atomic_load_n(&g_input.dropped, __ATOMIC_ACQUIRE);
}

uint32_t input_pending_count(void)
{
	return __atomic_load_n(&g_input.count, __ATOMIC_ACQUIRE);
}

/*
 * input_dump_activity:
 *
 * Emit low-frequency bring-up diagnostics when input counters change. This
 * routine is deliberately outside the IRQ event path.
 */
void input_dump_activity(void)
{
	if (!g_input.initialized) return;

	uint64_t published = input_event_count();
	uint64_t dropped = input_drop_count();

	if (
		published == g_input.last_dump_published &&
		dropped == g_input.last_dump_dropped
	) {
		return;
	}

	g_input.last_dump_published = published;
	g_input.last_dump_dropped = dropped;

	kputs("input: events ");
	kputu64(published);
	kputs(", pending ");
	kputu64(input_pending_count());
	kputs(", dropped ");
	kputu64(dropped);
	kputc('\n');

	keyboard_dump_state();
	mouse_dump_state();
}
