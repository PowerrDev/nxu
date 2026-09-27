#ifndef NXU_DRIVERS_INPUT_MOUSE_H
#define NXU_DRIVERS_INPUT_MOUSE_H

#include <drivers/input/input.h>

#include <stdbool.h>
#include <stdint.h>

#define MOUSE_BUTTON_LEFT 0x01U
#define MOUSE_BUTTON_RIGHT 0x02U
#define MOUSE_BUTTON_MIDDLE 0x04U

/*
 * mouse_init:
 *
 * Initialize relative-pointer class state.
 *
 * Axis and button events are accumulated until EV_SYN/SYN_REPORT. Consumers
 * therefore observe a coherent pointer sample rather than a partially
 * delivered transport packet.
 */
bool mouse_init(void);

/*
 * mouse_attach:
 *
 * Attach one normalized input device as the system relative pointer.
 *
 * Returns true when the device became the active mouse.
 */
bool mouse_attach(uint32_t device_id);

/*
 * mouse_handle_event:
 *
 * Consume one normalized pointer event and commit accumulated state at a
 * synchronization boundary.
 *
 * May be called from interrupt context. It must not block.
 */
void mouse_handle_event(const input_event_t *event);

bool mouse_is_present(void);
uint32_t mouse_device_id(void);
uint32_t mouse_buttons(void);

/*
 * mouse_take_button_edges:
 *
 * The buttons that went down, and those that came up, since the last call:
 * a click that is over before its reader looks at mouse_buttons() again
 * still shows up here.
 */
void mouse_take_button_edges(uint32_t *pressed, uint32_t *released);
int64_t mouse_x(void);
int64_t mouse_y(void);
int64_t mouse_wheel(void);

/*
 * mouse_take_delta:
 *
 * Pop one queued packet's relative motion (oldest first). Returns false once
 * nothing is queued. See mouse.c for why this exists alongside mouse_x()/
 * mouse_y() instead of replacing them.
 */
bool mouse_take_delta(int32_t *dx, int32_t *dy);
uint64_t mouse_event_count(void);
uint64_t mouse_packet_count(void);
void mouse_dump_state(void);

#endif
