#include <drivers/virtio/virtio_sound_core.h>

#include <drivers/virtio/virtio_features.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool virtio_snd_pcm_transition(virtio_snd_pcm_state_t state, virtio_snd_pcm_op_t op, virtio_snd_pcm_state_t *next)
{
	virtio_snd_pcm_state_t result;

	switch (op) {
	case VIRTIO_SND_PCM_OP_SET_PARAMS:
		if (state != VIRTIO_SND_PCM_STATE_UNINIT && state != VIRTIO_SND_PCM_STATE_PARAMS_SET && state != VIRTIO_SND_PCM_STATE_PREPARED && state != VIRTIO_SND_PCM_STATE_RELEASED) return false;
		result = VIRTIO_SND_PCM_STATE_PARAMS_SET;
		break;
	case VIRTIO_SND_PCM_OP_PREPARE:
		if (state != VIRTIO_SND_PCM_STATE_PARAMS_SET && state != VIRTIO_SND_PCM_STATE_RELEASED) return false;
		result = VIRTIO_SND_PCM_STATE_PREPARED;
		break;
	case VIRTIO_SND_PCM_OP_START:
		if (state != VIRTIO_SND_PCM_STATE_PREPARED && state != VIRTIO_SND_PCM_STATE_STOPPED) return false;
		result = VIRTIO_SND_PCM_STATE_STARTED;
		break;
	case VIRTIO_SND_PCM_OP_STOP:
		if (state != VIRTIO_SND_PCM_STATE_STARTED) return false;
		result = VIRTIO_SND_PCM_STATE_STOPPED;
		break;
	case VIRTIO_SND_PCM_OP_RELEASE:
		if (state != VIRTIO_SND_PCM_STATE_PREPARED && state != VIRTIO_SND_PCM_STATE_STOPPED) return false;
		result = VIRTIO_SND_PCM_STATE_RELEASED;
		break;
	default:
		return false;
	}

	if (next != 0) *next = result;
	return true;
}

uint32_t virtio_snd_pcm_op_request(virtio_snd_pcm_op_t op)
{
	switch (op) {
	case VIRTIO_SND_PCM_OP_SET_PARAMS: return VIRTIO_SND_R_PCM_SET_PARAMS;
	case VIRTIO_SND_PCM_OP_PREPARE: return VIRTIO_SND_R_PCM_PREPARE;
	case VIRTIO_SND_PCM_OP_START: return VIRTIO_SND_R_PCM_START;
	case VIRTIO_SND_PCM_OP_STOP: return VIRTIO_SND_R_PCM_STOP;
	case VIRTIO_SND_PCM_OP_RELEASE: return VIRTIO_SND_R_PCM_RELEASE;
	default: return 0U;
	}
}

bool virtio_snd_negotiate(uint64_t offered, uint64_t *accepted)
{
	if (accepted != 0) *accepted = 0ULL;
	if ((offered & (1ULL << VIRTIO_F_VERSION_1)) == 0ULL) return false;

	if (accepted != 0) *accepted = 1ULL << VIRTIO_F_VERSION_1;
	return true;
}

const char *virtio_snd_feature_name(uint32_t bit)
{
	switch (bit) {
	case VIRTIO_SND_F_CTLS: return "SND_F_CTLS";
	case VIRTIO_F_NOTIFY_ON_EMPTY: return "F_NOTIFY_ON_EMPTY";
	case VIRTIO_F_ANY_LAYOUT: return "F_ANY_LAYOUT";
	case VIRTIO_F_RING_INDIRECT_DESC: return "F_RING_INDIRECT_DESC";
	case VIRTIO_F_RING_EVENT_IDX: return "F_RING_EVENT_IDX";
	case VIRTIO_F_VERSION_1: return "F_VERSION_1";
	case VIRTIO_F_ACCESS_PLATFORM: return "F_ACCESS_PLATFORM";
	case VIRTIO_F_RING_PACKED: return "F_RING_PACKED";
	case VIRTIO_F_IN_ORDER: return "F_IN_ORDER";
	case VIRTIO_F_ORDER_PLATFORM: return "F_ORDER_PLATFORM";
	case VIRTIO_F_SR_IOV: return "F_SR_IOV";
	case VIRTIO_F_NOTIFICATION_DATA: return "F_NOTIFICATION_DATA";
	case VIRTIO_F_NOTIF_CONFIG_DATA: return "F_NOTIF_CONFIG_DATA";
	case VIRTIO_F_RING_RESET: return "F_RING_RESET";
	default: return "unknown";
	}
}

