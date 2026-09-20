#ifndef NXU_DRIVERS_VIRTIO_VIRTIO_SOUND_CORE_H
#define NXU_DRIVERS_VIRTIO_VIRTIO_SOUND_CORE_H

#include <drivers/virtio/virtio_snd.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The parts of the VirtIO Sound driver that are pure decisions -- which
 * features to accept, which stream requests are legal in which state, what a
 * format or rate is called, which parameters a stream can be given -- kept
 * free of the kernel so the host test (tools/audio/test_host.sh) can run them
 * natively with sanitizers. The driver (virtio_sound.c) does the I/O.
 */

/*
 * Stream states and the requests that move between them, from the state
 * diagram of section 5.14.6.6. The device treats a request that is illegal in
 * the current state as a fatal driver error and stops answering, so the
 * driver checks here first and never sends one.
 *
 *     request       legal in                       result
 *     SET_PARAMS    UNINIT, PARAMS_SET,            PARAMS_SET
 *                   PREPARED, RELEASED
 *     PREPARE       PARAMS_SET, RELEASED           PREPARED
 *     START         PREPARED, STOPPED              STARTED
 *     STOP          STARTED                        STOPPED
 *     RELEASE       PREPARED, STOPPED              RELEASED
 */
typedef enum {
	VIRTIO_SND_PCM_STATE_UNINIT,
	VIRTIO_SND_PCM_STATE_PARAMS_SET,
	VIRTIO_SND_PCM_STATE_PREPARED,
	VIRTIO_SND_PCM_STATE_STARTED,
	VIRTIO_SND_PCM_STATE_STOPPED,
	VIRTIO_SND_PCM_STATE_RELEASED
} virtio_snd_pcm_state_t;

typedef enum {
	VIRTIO_SND_PCM_OP_SET_PARAMS,
	VIRTIO_SND_PCM_OP_PREPARE,
	VIRTIO_SND_PCM_OP_START,
	VIRTIO_SND_PCM_OP_STOP,
	VIRTIO_SND_PCM_OP_RELEASE
} virtio_snd_pcm_op_t;

/*
 * virtio_snd_pcm_transition:
 *
 * The state a stream in `state` enters when `op` succeeds. Returns false and
 * leaves *next alone when the request is illegal in that state.
 */
bool virtio_snd_pcm_transition(virtio_snd_pcm_state_t state, virtio_snd_pcm_op_t op, virtio_snd_pcm_state_t *next);

/* The control request type that carries `op`. */
uint32_t virtio_snd_pcm_op_request(virtio_snd_pcm_op_t op);

/*
 * virtio_snd_negotiate:
 *
 * Decide which of the device's offered feature bits the driver accepts.
 * VIRTIO_F_VERSION_1 is mandatory: a device without it is refused (false) and
 * *accepted is zero. Everything else is declined -- the sound device defines
 * one optional bit, control elements, which the driver does not use, and the
 * ring and platform features change the queue layout the driver builds.
 */
bool virtio_snd_negotiate(uint64_t offered, uint64_t *accepted);

/*
 * What a request to the device came to. The device reports OK, BAD_MSG,
 * NOT_SUPP or IO_ERR; the driver adds the ways a request can fail before the
 * device has a say (no answer, a device already given up on, a request the
 * state machine forbids, a parameter no stream accepts).
 */
typedef enum {
	VIRTIO_SND_E_NONE,
	VIRTIO_SND_E_BAD_MSG,
	VIRTIO_SND_E_NOT_SUPP,
	VIRTIO_SND_E_IO,
	VIRTIO_SND_E_TIMEOUT,
	VIRTIO_SND_E_DEAD,
	VIRTIO_SND_E_ILLEGAL,
	VIRTIO_SND_E_INVALID,
	VIRTIO_SND_E_NO_MEMORY,
	VIRTIO_SND_E_BUSY,
	VIRTIO_SND_E_AGAIN,
	VIRTIO_SND_E_INTERRUPTED
} virtio_snd_error_t;

/* The outcome a response status code stands for; a code outside the specification is an I/O error. */
virtio_snd_error_t virtio_snd_error_from_status(uint32_t status);
const char *virtio_snd_error_name(virtio_snd_error_t error);

/* Names, "?" for a value outside the specification. */
const char *virtio_snd_feature_name(uint32_t bit);
const char *virtio_snd_status_name(uint32_t status);
const char *virtio_snd_request_name(uint32_t code);
const char *virtio_snd_event_name(uint32_t code);
const char *virtio_snd_format_name(uint32_t format);
const char *virtio_snd_rate_name(uint32_t rate);
const char *virtio_snd_chmap_name(uint32_t position);
const char *virtio_snd_direction_name(uint32_t direction);
const char *virtio_snd_pcm_state_name(virtio_snd_pcm_state_t state);
const char *virtio_snd_pcm_op_name(virtio_snd_pcm_op_t op);

/*
 * Human-readable lists for the log: the names of the formats or rates set in
 * a bit map, or the channel positions of a map, separated by spaces, into
 * buffer (always NUL terminated, truncated to fit). A bit outside the
 * specification is shown as "?<bit>". Return the length written.
 */
size_t virtio_snd_describe_formats(uint64_t formats, char *buffer, size_t capacity);
size_t virtio_snd_describe_rates(uint64_t rates, char *buffer, size_t capacity);
size_t virtio_snd_describe_positions(const uint8_t *positions, uint32_t channels, char *buffer, size_t capacity);

/* Width in bytes of one sample of `format`, or 0 for a format the driver cannot stream (compressed, 18/20-bit, DSD). */
uint32_t virtio_snd_format_bytes(uint32_t format);

/* Frame rate in Hz of a VIRTIO_SND_PCM_RATE_* value, or 0 outside the specification. */
uint32_t virtio_snd_rate_hz(uint32_t rate);

/* The VIRTIO_SND_PCM_RATE_* value for `hz`, or false when the specification has no such rate. */
bool virtio_snd_rate_from_hz(uint32_t hz, uint32_t *rate);

/*
 * What one stream will be asked to do. Sample format and frame rate are the
 * VIRTIO_SND_PCM_FMT_* and VIRTIO_SND_PCM_RATE_* codes.
 */
typedef struct {
	uint8_t channels;
	uint8_t format;
	uint8_t rate;
} virtio_snd_pcm_format_t;

/*
 * virtio_snd_pcm_supports:
 *
 * True when a stream described by `info` (output direction, channel range,
 * format and rate bit maps) accepts `format` as it stands.
 */
bool virtio_snd_pcm_supports(const virtio_snd_pcm_info_t *info, const virtio_snd_pcm_format_t *format);

/*
 * virtio_snd_pcm_pick:
 *
 * Choose the format a stream will really run at for a request of `wanted`.
 * The frame rate and channel count are honoured exactly when the stream has
 * them, else the nearest higher supported rate (or the highest below) and the
 * nearest supported channel count are used. The sample format is `wanted`
 * when supported, else the first of S16, S32, FLOAT, U8, S8 that is (S16
 * first: everything the WAV layer converts to is 16-bit). Returns false when
 * the stream has no usable format or rate at all.
 */
bool virtio_snd_pcm_pick(const virtio_snd_pcm_info_t *info, const virtio_snd_pcm_format_t *wanted, virtio_snd_pcm_format_t *picked);

#endif
