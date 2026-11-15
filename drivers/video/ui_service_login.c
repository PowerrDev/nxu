/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        drivers/video/ui_service_login.c
 *
 * See drivers/video/ui_service_login.h. Logs say what the passcode store
 * (kern/security/passcode.c) answered, never anything about the passcode
 * itself.
 */

#include <drivers/video/ui_service_login.h>

#if defined(NXU_UI_SERVICE)

#include <drivers/input/keyboard.h>
#include <kern/console/console.h>
#include <kern/security/passcode.h>

#include <stdbool.h>
#include <stdint.h>

#define UI_LOGIN_LOG(format, ...) kprintf(format, ##__VA_ARGS__)

uint64_t
ui_service_keyboard_capability(void)
{
	return keyboard_is_present() ? UI_SERVICE_HOST_CAP_KEYBOARD : 0ULL;
}

bool
ui_service_poll_key(UIServiceHostEvent *event)
{
	keyboard_key_t key;

	if (event == 0 || !keyboard_take_key(&key)) return false;

	uint32_t character = key.character;

	/*
	 * Control with a letter or one of @ [ \ ] ^ _ types the control character
	 * (^C is 3), as on any terminal keyboard: that is what a terminal's line
	 * discipline acts on.
	 */
	if ((key.modifiers & KEYBOARD_MOD_CTRL) != 0U &&
		((character >= 'a' && character <= 'z') || (character >= '@' && character <= '_')))
		character &= 0x1FU;

	event->event_type = UI_SERVICE_EVENT_KEY_DOWN;
	event->button = key.code;
	event->reserved = character;
	return true;
}

/* The store's answer as a UI_SERVICE_AUTH_* code; anything unexpected is an error, never OK. */
static uint32_t
ui_login_result(passcode_result_t result)
{
	switch (result) {
	case PASSCODE_OK:
		return UI_SERVICE_AUTH_OK;
	case PASSCODE_ERR_DENIED:
		return UI_SERVICE_AUTH_DENIED;
	case PASSCODE_ERR_RETRY_LATER:
		return UI_SERVICE_AUTH_RETRY_LATER;
	case PASSCODE_ERR_LOCKED:
		return UI_SERVICE_AUTH_LOCKED;
	case PASSCODE_ERR_UNAVAILABLE:
		return UI_SERVICE_AUTH_UNAVAILABLE;
	case PASSCODE_ERR_NOT_FOUND:
		return UI_SERVICE_AUTH_NOT_SET;
	case PASSCODE_ERR_INVALID:
		return UI_SERVICE_AUTH_INVALID;
	default:
		return UI_SERVICE_AUTH_ERROR;
	}
}

static uint32_t
ui_login_status(void *context, UIServiceAuthStatus *status)
{
	(void)context;

	passcode_status_t passcode;

	if (status == 0) return UI_SERVICE_AUTH_INVALID;

	passcode_result_t result = passcode_status(&passcode);

	if (result != PASSCODE_OK) return ui_login_result(result);

	status->passcode_set = passcode.passcode_set ? 1U : 0U;
	status->failures = passcode.failures;
	status->locked = passcode.locked ? 1U : 0U;
	status->wait_seconds = passcode.wait_seconds;
	return UI_SERVICE_AUTH_OK;
}

static uint32_t
ui_login_verify(void *context, const uint8_t *passcode, uint32_t length, uint32_t *wait_seconds)
{
	(void)context;

	passcode_result_t result = passcode_verify(passcode, length, wait_seconds);

	if (result == PASSCODE_OK) {
		UI_LOGIN_LOG("passcode accepted\n");
	} else if (result == PASSCODE_ERR_RETRY_LATER && wait_seconds != 0) {
		UI_LOGIN_LOG("passcode refused: retry in %u s\n", (unsigned)*wait_seconds);
	} else {
		UI_LOGIN_LOG("passcode refused: %s\n", passcode_result_name(result));
	}
	return ui_login_result(result);
}

static uint32_t
ui_login_set(void *context, const uint8_t *old_passcode, uint32_t old_length, const uint8_t *passcode, uint32_t length,
			 uint32_t *wait_seconds)
{
	(void)context;

	passcode_result_t result = passcode_set(old_passcode, old_length, passcode, length, wait_seconds);

	if (result == PASSCODE_OK) {
		UI_LOGIN_LOG("passcode set\n");
	} else {
		UI_LOGIN_LOG("setting the passcode failed: %s\n", passcode_result_name(result));
	}
	return ui_login_result(result);
}

bool
ui_service_login(const UIServiceHostV5 *host)
{
#if defined(NXU_LOGIN_UNLOCKED)
	/* A test kernel (makedefs/tests.mk): nobody is at the keyboard to type a passcode. */
	(void)host;
	(void)ui_login_status;
	(void)ui_login_verify;
	(void)ui_login_set;
	UI_LOGIN_LOG("test kernel built unlocked: no login screen\n");
	return true;
#else

	UIServiceLoginHostV1 login = {
		.header = {.struct_size = sizeof(UIServiceLoginHostV1), .abi_version = UI_SERVICE_LOGIN_ABI_VERSION_V1},
		.context = 0,
		.auth_status = ui_login_status,
		.auth_verify = ui_login_verify,
		.auth_set = ui_login_set};

	if (UIServiceLoginHostV1Size() != sizeof(UIServiceLoginHostV1)) {
		UI_LOGIN_LOG("login host table size mismatch (framework %u, kernel %u): the desktop stays locked\n",
					 (unsigned)UIServiceLoginHostV1Size(), (unsigned)sizeof(UIServiceLoginHostV1));
		return false;
	}

	/* Keys pressed before the screen was up are not part of anything typed on it. */
	keyboard_flush_keys();
	UI_LOGIN_LOG("the desktop waits for the passcode\n");

	uint32_t status = UIServiceRunLogin(host, &login);

	if (status != UI_SERVICE_STATUS_OK) {
		UI_LOGIN_LOG("login screen ended with status %u: the desktop stays locked\n", (unsigned)status);
		return false;
	}

	UI_LOGIN_LOG("unlocked\n");
	return true;
#endif
}

#endif