virtio_snd_error_t virtio_snd_error_from_status(uint32_t status)
{
	switch (status) {
	case VIRTIO_SND_S_OK: return VIRTIO_SND_E_NONE;
	case VIRTIO_SND_S_BAD_MSG: return VIRTIO_SND_E_BAD_MSG;
	case VIRTIO_SND_S_NOT_SUPP: return VIRTIO_SND_E_NOT_SUPP;
	default: return VIRTIO_SND_E_IO;
	}
}

const char *virtio_snd_error_name(virtio_snd_error_t error)
{
	switch (error) {
	case VIRTIO_SND_E_NONE: return "ok";
	case VIRTIO_SND_E_BAD_MSG: return "the device rejected the message (BAD_MSG)";
	case VIRTIO_SND_E_NOT_SUPP: return "not supported by the device (NOT_SUPP)";
	case VIRTIO_SND_E_IO: return "I/O error (IO_ERR)";
	case VIRTIO_SND_E_TIMEOUT: return "the device did not answer in time";
	case VIRTIO_SND_E_DEAD: return "the device stopped answering earlier";
	case VIRTIO_SND_E_ILLEGAL: return "illegal in the stream's current state";
	case VIRTIO_SND_E_INVALID: return "invalid parameters";
	case VIRTIO_SND_E_NO_MEMORY: return "out of memory";
	case VIRTIO_SND_E_BUSY: return "the device is in use";
	case VIRTIO_SND_E_AGAIN: return "would block";
	case VIRTIO_SND_E_INTERRUPTED: return "interrupted by a signal";
	default: return "?";
	}
}

const char *virtio_snd_status_name(uint32_t status)
{
	switch (status) {
	case VIRTIO_SND_S_OK: return "OK";
	case VIRTIO_SND_S_BAD_MSG: return "BAD_MSG";
	case VIRTIO_SND_S_NOT_SUPP: return "NOT_SUPP";
	case VIRTIO_SND_S_IO_ERR: return "IO_ERR";
	default: return "?";
	}
}

const char *virtio_snd_request_name(uint32_t code)
{
	switch (code) {
	case VIRTIO_SND_R_JACK_INFO: return "JACK_INFO";
	case VIRTIO_SND_R_JACK_REMAP: return "JACK_REMAP";
	case VIRTIO_SND_R_PCM_INFO: return "PCM_INFO";
	case VIRTIO_SND_R_PCM_SET_PARAMS: return "PCM_SET_PARAMS";
	case VIRTIO_SND_R_PCM_PREPARE: return "PCM_PREPARE";
	case VIRTIO_SND_R_PCM_RELEASE: return "PCM_RELEASE";
	case VIRTIO_SND_R_PCM_START: return "PCM_START";
	case VIRTIO_SND_R_PCM_STOP: return "PCM_STOP";
	case VIRTIO_SND_R_CHMAP_INFO: return "CHMAP_INFO";
	default: return "?";
	}
}

const char *virtio_snd_event_name(uint32_t code)
{
	switch (code) {
	case VIRTIO_SND_EVT_JACK_CONNECTED: return "JACK_CONNECTED";
	case VIRTIO_SND_EVT_JACK_DISCONNECTED: return "JACK_DISCONNECTED";
	case VIRTIO_SND_EVT_PCM_PERIOD_ELAPSED: return "PCM_PERIOD_ELAPSED";
	case VIRTIO_SND_EVT_PCM_XRUN: return "PCM_XRUN";
	case VIRTIO_SND_EVT_CTL_NOTIFY: return "CTL_NOTIFY";
	default: return "?";
	}
}

