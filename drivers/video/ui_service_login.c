/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        drivers/video/ui_service_login.c
 *
 * See drivers/video/ui_service_login.h. Logs say what tepOS answered, never
 * anything about the passcode itself.
 */

#include <drivers/video/ui_service_login.h>

#if defined(NXU_UI_SERVICE)

#include <drivers/input/keyboard.h>
#include <drivers/tep/tep_crypto.h>
#include <drivers/tep/tep_mailbox.h>
#include <kern/console/console.h>

#include <stdbool.h>
#include <stdint.h>

#define UI_LOGIN_LOG(format, ...) kprintf("%s: " format, __func__, ##__VA_ARGS__)

uint64_t ui_service_keyboard_capability(void)
{
	return keyboard_is_present() ? UI_SERVICE_HOST_CAP_KEYBOARD : 0ULL;
}

bool ui_service_poll_key(UIServiceHostEvent *event)
{
	keyboard_key_t key;

	if (event == 0 || !keyboard_take_key(&key)) return false;

	event->event_type = UI_SERVICE_EVENT_KEY_DOWN;
	event->button = key.code;
	event->reserved = key.character;
	return true;
}

/* tepOS's answer as a UI_SERVICE_AUTH_* code; anything unexpected is an error, never OK. */
static uint32_t ui_login_result(tep_result_t result)
{
	switch (result) {
	case TEP_OK: return UI_SERVICE_AUTH_OK;
	case TEP_ERR_DENIED: return UI_SERVICE_AUTH_DENIED;
	case TEP_ERR_RETRY_LATER: return UI_SERVICE_AUTH_RETRY_LATER;
	case TEP_ERR_LOCKED: return UI_SERVICE_AUTH_LOCKED;
	case TEP_ERR_UNAVAILABLE: return UI_SERVICE_AUTH_UNAVAILABLE;
	case TEP_ERR_NOT_FOUND: return UI_SERVICE_AUTH_NOT_SET;
	case TEP_ERR_INVALID: return UI_SERVICE_AUTH_INVALID;
	default: return UI_SERVICE_AUTH_ERROR;
	}
}

static uint32_t ui_login_status(void *context, UIServiceAuthStatus *status)
{
	(void)context;

	tep_auth_status_t tep_status;

	if (status == 0) return UI_SERVICE_AUTH_INVALID;

	tep_result_t result = tep_auth_status(&tep_status);

	if (result != TEP_OK) return ui_login_result(result);

	status->passcode_set = tep_status.passcode_set ? 1U : 0U;
	status->failures = tep_status.failures;
	status->locked = tep_status.locked ? 1U : 0U;
	status->wait_seconds = tep_status.wait_seconds;
	return UI_SERVICE_AUTH_OK;
}

static uint32_t ui_login_verify(void *context, const uint8_t *passcode, uint32_t length, uint32_t *wait_seconds)
{
	(void)context;

	tep_result_t result = tep_auth_verify(passcode, length, wait_seconds);

	if (result == TEP_OK) {
		UI_LOGIN_LOG("passcode accepted by tepOS\n");
	} else if (result == TEP_ERR_RETRY_LATER && wait_seconds != 0) {
		UI_LOGIN_LOG("passcode refused by tepOS: retry in %u s\n", (unsigned)*wait_seconds);
	} else {
		UI_LOGIN_LOG("passcode refused by tepOS: %s\n", tep_result_name(result));
	}
	return ui_login_result(result);
}

static uint32_t ui_login_set(void *context, const uint8_t *old_passcode, uint32_t old_length, const uint8_t *passcode, uint32_t length, uint32_t *wait_seconds)
{
	(void)context;

	tep_result_t result = tep_auth_set(old_passcode, old_length, passcode, length, wait_seconds);

	if (result == TEP_OK) {
		UI_LOGIN_LOG("passcode set in tepOS\n");
	} else {
		UI_LOGIN_LOG("setting the passcode failed: %s\n", tep_result_name(result));
	}
	return ui_login_result(result);
}

bool ui_service_login(const UIServiceHostV5 *host)
{
	if (tep_mailbox_link_state() == TEP_LINK_ABSENT) {
		UI_LOGIN_LOG("no Trusted Enclave link: no login screen\n");
		return true;
	}

	UIServiceLoginHostV1 login = {
		.header = {
			.struct_size = sizeof(UIServiceLoginHostV1),
			.abi_version = UI_SERVICE_LOGIN_ABI_VERSION_V1
		},
		.context = 0,
		.auth_status = ui_login_status,
		.auth_verify = ui_login_verify,
		.auth_set = ui_login_set
	};

	if (UIServiceLoginHostV1Size() != sizeof(UIServiceLoginHostV1)) {
		UI_LOGIN_LOG("login host table size mismatch (framework %u, kernel %u): the desktop stays locked\n",
			(unsigned)UIServiceLoginHostV1Size(), (unsigned)sizeof(UIServiceLoginHostV1));
		return false;
	}

	/* Keys pressed before the screen was up are not part of anything typed on it. */
	keyboard_flush_keys();
	UI_LOGIN_LOG("the desktop waits for the passcode (checked by tepOS)\n");

	uint32_t status = UIServiceRunLogin(host, &login);

	if (status != UI_SERVICE_STATUS_OK) {
		UI_LOGIN_LOG("login screen ended with status %u: the desktop stays locked\n", (unsigned)status);
		return false;
	}

	UI_LOGIN_LOG("unlocked\n");
	return true;
}

#endif
