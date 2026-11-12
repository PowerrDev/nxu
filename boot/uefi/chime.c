/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        boot/uefi/chime.c
 *
 * The boot chime, played by the loader.
 *
 * The chime marks the display coming up: efi_main plays it only once the
 * firmware's display is the loader's (main.c, display_start), and not at all
 * on a machine with no display.
 *
 * NBL reads \NXU\chime.wav from the boot volume and drives the VirtIO Sound
 * device itself: the firmware has no sound driver. The whole chime is
 * converted to 16-bit stereo, queued on the device's transmit queue in one
 * go and started; the loader does not wait for it. The device then plays
 * from the queued memory on its own while the kernel is loaded and starts.
 *
 * What the kernel is told, and what it must leave alone until the chime is
 * over:
 *
 *   - the length of the chime, as the boot argument nbl.chime=<milliseconds>;
 *     the kernel plays no chime of its own and does not reset the sound
 *     device before that time has passed
 *   - the memory holding the queues and the samples: one EfiLoaderData
 *     allocation, which the x86 handoff's memory map marks reserved and the
 *     arm64 Device Tree lists in its memory reservation block
 *
 * The device is told not to interrupt (the loader polls the control queue
 * and nobody collects the transmit queue), so it raises nothing on a line
 * the kernel has no handler on yet.
 */

#include "nbl.h"

#include <drivers/virtio/virtio_snd.h>
#include <libk/wav.h>

#define PAGE_SIZE 4096ULL

#define VIRTIO_STATUS_ACKNOWLEDGE 0x01U
#define VIRTIO_STATUS_DRIVER 0x02U
#define VIRTIO_STATUS_DRIVER_OK 0x04U
#define VIRTIO_STATUS_FEATURES_OK 0x08U
#define VIRTIO_F_VERSION_1 (1ULL << 32)

#define VIRTQ_DESC_F_NEXT 1U
#define VIRTQ_DESC_F_WRITE 2U
#define VIRTQ_AVAIL_F_NO_INTERRUPT 1U

/* Queue sizes the loader asks for; a device offering less gets what it offers. */
#define QUEUE_SIZE_MAX 64U

/* The longest chime queued: a boot sound, not a song. */
#define CHIME_BYTES_MAX (16U * 1024U * 1024U)

/* How long a control request may take: 2 s in 100 us steps. */
#define CONTROL_POLLS 20000U

/* Streams asked about when looking for one to play on. */
#define STREAMS_MAX 16U

/* The pages at the head of the allocation; the samples follow. */
enum {
	PAGE_CONTROL_DESC,
	PAGE_CONTROL_AVAIL,
	PAGE_CONTROL_USED,
	PAGE_TX_DESC,
	PAGE_TX_AVAIL,
	PAGE_TX_USED,
	PAGE_CONTROL_MESSAGE,
	PAGE_SAMPLES
};

#define CONTROL_RESPONSE_OFFSET 2048U

typedef struct {
	uint64_t address;
	uint32_t length;
	uint16_t flags;
	uint16_t next;
} virtq_desc;

typedef struct {
	uint16_t flags;
	uint16_t index;
	uint16_t ring[QUEUE_SIZE_MAX];
} virtq_avail;

typedef struct {
	uint16_t flags;
	uint16_t index;
	struct {
		uint32_t id;
		uint32_t length;
	} ring[QUEUE_SIZE_MAX];
} virtq_used;

static uint64_t region_base;
static uint64_t region_size;

static nbl_virtio sound;
static uint16_t control_size;
static uint16_t control_sent;

void
nbl_chime_region(uint64_t *base, uint64_t *size)
{
	*base = region_base;
	*size = region_size;
}

static void *
region_page(unsigned page)
{
	return (void *)(uintptr_t)(region_base + page * PAGE_SIZE);
}

/* The index of a sample rate in the device's rate bitmap, or -1. */
static int
rate_index(uint32_t rate)
{
	static const uint32_t rates[VIRTIO_SND_PCM_RATE_COUNT] = {5512,	 8000,	11025, 16000, 22050,  32000,  44100,
															   48000, 64000, 88200, 96000, 176400, 192000, 384000};

	for (int index = 0; index < (int)VIRTIO_SND_PCM_RATE_COUNT; index++) {
		if (rates[index] == rate) {
			return index;
		}
	}

	return -1;
}