static const char *const g_virtio_snd_format_names[VIRTIO_SND_PCM_FMT_COUNT] = {
	"IMA_ADPCM", "MU_LAW", "A_LAW", "S8", "U8", "S16", "U16", "S18_3", "U18_3", "S20_3",
	"U20_3", "S24_3", "U24_3", "S20", "U20", "S24", "U24", "S32", "U32", "FLOAT",
	"FLOAT64", "DSD_U8", "DSD_U16", "DSD_U32", "IEC958_SUBFRAME"
};

const char *virtio_snd_format_name(uint32_t format)
{
	return format < VIRTIO_SND_PCM_FMT_COUNT ? g_virtio_snd_format_names[format] : "?";
}

static const uint32_t g_virtio_snd_rate_hz[VIRTIO_SND_PCM_RATE_COUNT] = {
	5512U, 8000U, 11025U, 16000U, 22050U, 32000U, 44100U, 48000U, 64000U, 88200U, 96000U, 176400U, 192000U, 384000U
};

static const char *const g_virtio_snd_rate_names[VIRTIO_SND_PCM_RATE_COUNT] = {
	"5512", "8000", "11025", "16000", "22050", "32000", "44100", "48000", "64000", "88200", "96000", "176400", "192000", "384000"
};

const char *virtio_snd_rate_name(uint32_t rate)
{
	return rate < VIRTIO_SND_PCM_RATE_COUNT ? g_virtio_snd_rate_names[rate] : "?";
}

static const char *const g_virtio_snd_chmap_names[VIRTIO_SND_CHMAP_COUNT] = {
	"NONE", "NA", "MONO", "FL", "FR", "RL", "RR", "FC", "LFE", "SL", "SR", "RC", "FLC", "FRC", "RLC", "RRC",
	"FLW", "FRW", "FLH", "FCH", "FRH", "TC", "TFL", "TFR", "TFC", "TRL", "TRR", "TRC", "TFLC", "TFRC", "TSL", "TSR",
	"LLFE", "RLFE", "BC", "BLC", "BRC"
};

const char *virtio_snd_chmap_name(uint32_t position)
{
	return position < VIRTIO_SND_CHMAP_COUNT ? g_virtio_snd_chmap_names[position] : "?";
}

const char *virtio_snd_direction_name(uint32_t direction)
{
	switch (direction) {
	case VIRTIO_SND_D_OUTPUT: return "output";
	case VIRTIO_SND_D_INPUT: return "input";
	default: return "?";
	}
}

const char *virtio_snd_pcm_state_name(virtio_snd_pcm_state_t state)
{
	switch (state) {
	case VIRTIO_SND_PCM_STATE_UNINIT: return "UNINIT";
	case VIRTIO_SND_PCM_STATE_PARAMS_SET: return "PARAMS_SET";
	case VIRTIO_SND_PCM_STATE_PREPARED: return "PREPARED";
	case VIRTIO_SND_PCM_STATE_STARTED: return "STARTED";
	case VIRTIO_SND_PCM_STATE_STOPPED: return "STOPPED";
	case VIRTIO_SND_PCM_STATE_RELEASED: return "RELEASED";
	default: return "?";
	}
}

const char *virtio_snd_pcm_op_name(virtio_snd_pcm_op_t op)
{
	switch (op) {
	case VIRTIO_SND_PCM_OP_SET_PARAMS: return "SET_PARAMS";
	case VIRTIO_SND_PCM_OP_PREPARE: return "PREPARE";
	case VIRTIO_SND_PCM_OP_START: return "START";
	case VIRTIO_SND_PCM_OP_STOP: return "STOP";
	case VIRTIO_SND_PCM_OP_RELEASE: return "RELEASE";
	default: return "?";
	}
}

