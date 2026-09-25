#ifndef NXU_DRIVERS_INPUT_KEYBOARD_H
#define NXU_DRIVERS_INPUT_KEYBOARD_H

#include <drivers/input/input.h>

#include <stdbool.h>
#include <stdint.h>

#define KEYBOARD_MOD_SHIFT 0x01U
#define KEYBOARD_MOD_CTRL  0x02U
#define KEYBOARD_MOD_ALT   0x04U
#define KEYBOARD_MOD_CAPS  0x08U

/*
 * keyboard_init:
 *
 * Initialize global keyboard-class state.
 *
 * The class layer is independent of the transport. Transport drivers are
 * responsible for converting their native reports into input_event_t before
 * calling keyboard_handle_event().
 */
bool keyboard_init(void);

/*
 * keyboard_attach:
 *
 * Attach one normalized input device as the system keyboard.
 *
 * Only one keyboard is selected during the current bring-up stage.
 *
 * Returns true when the device became the active keyboard.
 */
bool keyboard_attach(uint32_t device_id);

/*
 * keyboard_handle_event:
 *
 * Consume one keyboard-class input event.
 *
 * The driver maintains key-down and modifier state. A small US-QWERTY
 * translation is retained only for UART validation; raw key events remain
 * authoritative and are preserved for future userspace delivery.
 *
 * May be called from interrupt context. It must not block.
 */
void keyboard_handle_event(const input_event_t *event);

bool keyboard_is_present(void);
uint32_t keyboard_device_id(void);
uint32_t keyboard_modifiers(void);
uint64_t keyboard_event_count(void);
char keyboard_last_character(void);

/*
 * Key presses (and auto-repeats) for the UI, in order: a small queue filled
 * from the input path and drained by one consumer. `character` is what the
 * key types under the bring-up US layout, 0 for none. When the queue is full
 * new presses are dropped.
 */
typedef struct {
	uint16_t code;
	uint8_t character;
	uint8_t modifiers;
} keyboard_key_t;

bool keyboard_take_key(keyboard_key_t *key);
/* Forget queued presses (e.g. typed before a login screen appeared). */
void keyboard_flush_keys(void);
void keyboard_dump_state(void);

#endif
