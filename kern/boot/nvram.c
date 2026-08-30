#include <kern/boot/nvram.h>

#include <string.h>

typedef struct {
	char boot_args[NVRAM_VALUE_MAX + 1U];
	uint32_t boot_args_length;
	nvram_backend_t backend;
	bool initialized;
} nvram_state_t;

static nvram_state_t g_nvram;

static uint32_t
nvram_string_length_bounded(const char *string, uint32_t limit)
{
	if (string == 0) return 0U;

	uint32_t length = 0U;
	while (length < limit && string[length] != '\0') length++;
	return length;
}

bool
nvram_bootstrap(const dtb_t *dtb)
{
	if (g_nvram.initialized || dtb == 0) return false;

	memset(&g_nvram, 0, sizeof(g_nvram));
	g_nvram.backend = NVRAM_BACKEND_DEVICE_TREE;

	const void *value = 0;
	uint32_t length = 0U;

	if (dtb_chosen_property_get(dtb, "bootargs", &value, &length) && value != 0 && length != 0U) {
		const char *source = value;
		uint32_t copy_length = length;

		if (copy_length != 0U && source[copy_length - 1U] == '\0') copy_length--;
		if (copy_length > NVRAM_VALUE_MAX) copy_length = NVRAM_VALUE_MAX;

		memcpy(g_nvram.boot_args, source, copy_length);
		g_nvram.boot_args[copy_length] = '\0';
		g_nvram.boot_args_length = copy_length;
	}

	g_nvram.initialized = true;
	return true;
}

nvram_status_t
nvram_get(const char *name, char *value, uint32_t capacity, uint32_t *length)
{
	if (length != 0) *length = 0U;
	if (!g_nvram.initialized || name == 0 || value == 0 || length == 0 || capacity == 0U) return NVRAM_STATUS_INVALID;
	if (strcmp(name, NVRAM_BOOT_ARGS_NAME) != 0) return NVRAM_STATUS_NOT_FOUND;
	if (capacity <= g_nvram.boot_args_length) return NVRAM_STATUS_NO_SPACE;

	memcpy(value, g_nvram.boot_args, g_nvram.boot_args_length + 1U);
	*length = g_nvram.boot_args_length;
	return NVRAM_STATUS_OK;
}

nvram_status_t
nvram_set(const char *name, const char *value)
{
	if (!g_nvram.initialized || name == 0 || value == 0) return NVRAM_STATUS_INVALID;
	if (strcmp(name, NVRAM_BOOT_ARGS_NAME) != 0) return NVRAM_STATUS_NOT_FOUND;
	if (nvram_string_length_bounded(value, NVRAM_VALUE_MAX + 1U) > NVRAM_VALUE_MAX) return NVRAM_STATUS_NO_SPACE;
	return NVRAM_STATUS_READ_ONLY;
}

nvram_backend_t
nvram_backend(void)
{
	return g_nvram.initialized ? g_nvram.backend : NVRAM_BACKEND_NONE;
}

bool
nvram_is_persistent(void)
{
	return false;
}

const char *
nvram_status_name(nvram_status_t status)
{
	switch (status) {
	case NVRAM_STATUS_OK: return "ok";
	case NVRAM_STATUS_NOT_FOUND: return "not found";
	case NVRAM_STATUS_INVALID: return "invalid";
	case NVRAM_STATUS_NO_SPACE: return "no space";
	case NVRAM_STATUS_READ_ONLY: return "read only";
	default: return "unknown";
	}
}