/*
 * Routine:     control
 * Purpose:
 *              Send one request on the control queue and wait for the
 *              device's answer. True when the device answered OK.
 */
static bool
control(const void *request, uint32_t request_size, uint32_t response_size)
{
	uint8_t *message = region_page(PAGE_CONTROL_MESSAGE);
	virtq_desc *descriptors = region_page(PAGE_CONTROL_DESC);
	volatile virtq_avail *avail = region_page(PAGE_CONTROL_AVAIL);
	volatile virtq_used *used = region_page(PAGE_CONTROL_USED);

	nbl_copy(message, request, request_size);
	nbl_zero(message + CONTROL_RESPONSE_OFFSET, response_size);
	descriptors[0] = (virtq_desc){(uint64_t)(uintptr_t)message, request_size, VIRTQ_DESC_F_NEXT, 1};
	descriptors[1] =
		(virtq_desc){(uint64_t)(uintptr_t)message + CONTROL_RESPONSE_OFFSET, response_size, VIRTQ_DESC_F_WRITE, 0};
	avail->ring[control_sent % control_size] = 0;
	__sync_synchronize();
	avail->index = (uint16_t)(control_sent + 1);
	__sync_synchronize();
	control_sent++;
	nbl_virtio_queue_notify(&sound, VIRTIO_SND_VQ_CONTROL);

	for (unsigned poll = 0; used->index != control_sent; poll++) {
		if (poll == CONTROL_POLLS) {
			return false;
		}

		bs->stall(100);
	}

	return ((const virtio_snd_hdr_t *)(message + CONTROL_RESPONSE_OFFSET))->code == VIRTIO_SND_S_OK;
}

static bool
stream_request(uint32_t code, uint32_t stream)
{
	virtio_snd_pcm_hdr_t request = {{code}, stream};

	return control(&request, sizeof(request), sizeof(virtio_snd_hdr_t));
}

/*
 * Routine:     negotiate
 * Purpose:
 *              Reset the device and take it to FEATURES_OK as a VirtIO 1
 *              driver that uses no optional feature.
 */