uint32_t virtio_snd_format_bytes(uint32_t format)
{
	switch (format) {
	case VIRTIO_SND_PCM_FMT_S8:
	case VIRTIO_SND_PCM_FMT_U8:
		return 1U;
	case VIRTIO_SND_PCM_FMT_S16:
	case VIRTIO_SND_PCM_FMT_U16:
		return 2U;
	case VIRTIO_SND_PCM_FMT_S24_3:
	case VIRTIO_SND_PCM_FMT_U24_3:
		return 3U;
	case VIRTIO_SND_PCM_FMT_S24:
	case VIRTIO_SND_PCM_FMT_U24:
	case VIRTIO_SND_PCM_FMT_S32:
	case VIRTIO_SND_PCM_FMT_U32:
	case VIRTIO_SND_PCM_FMT_FLOAT:
		return 4U;
	case VIRTIO_SND_PCM_FMT_FLOAT64:
		return 8U;
	default:
		return 0U;
	}
}

uint32_t virtio_snd_rate_hz(uint32_t rate)
{
	return rate < VIRTIO_SND_PCM_RATE_COUNT ? g_virtio_snd_rate_hz[rate] : 0U;
}

bool virtio_snd_rate_from_hz(uint32_t hz, uint32_t *rate)
{
	for (uint32_t index = 0U; index < VIRTIO_SND_PCM_RATE_COUNT; index++) {
		if (g_virtio_snd_rate_hz[index] != hz) continue;

		if (rate != 0) *rate = index;
		return true;
	}

	return false;
}

static bool virtio_snd_has_format(uint64_t map, uint32_t format)
{
	return format < VIRTIO_SND_PCM_FMT_COUNT && (map & (1ULL << format)) != 0ULL;
}

static bool virtio_snd_has_rate(uint64_t map, uint32_t rate)
{
	return rate < VIRTIO_SND_PCM_RATE_COUNT && (map & (1ULL << rate)) != 0ULL;
}

bool virtio_snd_pcm_supports(const virtio_snd_pcm_info_t *info, const virtio_snd_pcm_format_t *format)
{
	if (info == 0 || format == 0) return false;
	if (format->channels == 0U || format->channels < info->channels_min || format->channels > info->channels_max) return false;
	if (virtio_snd_format_bytes(format->format) == 0U) return false;

	return virtio_snd_has_format(info->formats, format->format) && virtio_snd_has_rate(info->rates, format->rate);
}

/*
 * The formats the driver falls back on, best first: 16-bit is what the WAV
 * layer converts everything to, then the wider ones it can also produce, then
 * the 8-bit pair every device in practice offers.
 */
static const uint8_t g_virtio_snd_format_preference[] = {
	VIRTIO_SND_PCM_FMT_S16,
	VIRTIO_SND_PCM_FMT_S32,
	VIRTIO_SND_PCM_FMT_FLOAT,
	VIRTIO_SND_PCM_FMT_U8,
	VIRTIO_SND_PCM_FMT_S8
};

bool virtio_snd_pcm_pick(const virtio_snd_pcm_info_t *info, const virtio_snd_pcm_format_t *wanted, virtio_snd_pcm_format_t *picked)
{
	if (info == 0 || wanted == 0 || picked == 0 || info->channels_min == 0U || info->channels_min > info->channels_max) return false;

	virtio_snd_pcm_format_t result = *wanted;

	if (result.channels < info->channels_min) result.channels = info->channels_min;
	if (result.channels > info->channels_max) result.channels = info->channels_max;

	if (!virtio_snd_has_format(info->formats, result.format) || virtio_snd_format_bytes(result.format) == 0U) {
		bool found = false;

		for (uint32_t index = 0U; index < sizeof(g_virtio_snd_format_preference); index++) {
			if (!virtio_snd_has_format(info->formats, g_virtio_snd_format_preference[index])) continue;

			result.format = g_virtio_snd_format_preference[index];
			found = true;
			break;
		}

		if (!found) return false;
	}

	if (!virtio_snd_has_rate(info->rates, result.rate)) {
		bool found = false;

		/* The nearest rate at or above the one asked for keeps the audio's pitch and detail; only then fall back downwards. */
		for (uint32_t rate = result.rate; rate < VIRTIO_SND_PCM_RATE_COUNT && !found; rate++) {
			if (virtio_snd_has_rate(info->rates, rate)) {
				result.rate = (uint8_t)rate;
				found = true;
			}
		}

		uint32_t start = result.rate < VIRTIO_SND_PCM_RATE_COUNT ? result.rate : VIRTIO_SND_PCM_RATE_COUNT;

		for (uint32_t rate = start; rate != 0U && !found; rate--) {
			if (virtio_snd_has_rate(info->rates, rate - 1U)) {
				result.rate = (uint8_t)(rate - 1U);
				found = true;
			}
		}

		if (!found) return false;
	}

	*picked = result;
	return true;
}

