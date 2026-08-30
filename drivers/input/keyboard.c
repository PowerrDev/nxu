#include <kern/console/console.h>
#include <drivers/input/keyboard.h>

#include <platform/uart.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define KEY_RELEASE 0
#define KEY_PRESS 1
#define KEY_REPEAT 2

#define KEY_LEFTCTRL 29U
#define KEY_LEFTSHIFT 42U
#define KEY_RIGHTSHIFT 54U
#define KEY_LEFTALT 56U
#define KEY_CAPSLOCK 58U
#define KEY_RIGHTCTRL 97U
#define KEY_RIGHTALT 100U

#define KEYBOARD_KEY_BITMAP_WORDS 8U

typedef struct {
	uint64_t down[KEYBOARD_KEY_BITMAP_WORDS];
	uint32_t device_id;
	uint32_t modifiers;
	uint64_t events;
	char last_character;
	bool initialized;
	bool present;
} keyboard_state_t;

static keyboard_state_t g_keyboard;

static bool keyboard_key_in_bitmap(uint16_t code)
{
	return code < KEYBOARD_KEY_BITMAP_WORDS * 64U;
}

static void keyboard_set_down(uint16_t code, bool down)
{
	if (!keyboard_key_in_bitmap(code)) return;

	uint32_t word = code / 64U;
	uint32_t bit = code % 64U;
	uint64_t mask = 1ULL << bit;

	if (down) {
		g_keyboard.down[word] |= mask;
	} else {
		g_keyboard.down[word] &= ~mask;
	}
}

static bool keyboard_is_down(uint16_t code)
{
	if (!keyboard_key_in_bitmap(code)) return false;
	return (g_keyboard.down[code / 64U] & (1ULL << (code % 64U))) != 0ULL;
}

/*
 * keyboard_update_modifiers:
 *
 * Recompute modifier state from the key-down bitmap while preserving the
 * toggled Caps Lock state.
 */
static void keyboard_update_modifiers(void)
{
	uint32_t modifiers = g_keyboard.modifiers & KEYBOARD_MOD_CAPS;

	if (keyboard_is_down(KEY_LEFTSHIFT) || keyboard_is_down(KEY_RIGHTSHIFT)) {
		modifiers |= KEYBOARD_MOD_SHIFT;
	}

	if (keyboard_is_down(KEY_LEFTCTRL) || keyboard_is_down(KEY_RIGHTCTRL)) {
		modifiers |= KEYBOARD_MOD_CTRL;
	}

	if (keyboard_is_down(KEY_LEFTALT) || keyboard_is_down(KEY_RIGHTALT)) {
		modifiers |= KEYBOARD_MOD_ALT;
	}

	g_keyboard.modifiers = modifiers;
}

static char keyboard_letter(uint16_t code, bool upper)
{
	static const char letters[] = "qwertyuiopasdfghjklzxcvbnm";
	static const uint16_t codes[] = {
		16U, 17U, 18U, 19U, 20U, 21U, 22U, 23U, 24U, 25U,
		30U, 31U, 32U, 33U, 34U, 35U, 36U, 37U, 38U,
		44U, 45U, 46U, 47U, 48U, 49U, 50U
	};

	for (uint32_t index = 0U; index < sizeof(codes) / sizeof(codes[0]); index++) {
		if (codes[index] != code) continue;
		char value = letters[index];
		return upper ? (char)(value - 'a' + 'A') : value;
	}

	return '\0';
}

/*
 * keyboard_translate:
 *
 * Translate a restricted evdev key code into the temporary bring-up
 * character set. This table is diagnostic policy and is not the future
 * userspace keyboard layout interface.
 */