static bool
negotiate(void)
{
	nbl_virtio_set_status(&sound, 0);
	for (unsigned poll = 0; nbl_virtio_status(&sound) != 0; poll++) {
		if (poll == 1000) {
			return false;
		}

		bs->stall(100);
	}

	nbl_virtio_set_status(&sound, VIRTIO_STATUS_ACKNOWLEDGE);
	nbl_virtio_set_status(&sound, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
	if (!(nbl_virtio_device_features(&sound) & VIRTIO_F_VERSION_1)) {
		return false;
	}

	nbl_virtio_set_driver_features(&sound, VIRTIO_F_VERSION_1);
	nbl_virtio_set_status(&sound, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);
	return (nbl_virtio_status(&sound) & VIRTIO_STATUS_FEATURES_OK) != 0;
}

/* Publish one queue whose three parts sit in consecutive pages from first_page. */
static void
queue_enable(uint16_t queue, uint16_t size, unsigned first_page)
{
	volatile virtq_avail *avail = region_page(first_page + 1);

	avail->flags = VIRTQ_AVAIL_F_NO_INTERRUPT;
	nbl_virtio_queue_enable(&sound, queue, size, (uint64_t)(uintptr_t)region_page(first_page),
							(uint64_t)(uintptr_t)region_page(first_page + 1),
							(uint64_t)(uintptr_t)region_page(first_page + 2));
}

/*
 * Routine:     find_stream
 * Purpose:
 *              The first output stream that takes 16-bit stereo at this
 *              rate, from the device's own description of its streams.
 */
static bool
find_stream(uint32_t rate, uint32_t *stream)
{
	uint32_t count = nbl_virtio_config32(&sound, VIRTIO_SND_CONFIG_STREAMS);

	if (count > STREAMS_MAX) {
		count = STREAMS_MAX;
	}

	if (!count) {
		return false;
	}

	virtio_snd_query_info_t query = {{VIRTIO_SND_R_PCM_INFO}, 0, count, sizeof(virtio_snd_pcm_info_t)};

	if (!control(&query, sizeof(query), sizeof(virtio_snd_hdr_t) + count * sizeof(virtio_snd_pcm_info_t))) {
		return false;
	}

	const uint8_t *items =
		(const uint8_t *)region_page(PAGE_CONTROL_MESSAGE) + CONTROL_RESPONSE_OFFSET + sizeof(virtio_snd_hdr_t);

	for (uint32_t index = 0; index < count; index++) {
		virtio_snd_pcm_info_t info;

		nbl_copy(&info, items + index * sizeof(info), sizeof(info));
		if (info.direction == VIRTIO_SND_D_OUTPUT && (info.formats & (1ULL << VIRTIO_SND_PCM_FMT_S16)) &&
			(info.rates & (1ULL << rate)) && info.channels_min <= 2 && info.channels_max >= 2) {
			*stream = index;
			return true;
		}
	}

	return false;
}

/*
 * Routine:     start
 * Purpose:
 *              Bring the device up, queue every sample and start the stream.
 *              `wav` is the parsed chime; the region is already allocated
 *              with room for `messages` records of `chunk_frames` frames.
 */
static bool
start(const wav_info_t *wav, int rate, uint32_t messages, uint32_t chunk_frames)
{
	if (!negotiate()) {
		nbl_log("nbl_chime_play: the sound device is not a VirtIO 1 device");
		return false;
	}

	queue_enable(VIRTIO_SND_VQ_CONTROL, control_size, PAGE_CONTROL_DESC);
	queue_enable(VIRTIO_SND_VQ_TX, (uint16_t)(messages * 2U), PAGE_TX_DESC);
	nbl_virtio_set_status(&sound, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK |
									  VIRTIO_STATUS_DRIVER_OK);

	uint32_t stream;

	if (!find_stream((uint32_t)rate, &stream)) {
		nbl_log("nbl_chime_play: no output stream takes the chime's format");
		return false;
	}

	uint32_t chunk_bytes = chunk_frames * 4U;
	virtio_snd_pcm_set_params_t params = {
		{{VIRTIO_SND_R_PCM_SET_PARAMS}, stream}, chunk_bytes * messages, chunk_bytes, 0, 2,
		VIRTIO_SND_PCM_FMT_S16,					 (uint8_t)rate,			 0};

	if (!control(&params, sizeof(params), sizeof(virtio_snd_hdr_t)) ||
		!stream_request(VIRTIO_SND_R_PCM_PREPARE, stream)) {
		nbl_log("nbl_chime_play: the sound device refused the stream parameters");
		return false;
	}

	/* One record per message: the stream ID, the samples, then the status the device writes back. */
	uint32_t record_bytes = (uint32_t)(sizeof(virtio_snd_pcm_xfer_t) + chunk_bytes + sizeof(virtio_snd_pcm_status_t));
	uint8_t *records = region_page(PAGE_SAMPLES);
	virtq_desc *descriptors = region_page(PAGE_TX_DESC);
	volatile virtq_avail *avail = region_page(PAGE_TX_AVAIL);
	uint64_t frame = 0;
	uint32_t queued = 0;

	while (queued < messages && frame < wav->frames) {
		uint8_t *record = records + (uint64_t)queued * record_bytes;
		virtio_snd_pcm_xfer_t header = {stream};
		uint32_t got =
			(uint32_t)wav_convert_s16_stereo(wav, frame, (int16_t *)(void *)(record + sizeof(header)), chunk_frames);

		if (!got) {
			break;
		}

		nbl_copy(record, &header, sizeof(header));
		descriptors[queued * 2U] = (virtq_desc){(uint64_t)(uintptr_t)record, (uint32_t)sizeof(header) + got * 4U,
												 VIRTQ_DESC_F_NEXT, (uint16_t)(queued * 2U + 1U)};
		descriptors[queued * 2U + 1U] =
			(virtq_desc){(uint64_t)(uintptr_t)(record + sizeof(header) + chunk_bytes),
						 sizeof(virtio_snd_pcm_status_t), VIRTQ_DESC_F_WRITE, 0};
		avail->ring[queued] = (uint16_t)(queued * 2U);
		frame += got;
		queued++;
	}

	if (frame != wav->frames) {
		return false;
	}

	__sync_synchronize();
	avail->index = (uint16_t)queued;
	__sync_synchronize();
	nbl_virtio_queue_notify(&sound, VIRTIO_SND_VQ_TX);

	if (!stream_request(VIRTIO_SND_R_PCM_START, stream)) {
		nbl_log("nbl_chime_play: the sound device did not start the stream");
		return false;
	}

	return true;
}

uint32_t
nbl_chime_play(handle image)
{
	void *file = 0;
	uintn file_size = 0;

	if (EFI_ERROR(nbl_read_file(image, (char16 *)u"\\NXU\\chime.wav", &file, &file_size))) {
		nbl_log("nbl_chime_play: no chime on the boot volume");
		return 0;
	}

	uint32_t milliseconds = 0;
	wav_info_t wav;
	int rate = -1;

	if (wav_parse(file, file_size, &wav) != WAV_OK || wav.frames == 0 || wav.frames > CHIME_BYTES_MAX / 4U ||
		(rate = rate_index(wav.sample_rate)) < 0) {
		nbl_log("nbl_chime_play: chime.wav is not a sound the loader can play");
		goto out;
	}

	if (!nbl_virtio_find_sound(&sound)) {
		nbl_log("nbl_chime_play: no VirtIO Sound device");
		goto out;
	}

	/*
	 * Every message takes two descriptors, so the transmit queue bounds how
	 * many pieces the chime is cut into; the pieces are sized to fit it.
	 */
	uint16_t tx_max = nbl_virtio_queue_max(&sound, VIRTIO_SND_VQ_TX);

	control_size = nbl_virtio_queue_max(&sound, VIRTIO_SND_VQ_CONTROL);
	control_sent = 0;
	if (control_size > QUEUE_SIZE_MAX) {
		control_size = QUEUE_SIZE_MAX;
	}

	if (tx_max > QUEUE_SIZE_MAX) {
		tx_max = QUEUE_SIZE_MAX;
	}

	/* Queue sizes are powers of two: round what the device offers down to one. */
	while (control_size & (control_size - 1)) {
		control_size &= (uint16_t)(control_size - 1);
	}

	while (tx_max & (tx_max - 1)) {
		tx_max &= (uint16_t)(tx_max - 1);
	}

	if (control_size < 2 || tx_max < 2) {
		nbl_log("nbl_chime_play: the sound device has no usable queues");
		goto out;
	}

	uint32_t messages = tx_max / 2U;
	uint32_t chunk_frames = (uint32_t)((wav.frames + messages - 1U) / messages);
	uint64_t record_bytes = sizeof(virtio_snd_pcm_xfer_t) + (uint64_t)chunk_frames * 4U + sizeof(virtio_snd_pcm_status_t);
	uint64_t pages = PAGE_SAMPLES + (record_bytes * messages + PAGE_SIZE - 1) / PAGE_SIZE;

	/* Below 4 GiB, where every kernel NBL loads can leave it be and the i386 one can describe it. */
	uint64_t address = 0xFFFFFFFFULL;

	if (EFI_ERROR(bs->allocate_pages(1, 2, pages, &address))) {
		nbl_log("nbl_chime_play: no memory for the chime");
		goto out;
	}

	region_base = address;
	region_size = pages * PAGE_SIZE;
	nbl_zero(region_page(0), PAGE_SAMPLES * PAGE_SIZE);

	if (start(&wav, rate, messages, chunk_frames)) {
		milliseconds = (uint32_t)(wav.frames * 1000ULL / wav.sample_rate);
		if (!milliseconds) {
			milliseconds = 1;
		}

		nbl_log("nbl_chime_play: boot chime playing");
	} else {
		/* Nothing may keep using the memory: reset the device before giving it back. */
		nbl_virtio_set_status(&sound, 0);
		bs->free_pages(region_base, pages);
		region_base = 0;
		region_size = 0;
	}

out:
	bs->free_pool(file);
	return milliseconds;
}