/*
 * Text output without a formatter, so the same code runs in the kernel and
 * on the host: append `text` to buffer at *length and keep it terminated.
 */
static void virtio_snd_append(char *buffer, size_t capacity, size_t *length, const char *text)
{
	if (capacity == 0U) return;

	while (*text != '\0' && *length + 1U < capacity) buffer[(*length)++] = *text++;

	buffer[*length] = '\0';
}

static void virtio_snd_append_number(char *buffer, size_t capacity, size_t *length, uint32_t value)
{
	char digits[11];
	size_t count = 0U;

	do {
		digits[count++] = (char)('0' + value % 10U);
		value /= 10U;
	} while (value != 0U);

	char text[12];
	size_t used = 0U;

	while (count != 0U) text[used++] = digits[--count];
	text[used] = '\0';
	virtio_snd_append(buffer, capacity, length, text);
}

size_t virtio_snd_describe_formats(uint64_t formats, char *buffer, size_t capacity)
{
	size_t length = 0U;

	if (capacity != 0U) buffer[0] = '\0';

	for (uint32_t bit = 0U; bit < 64U; bit++) {
		if ((formats & (1ULL << bit)) == 0ULL) continue;

		if (length != 0U) virtio_snd_append(buffer, capacity, &length, " ");

		if (bit < VIRTIO_SND_PCM_FMT_COUNT) {
			virtio_snd_append(buffer, capacity, &length, virtio_snd_format_name(bit));
		} else {
			virtio_snd_append(buffer, capacity, &length, "?");
			virtio_snd_append_number(buffer, capacity, &length, bit);
		}
	}

	return length;
}

size_t virtio_snd_describe_rates(uint64_t rates, char *buffer, size_t capacity)
{
	size_t length = 0U;

	if (capacity != 0U) buffer[0] = '\0';

	for (uint32_t bit = 0U; bit < 64U; bit++) {
		if ((rates & (1ULL << bit)) == 0ULL) continue;

		if (length != 0U) virtio_snd_append(buffer, capacity, &length, " ");

		if (bit < VIRTIO_SND_PCM_RATE_COUNT) {
			virtio_snd_append(buffer, capacity, &length, virtio_snd_rate_name(bit));
		} else {
			virtio_snd_append(buffer, capacity, &length, "?");
			virtio_snd_append_number(buffer, capacity, &length, bit);
		}
	}

	return length;
}

size_t virtio_snd_describe_positions(const uint8_t *positions, uint32_t channels, char *buffer, size_t capacity)
{
	size_t length = 0U;

	if (capacity != 0U) buffer[0] = '\0';
	if (positions == 0) return 0U;

	if (channels > VIRTIO_SND_CHMAP_MAX_SIZE) channels = VIRTIO_SND_CHMAP_MAX_SIZE;

	for (uint32_t index = 0U; index < channels; index++) {
		if (index != 0U) virtio_snd_append(buffer, capacity, &length, " ");

		if (positions[index] < VIRTIO_SND_CHMAP_COUNT) {
			virtio_snd_append(buffer, capacity, &length, virtio_snd_chmap_name(positions[index]));
		} else {
			virtio_snd_append(buffer, capacity, &length, "?");
			virtio_snd_append_number(buffer, capacity, &length, positions[index]);
		}
	}

	return length;
}
