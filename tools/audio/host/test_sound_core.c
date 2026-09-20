/*
 * Host test for the pure half of the VirtIO Sound driver
 * (drivers/virtio/virtio_sound_core.c): feature negotiation, the stream state
 * machine, format and rate tables and the parameter negotiation. Built and run
 * natively with ASan and UBSan by tools/audio/test_host.sh.
 */

#include <drivers/virtio/virtio_features.h>
#include <drivers/virtio/virtio_snd.h>
#include <drivers/virtio/virtio_sound_core.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned int g_checks;
static unsigned int g_failures;

#define CHECK(condition) \
	do { \
		g_checks++; \
		if (!(condition)) { \
			g_failures++; \
			printf("FAIL  %s:%d: %s\n", __FILE__, __LINE__, #condition); \
		} \
	} while (0)

/* ---- feature negotiation ---------------------------------------------- */

static void test_negotiation(void)
{
	uint64_t accepted = 0xFFFFFFFFFFFFFFFFULL;

	/* No VERSION_1: the device is refused, whatever else it offers. */
	CHECK(!virtio_snd_negotiate(0ULL, &accepted));
	CHECK(accepted == 0ULL);

	accepted = 0xFFFFFFFFFFFFFFFFULL;
	CHECK(!virtio_snd_negotiate(1ULL << VIRTIO_SND_F_CTLS, &accepted));
	CHECK(accepted == 0ULL);

	accepted = 0xFFFFFFFFFFFFFFFFULL;
	CHECK(!virtio_snd_negotiate(~(1ULL << VIRTIO_F_VERSION_1), &accepted));
	CHECK(accepted == 0ULL);

	/* A NULL result is allowed. */
	CHECK(!virtio_snd_negotiate(0ULL, 0));
	CHECK(virtio_snd_negotiate(1ULL << VIRTIO_F_VERSION_1, 0));

	/* VERSION_1 alone. */
	CHECK(virtio_snd_negotiate(1ULL << VIRTIO_F_VERSION_1, &accepted));
	CHECK(accepted == (1ULL << VIRTIO_F_VERSION_1));

	/* Everything on offer: only VERSION_1 is taken. */
	CHECK(virtio_snd_negotiate(0xFFFFFFFFFFFFFFFFULL, &accepted));
	CHECK(accepted == (1ULL << VIRTIO_F_VERSION_1));

	/* The transport bits QEMU really offers. */
	uint64_t qemu = (1ULL << VIRTIO_F_NOTIFY_ON_EMPTY) | (1ULL << VIRTIO_F_ANY_LAYOUT) | (1ULL << VIRTIO_F_RING_INDIRECT_DESC) | (1ULL << VIRTIO_F_RING_EVENT_IDX) | (1ULL << VIRTIO_F_VERSION_1) | (1ULL << VIRTIO_F_RING_RESET);

	CHECK(virtio_snd_negotiate(qemu, &accepted));
	CHECK(accepted == (1ULL << VIRTIO_F_VERSION_1));

	/* Packed rings and in-order use would change the queue layout the driver builds. */
	CHECK(virtio_snd_negotiate((1ULL << VIRTIO_F_VERSION_1) | (1ULL << VIRTIO_F_RING_PACKED) | (1ULL << VIRTIO_F_IN_ORDER), &accepted));
	CHECK((accepted & (1ULL << VIRTIO_F_RING_PACKED)) == 0ULL);
	CHECK((accepted & (1ULL << VIRTIO_F_IN_ORDER)) == 0ULL);

	CHECK(strcmp(virtio_snd_feature_name(VIRTIO_F_VERSION_1), "F_VERSION_1") == 0);
	CHECK(strcmp(virtio_snd_feature_name(VIRTIO_F_NOTIFY_ON_EMPTY), "F_NOTIFY_ON_EMPTY") == 0);
	CHECK(strcmp(virtio_snd_feature_name(63U), "unknown") == 0);
	CHECK(strcmp(virtio_snd_feature_name(1000U), "unknown") == 0);
}

/* ---- the stream state machine ----------------------------------------- */

/* Rows are states, columns the operations, in enum order; the value is the next state or -1. */
static const int g_expected[6][5] = {
	/* SET_PARAMS  PREPARE  START  STOP  RELEASE */
	/* UNINIT     */ { VIRTIO_SND_PCM_STATE_PARAMS_SET, -1, -1, -1, -1 },
	/* PARAMS_SET */ { VIRTIO_SND_PCM_STATE_PARAMS_SET, VIRTIO_SND_PCM_STATE_PREPARED, -1, -1, -1 },
	/* PREPARED   */ { VIRTIO_SND_PCM_STATE_PARAMS_SET, -1, VIRTIO_SND_PCM_STATE_STARTED, -1, VIRTIO_SND_PCM_STATE_RELEASED },
	/* STARTED    */ { -1, -1, -1, VIRTIO_SND_PCM_STATE_STOPPED, -1 },
	/* STOPPED    */ { -1, -1, VIRTIO_SND_PCM_STATE_STARTED, -1, VIRTIO_SND_PCM_STATE_RELEASED },
	/* RELEASED   */ { VIRTIO_SND_PCM_STATE_PARAMS_SET, VIRTIO_SND_PCM_STATE_PREPARED, -1, -1, -1 }
};

static void test_state_machine(void)
{
	for (int state = 0; state < 6; state++) {
		for (int op = 0; op < 5; op++) {
			virtio_snd_pcm_state_t next = (virtio_snd_pcm_state_t)99;
			bool legal = virtio_snd_pcm_transition((virtio_snd_pcm_state_t)state, (virtio_snd_pcm_op_t)op, &next);

			if (g_expected[state][op] < 0) {
				CHECK(!legal);
				CHECK((int)next == 99); /* an illegal request leaves the result alone */
			} else {
				CHECK(legal);
				CHECK((int)next == g_expected[state][op]);
			}

			/* The result pointer is optional. */
			CHECK(virtio_snd_pcm_transition((virtio_snd_pcm_state_t)state, (virtio_snd_pcm_op_t)op, 0) == (g_expected[state][op] >= 0));
		}
	}

	/* Values outside the enumerations are refused, not crashed on. */
	virtio_snd_pcm_state_t next = VIRTIO_SND_PCM_STATE_UNINIT;

	CHECK(!virtio_snd_pcm_transition(VIRTIO_SND_PCM_STATE_PREPARED, (virtio_snd_pcm_op_t)5, &next));
	CHECK(!virtio_snd_pcm_transition(VIRTIO_SND_PCM_STATE_PREPARED, (virtio_snd_pcm_op_t)-1, &next));
	CHECK(!virtio_snd_pcm_transition((virtio_snd_pcm_state_t)6, VIRTIO_SND_PCM_OP_START, &next));
	CHECK(!virtio_snd_pcm_transition((virtio_snd_pcm_state_t)-1, VIRTIO_SND_PCM_OP_START, &next));

	/* A full life: set, prepare, start, stop, start again, stop, release, prepare again. */
	virtio_snd_pcm_state_t state = VIRTIO_SND_PCM_STATE_UNINIT;
	static const virtio_snd_pcm_op_t life[] = {
		VIRTIO_SND_PCM_OP_SET_PARAMS, VIRTIO_SND_PCM_OP_PREPARE, VIRTIO_SND_PCM_OP_START, VIRTIO_SND_PCM_OP_STOP,
		VIRTIO_SND_PCM_OP_START, VIRTIO_SND_PCM_OP_STOP, VIRTIO_SND_PCM_OP_RELEASE, VIRTIO_SND_PCM_OP_PREPARE
	};

	for (unsigned int index = 0U; index < sizeof(life) / sizeof(life[0]); index++) {
		CHECK(virtio_snd_pcm_transition(state, life[index], &state));
	}

	CHECK(state == VIRTIO_SND_PCM_STATE_PREPARED);

	/* Starting a stream that is running, or releasing one that is running, is what the device treats as fatal. */
	CHECK(!virtio_snd_pcm_transition(VIRTIO_SND_PCM_STATE_STARTED, VIRTIO_SND_PCM_OP_START, 0));
	CHECK(!virtio_snd_pcm_transition(VIRTIO_SND_PCM_STATE_STARTED, VIRTIO_SND_PCM_OP_RELEASE, 0));
	CHECK(!virtio_snd_pcm_transition(VIRTIO_SND_PCM_STATE_UNINIT, VIRTIO_SND_PCM_OP_START, 0));

	CHECK(virtio_snd_pcm_op_request(VIRTIO_SND_PCM_OP_SET_PARAMS) == VIRTIO_SND_R_PCM_SET_PARAMS);
	CHECK(virtio_snd_pcm_op_request(VIRTIO_SND_PCM_OP_PREPARE) == VIRTIO_SND_R_PCM_PREPARE);
	CHECK(virtio_snd_pcm_op_request(VIRTIO_SND_PCM_OP_START) == VIRTIO_SND_R_PCM_START);
	CHECK(virtio_snd_pcm_op_request(VIRTIO_SND_PCM_OP_STOP) == VIRTIO_SND_R_PCM_STOP);
	CHECK(virtio_snd_pcm_op_request(VIRTIO_SND_PCM_OP_RELEASE) == VIRTIO_SND_R_PCM_RELEASE);
	CHECK(virtio_snd_pcm_op_request((virtio_snd_pcm_op_t)77) == 0U);
}

/* ---- names and tables ------------------------------------------------- */

static void test_names(void)
{
	for (uint32_t format = 0U; format < VIRTIO_SND_PCM_FMT_COUNT; format++) {
		const char *name = virtio_snd_format_name(format);

		CHECK(name != 0 && strcmp(name, "?") != 0);
	}

	CHECK(strcmp(virtio_snd_format_name(VIRTIO_SND_PCM_FMT_S16), "S16") == 0);
	CHECK(strcmp(virtio_snd_format_name(VIRTIO_SND_PCM_FMT_FLOAT), "FLOAT") == 0);
	CHECK(strcmp(virtio_snd_format_name(VIRTIO_SND_PCM_FMT_COUNT), "?") == 0);
	CHECK(strcmp(virtio_snd_format_name(0xFFFFFFFFU), "?") == 0);

	for (uint32_t rate = 0U; rate < VIRTIO_SND_PCM_RATE_COUNT; rate++) {
		CHECK(strcmp(virtio_snd_rate_name(rate), "?") != 0);
		CHECK(virtio_snd_rate_hz(rate) != 0U);
	}

	CHECK(strcmp(virtio_snd_rate_name(VIRTIO_SND_PCM_RATE_44100), "44100") == 0);
	CHECK(virtio_snd_rate_hz(VIRTIO_SND_PCM_RATE_48000) == 48000U);
	CHECK(virtio_snd_rate_hz(VIRTIO_SND_PCM_RATE_COUNT) == 0U);

	for (uint32_t position = 0U; position < VIRTIO_SND_CHMAP_COUNT; position++) {
		CHECK(strcmp(virtio_snd_chmap_name(position), "?") != 0);
	}

	CHECK(strcmp(virtio_snd_chmap_name(VIRTIO_SND_CHMAP_FL), "FL") == 0);
	CHECK(strcmp(virtio_snd_chmap_name(VIRTIO_SND_CHMAP_COUNT), "?") == 0);

	CHECK(strcmp(virtio_snd_status_name(VIRTIO_SND_S_OK), "OK") == 0);
	CHECK(strcmp(virtio_snd_status_name(VIRTIO_SND_S_NOT_SUPP), "NOT_SUPP") == 0);
	CHECK(strcmp(virtio_snd_status_name(0U), "?") == 0);
	CHECK(strcmp(virtio_snd_request_name(VIRTIO_SND_R_PCM_SET_PARAMS), "PCM_SET_PARAMS") == 0);
	CHECK(strcmp(virtio_snd_request_name(0xDEADU), "?") == 0);
	CHECK(strcmp(virtio_snd_event_name(VIRTIO_SND_EVT_PCM_XRUN), "PCM_XRUN") == 0);
	CHECK(strcmp(virtio_snd_event_name(0U), "?") == 0);
	CHECK(strcmp(virtio_snd_direction_name(VIRTIO_SND_D_OUTPUT), "output") == 0);
	CHECK(strcmp(virtio_snd_direction_name(VIRTIO_SND_D_INPUT), "input") == 0);
	CHECK(strcmp(virtio_snd_direction_name(2U), "?") == 0);

	for (int state = 0; state < 6; state++) CHECK(strcmp(virtio_snd_pcm_state_name((virtio_snd_pcm_state_t)state), "?") != 0);
	CHECK(strcmp(virtio_snd_pcm_state_name((virtio_snd_pcm_state_t)9), "?") == 0);
	for (int op = 0; op < 5; op++) CHECK(strcmp(virtio_snd_pcm_op_name((virtio_snd_pcm_op_t)op), "?") != 0);
	CHECK(strcmp(virtio_snd_pcm_op_name((virtio_snd_pcm_op_t)9), "?") == 0);
}

static void test_sample_widths(void)
{
	CHECK(virtio_snd_format_bytes(VIRTIO_SND_PCM_FMT_U8) == 1U);
	CHECK(virtio_snd_format_bytes(VIRTIO_SND_PCM_FMT_S8) == 1U);
	CHECK(virtio_snd_format_bytes(VIRTIO_SND_PCM_FMT_S16) == 2U);
	CHECK(virtio_snd_format_bytes(VIRTIO_SND_PCM_FMT_S24_3) == 3U);
	CHECK(virtio_snd_format_bytes(VIRTIO_SND_PCM_FMT_S24) == 4U);
	CHECK(virtio_snd_format_bytes(VIRTIO_SND_PCM_FMT_S32) == 4U);
	CHECK(virtio_snd_format_bytes(VIRTIO_SND_PCM_FMT_FLOAT) == 4U);
	CHECK(virtio_snd_format_bytes(VIRTIO_SND_PCM_FMT_FLOAT64) == 8U);

	/* Compressed, 18/20-bit and DSD formats cannot be streamed by this driver. */
	CHECK(virtio_snd_format_bytes(VIRTIO_SND_PCM_FMT_IMA_ADPCM) == 0U);
	CHECK(virtio_snd_format_bytes(VIRTIO_SND_PCM_FMT_MU_LAW) == 0U);
	CHECK(virtio_snd_format_bytes(VIRTIO_SND_PCM_FMT_S20) == 0U);
	CHECK(virtio_snd_format_bytes(VIRTIO_SND_PCM_FMT_DSD_U8) == 0U);
	CHECK(virtio_snd_format_bytes(VIRTIO_SND_PCM_FMT_COUNT) == 0U);
	CHECK(virtio_snd_format_bytes(0xFFFFFFFFU) == 0U);

	uint32_t rate = 0U;

	CHECK(virtio_snd_rate_from_hz(44100U, &rate) && rate == VIRTIO_SND_PCM_RATE_44100);
	CHECK(virtio_snd_rate_from_hz(48000U, &rate) && rate == VIRTIO_SND_PCM_RATE_48000);
	CHECK(virtio_snd_rate_from_hz(5512U, &rate) && rate == VIRTIO_SND_PCM_RATE_5512);
	CHECK(virtio_snd_rate_from_hz(384000U, &rate) && rate == VIRTIO_SND_PCM_RATE_384000);
	CHECK(!virtio_snd_rate_from_hz(44101U, &rate));
	CHECK(!virtio_snd_rate_from_hz(0U, &rate));
	CHECK(virtio_snd_rate_from_hz(8000U, 0));

	for (uint32_t index = 0U; index < VIRTIO_SND_PCM_RATE_COUNT; index++) {
		CHECK(virtio_snd_rate_from_hz(virtio_snd_rate_hz(index), &rate) && rate == index);
	}
}

/* ---- parameter negotiation -------------------------------------------- */

/* What QEMU's virtio-sound reports for every stream: all listed rates, the formats it can convert, 1..2 channels. */
static virtio_snd_pcm_info_t qemu_stream(void)
{
	virtio_snd_pcm_info_t info;

	memset(&info, 0, sizeof(info));
	info.formats = (1ULL << VIRTIO_SND_PCM_FMT_S8) | (1ULL << VIRTIO_SND_PCM_FMT_U8) | (1ULL << VIRTIO_SND_PCM_FMT_S16) | (1ULL << VIRTIO_SND_PCM_FMT_U16) | (1ULL << VIRTIO_SND_PCM_FMT_S32) | (1ULL << VIRTIO_SND_PCM_FMT_U32) | (1ULL << VIRTIO_SND_PCM_FMT_FLOAT);

	for (uint32_t rate = 0U; rate < VIRTIO_SND_PCM_RATE_COUNT; rate++) info.rates |= 1ULL << rate;

	info.direction = VIRTIO_SND_D_OUTPUT;
	info.channels_min = 1U;
	info.channels_max = 2U;
	return info;
}

static void test_negotiation_of_parameters(void)
{
	virtio_snd_pcm_info_t info = qemu_stream();
	virtio_snd_pcm_format_t wanted = { .channels = 2U, .format = VIRTIO_SND_PCM_FMT_S16, .rate = VIRTIO_SND_PCM_RATE_44100 };
	virtio_snd_pcm_format_t picked;

	/* S16 stereo at 44.1 kHz and 48 kHz are honoured as they are. */
	CHECK(virtio_snd_pcm_supports(&info, &wanted));
	CHECK(virtio_snd_pcm_pick(&info, &wanted, &picked));
	CHECK(picked.channels == 2U && picked.format == VIRTIO_SND_PCM_FMT_S16 && picked.rate == VIRTIO_SND_PCM_RATE_44100);

	wanted.rate = VIRTIO_SND_PCM_RATE_48000;
	CHECK(virtio_snd_pcm_supports(&info, &wanted));
	CHECK(virtio_snd_pcm_pick(&info, &wanted, &picked));
	CHECK(picked.rate == VIRTIO_SND_PCM_RATE_48000);

	/* Too many channels is clamped, none is raised to the minimum. */
	wanted.channels = 6U;
	CHECK(!virtio_snd_pcm_supports(&info, &wanted));
	CHECK(virtio_snd_pcm_pick(&info, &wanted, &picked) && picked.channels == 2U);
	wanted.channels = 0U;
	CHECK(!virtio_snd_pcm_supports(&info, &wanted));
	CHECK(virtio_snd_pcm_pick(&info, &wanted, &picked) && picked.channels == 1U);
	wanted.channels = 2U;

	/* A format the device lacks falls back to S16, then to what remains. */
	wanted.format = VIRTIO_SND_PCM_FMT_S24;
	CHECK(!virtio_snd_pcm_supports(&info, &wanted));
	CHECK(virtio_snd_pcm_pick(&info, &wanted, &picked) && picked.format == VIRTIO_SND_PCM_FMT_S16);

	wanted.format = VIRTIO_SND_PCM_FMT_S16;
	info.formats = (1ULL << VIRTIO_SND_PCM_FMT_U8) | (1ULL << VIRTIO_SND_PCM_FMT_FLOAT);
	CHECK(virtio_snd_pcm_pick(&info, &wanted, &picked) && picked.format == VIRTIO_SND_PCM_FMT_FLOAT);

	info.formats = 1ULL << VIRTIO_SND_PCM_FMT_U8;
	CHECK(virtio_snd_pcm_pick(&info, &wanted, &picked) && picked.format == VIRTIO_SND_PCM_FMT_U8);

	/* A device with only formats the driver cannot stream has no usable stream. */
	info.formats = (1ULL << VIRTIO_SND_PCM_FMT_IMA_ADPCM) | (1ULL << VIRTIO_SND_PCM_FMT_DSD_U8);
	CHECK(!virtio_snd_pcm_pick(&info, &wanted, &picked));
	info.formats = 0ULL;
	CHECK(!virtio_snd_pcm_pick(&info, &wanted, &picked));

	/* Rates: the nearest at or above the one wanted, else the nearest below. */
	info = qemu_stream();
	info.rates = (1ULL << VIRTIO_SND_PCM_RATE_48000) | (1ULL << VIRTIO_SND_PCM_RATE_96000);
	wanted.rate = VIRTIO_SND_PCM_RATE_44100;
	CHECK(virtio_snd_pcm_pick(&info, &wanted, &picked) && picked.rate == VIRTIO_SND_PCM_RATE_48000);
	wanted.rate = VIRTIO_SND_PCM_RATE_192000;
	CHECK(virtio_snd_pcm_pick(&info, &wanted, &picked) && picked.rate == VIRTIO_SND_PCM_RATE_96000);
	wanted.rate = VIRTIO_SND_PCM_RATE_8000;
	CHECK(virtio_snd_pcm_pick(&info, &wanted, &picked) && picked.rate == VIRTIO_SND_PCM_RATE_48000);
	info.rates = 0ULL;
	CHECK(!virtio_snd_pcm_pick(&info, &wanted, &picked));

	/* A rate code outside the specification. */
	info = qemu_stream();
	wanted.rate = 200U;
	CHECK(!virtio_snd_pcm_supports(&info, &wanted));
	CHECK(virtio_snd_pcm_pick(&info, &wanted, &picked) && picked.rate == VIRTIO_SND_PCM_RATE_384000);

	/* A device that reports a nonsense channel range, or no info at all. */
	info.channels_min = 0U;
	CHECK(!virtio_snd_pcm_pick(&info, &wanted, &picked));
	info.channels_min = 3U;
	info.channels_max = 2U;
	CHECK(!virtio_snd_pcm_pick(&info, &wanted, &picked));
	CHECK(!virtio_snd_pcm_pick(0, &wanted, &picked));
	CHECK(!virtio_snd_pcm_pick(&info, 0, &picked));
	CHECK(!virtio_snd_pcm_pick(&info, &wanted, 0));
	CHECK(!virtio_snd_pcm_supports(0, &wanted));
	CHECK(!virtio_snd_pcm_supports(&info, 0));
}

/* ---- errors and the lists for the log ---------------------------------- */

static void test_errors_and_descriptions(void)
{
	CHECK(virtio_snd_error_from_status(VIRTIO_SND_S_OK) == VIRTIO_SND_E_NONE);
	CHECK(virtio_snd_error_from_status(VIRTIO_SND_S_BAD_MSG) == VIRTIO_SND_E_BAD_MSG);
	CHECK(virtio_snd_error_from_status(VIRTIO_SND_S_NOT_SUPP) == VIRTIO_SND_E_NOT_SUPP);
	CHECK(virtio_snd_error_from_status(VIRTIO_SND_S_IO_ERR) == VIRTIO_SND_E_IO);

	/* A status the specification does not have is a failure, never a success (a zeroed response is not OK). */
	CHECK(virtio_snd_error_from_status(0U) == VIRTIO_SND_E_IO);
	CHECK(virtio_snd_error_from_status(0x8004U) == VIRTIO_SND_E_IO);
	CHECK(virtio_snd_error_from_status(0xFFFFFFFFU) == VIRTIO_SND_E_IO);

	for (int error = VIRTIO_SND_E_NONE; error <= VIRTIO_SND_E_INTERRUPTED; error++) {
		CHECK(strcmp(virtio_snd_error_name((virtio_snd_error_t)error), "?") != 0);
	}

	CHECK(strcmp(virtio_snd_error_name((virtio_snd_error_t)99), "?") == 0);

	char text[256];

	CHECK(virtio_snd_describe_formats(0ULL, text, sizeof(text)) == 0U && text[0] == '\0');
	CHECK(virtio_snd_describe_formats((1ULL << VIRTIO_SND_PCM_FMT_S16) | (1ULL << VIRTIO_SND_PCM_FMT_FLOAT), text, sizeof(text)) == 9U);
	CHECK(strcmp(text, "S16 FLOAT") == 0);
	CHECK(virtio_snd_describe_formats(1ULL << 40, text, sizeof(text)) == 3U && strcmp(text, "?40") == 0);
	CHECK(virtio_snd_describe_rates((1ULL << VIRTIO_SND_PCM_RATE_44100) | (1ULL << VIRTIO_SND_PCM_RATE_48000), text, sizeof(text)) == 11U);
	CHECK(strcmp(text, "44100 48000") == 0);
	CHECK(virtio_snd_describe_rates(1ULL << 63, text, sizeof(text)) == 3U && strcmp(text, "?63") == 0);

	/* Every format at once: fits in the buffer the driver uses. */
	uint64_t all = 0ULL;

	for (uint32_t bit = 0U; bit < VIRTIO_SND_PCM_FMT_COUNT; bit++) all |= 1ULL << bit;
	CHECK(virtio_snd_describe_formats(all, text, sizeof(text)) < sizeof(text));

	/* A buffer that is too small is truncated, terminated and never overrun. */
	char small[8];

	memset(small, 'x', sizeof(small));
	CHECK(virtio_snd_describe_formats(all, small, sizeof(small)) == sizeof(small) - 1U);
	CHECK(small[sizeof(small) - 1U] == '\0');

	char one[1] = { 'x' };

	CHECK(virtio_snd_describe_formats(all, one, sizeof(one)) == 0U && one[0] == '\0');
	CHECK(virtio_snd_describe_rates(all, one, 0U) == 0U);

	static const uint8_t stereo[] = { VIRTIO_SND_CHMAP_FL, VIRTIO_SND_CHMAP_FR };

	CHECK(virtio_snd_describe_positions(stereo, 2U, text, sizeof(text)) == 5U && strcmp(text, "FL FR") == 0);
	CHECK(virtio_snd_describe_positions(stereo, 0U, text, sizeof(text)) == 0U);
	CHECK(virtio_snd_describe_positions(0, 2U, text, sizeof(text)) == 0U);

	/* A channel count beyond the array the device may send is clamped, not read past. */
	uint8_t many[VIRTIO_SND_CHMAP_MAX_SIZE];

	memset(many, VIRTIO_SND_CHMAP_FC, sizeof(many));
	CHECK(virtio_snd_describe_positions(many, 200U, text, sizeof(text)) == VIRTIO_SND_CHMAP_MAX_SIZE * 3U - 1U);

	static const uint8_t odd[] = { 200U };

	CHECK(virtio_snd_describe_positions(odd, 1U, text, sizeof(text)) == 4U && strcmp(text, "?200") == 0);
}

int main(void)
{
	test_negotiation();
	test_state_machine();
	test_names();
	test_sample_widths();
	test_negotiation_of_parameters();
	test_errors_and_descriptions();

	printf("sound core: %u check(s), %u failure(s)\n", g_checks, g_failures);
	return g_failures == 0U ? 0 : 1;
}
