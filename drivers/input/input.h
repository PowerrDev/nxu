#ifndef NXU_DRIVERS_INPUT_INPUT_H
#define NXU_DRIVERS_INPUT_INPUT_H

#include <stdbool.h>
#include <stdint.h>

#define INPUT_EVENT_QUEUE_CAPACITY 256U

typedef enum {
	INPUT_DEVICE_UNKNOWN = 0,
	INPUT_DEVICE_KEYBOARD,
	INPUT_DEVICE_MOUSE
} input_device_class_t;

typedef struct {
	uint32_t device_id;
	input_device_class_t device_class;
	uint16_t type;
	uint16_t code;
	int32_t value;
} input_event_t;

/*
 * input_init:
 *
 * Initialize the transport-independent input event queue.
 *
 * Transport drivers publish normalized events here. Device-class drivers
 * consume the same events to maintain keyboard and pointer state. The raw
 * queue is retained for a future userspace input endpoint.
 *
 * Returns true after successful initialization. Repeated initialization is
 * harmless.
 */
bool input_init(void);

/*
 * input_publish:
 *
 * Publish one normalized input event.
 *
 * This routine is safe in interrupt context and never allocates or blocks.
 * The queue is bounded. When full, the new event is dropped and the drop
 * counter is incremented; already queued events remain ordered and intact.
 *
 * Returns true when the event was queued.
 */
bool input_publish(const input_event_t *event);

/*
 * input_read:
 *
 * Remove the oldest normalized event from the raw input queue.
 *
 * The current operation is non-blocking. A later userspace-facing interface
 * may sleep the caller while no input is available.
 *
 * Returns true when an event was returned.
 */
bool input_read(input_event_t *event);

uint64_t input_event_count(void);
uint64_t input_drop_count(void);
uint32_t input_pending_count(void);

/*
 * input_dump_activity:
 *
 * Print input counters only when observable activity changed. This is a
 * bring-up diagnostic and is not part of the future userspace input ABI.
 */
void input_dump_activity(void);

#endif
