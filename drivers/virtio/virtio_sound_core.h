#ifndef NXU_DRIVERS_VIRTIO_VIRTIO_SOUND_CORE_H
#define NXU_DRIVERS_VIRTIO_VIRTIO_SOUND_CORE_H

#include <drivers/virtio/virtio_snd.h>

#include <stdbool.h>
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