static char keyboard_translate(uint16_t code)
{
	bool shift = (g_keyboard.modifiers & KEYBOARD_MOD_SHIFT) != 0U;
	bool caps = (g_keyboard.modifiers & KEYBOARD_MOD_CAPS) != 0U;
	char letter = keyboard_letter(code, shift != caps);
	if (letter != '\0') return letter;

	if (code >= 2U && code <= 11U) {
		static const char normal[] = "1234567890";
		static const char shifted[] = "!@#$%^&*()";
		return shift ? shifted[code - 2U] : normal[code - 2U];
	}

	switch (code) {
	case 1U: return 27;
	case 12U: return shift ? '_' : '-';
	case 13U: return shift ? '+' : '=';
	case 14U: return '\b';
	case 15U: return '\t';
	case 26U: return shift ? '{' : '[';
	case 27U: return shift ? '}' : ']';
	case 28U: return '\n';
	case 39U: return shift ? ':' : ';';
	case 40U: return shift ? '"' : '\'';
	case 41U: return shift ? '~' : '`';
	case 43U: return shift ? '|' : '\\';
	case 51U: return shift ? '<' : ',';
	case 52U: return shift ? '>' : '.';
	case 53U: return shift ? '?' : '/';
	case 57U: return ' ';
	default: return '\0';
	}
}

/*
 * keyboard_init:
 *
 * Reset keyboard class state before an input device is attached.
 */
bool keyboard_init(void)
{
	if (g_keyboard.initialized) return true;
	memset(&g_keyboard, 0, sizeof(g_keyboard));
	g_keyboard.initialized = true;
	return true;
}

/*
 * keyboard_attach:
 *
 * Select one normalized input device as the active system keyboard.
 */
bool keyboard_attach(uint32_t device_id)
{
	if (!g_keyboard.initialized || g_keyboard.present || device_id == 0U) return false;
	g_keyboard.device_id = device_id;
	g_keyboard.present = true;
	return true;
}

/*
 * keyboard_handle_event:
 *
 * Apply one EV_KEY event to key-down, modifier, and diagnostic character
 * state. The routine is bounded and safe for invocation from the input
 * interrupt path.
 */
void keyboard_handle_event(const input_event_t *event)
{
	if (
		event == 0 ||
		!g_keyboard.present ||
		event->device_id != g_keyboard.device_id ||
		event->type != 1U
	) {
		return;
	}

	bool press = event->value == KEY_PRESS || event->value == KEY_REPEAT;
	if (event->value == KEY_PRESS || event->value == KEY_RELEASE) {
		keyboard_set_down(event->code, press);
	}

	if (event->code == KEY_CAPSLOCK && event->value == KEY_PRESS) {
		g_keyboard.modifiers ^= KEYBOARD_MOD_CAPS;
	}

	keyboard_update_modifiers();
	g_keyboard.events++;

	if (press) {
		char character = keyboard_translate(event->code);
		if (character != '\0') g_keyboard.last_character = character;
	}
}

bool keyboard_is_present(void)
{
	return g_keyboard.present;
}

uint32_t keyboard_device_id(void)
{
	return g_keyboard.present ? g_keyboard.device_id : 0U;
}

uint32_t keyboard_modifiers(void)
{
	return g_keyboard.modifiers;
}

uint64_t keyboard_event_count(void)
{
	return g_keyboard.events;
}

char keyboard_last_character(void)
{
	return g_keyboard.last_character;
}

/*
 * keyboard_dump_state:
 *
 * Print keyboard state only after observable activity changes.
 */
void keyboard_dump_state(void)
{
	if (!g_keyboard.present) return;

	kputs("keyboard: events ");
	kputu64(g_keyboard.events);
	kputs(", modifiers 0x");
	kputhex_byte((uint8_t)g_keyboard.modifiers);

	if (g_keyboard.last_character != '\0') {
		kputs(", last '");
		if (g_keyboard.last_character == '\n') {
			kputs("\\n");
		} else if (g_keyboard.last_character == '\t') {
			kputs("\\t");
		} else {
			kputc(g_keyboard.last_character);
		}
		kputc('\'');
	}

	kputc('\n');
}
