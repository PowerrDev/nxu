#ifndef NXU_KERN_AUDIO_AUDIO_DEFS_H
#define NXU_KERN_AUDIO_AUDIO_DEFS_H

#include <kern/syscall/syscall_defs.h>

#include <stdint.h>

/*
 * The audio device, /dev/audio0, as user programs see it.
 *
 * Open it for writing (it needs NXU_CAP_AUDIO; one program at a time, a
 * second open is refused with -NXU_SYS_E_BUSY), optionally negotiate the
 * format with NXU_AUDIO_SET_PARAMS, then write PCM samples: interleaved
 * frames, little endian, in the format that was negotiated (16-bit stereo at
 * 44100 Hz until it says otherwise). A write returns when the samples are in
 * the device's ring, sleeping while the ring is full, or returns a short
 * count / -NXU_SYS_E_AGAIN if the descriptor is non-blocking (NXU_O_NONBLOCK
 * or NXU_AUDIO_SET_NONBLOCK). NXU_AUDIO_DRAIN waits until the samples have
 * been played; closing the descriptor without draining cuts what is queued.
 */

#define NXU_AUDIO_DEVICE_PATH "/dev/audio0"

/* Sample formats (little endian). */
#define NXU_AUDIO_FORMAT_U8 1U
#define NXU_AUDIO_FORMAT_S16 2U
#define NXU_AUDIO_FORMAT_S32 3U
#define NXU_AUDIO_FORMAT_F32 4U

/* The stream's state. */
#define NXU_AUDIO_STATE_IDLE 0U
#define NXU_AUDIO_STATE_PREPARED 1U
#define NXU_AUDIO_STATE_RUNNING 2U
#define NXU_AUDIO_STATE_STOPPED 3U

/*
 * The stream's format. In a SET_PARAMS request rate, channels and format say
 * what is wanted; the reply holds what the device will really use, which can
 * differ (it picks the nearest rate and channel count it has and falls back to
 * 16-bit samples), so a program compares and converts. period_bytes and
 * buffer_bytes are only ever outputs.
 */
typedef struct {
	uint32_t rate;
	uint32_t channels;
	uint32_t format;
	uint32_t period_bytes;
	uint32_t buffer_bytes;
} nxu_audio_params_t;

typedef struct {
	uint64_t bytes_written;
	uint64_t periods_played;
	uint64_t xruns;
	uint64_t io_errors;
	uint64_t interrupts;
	nxu_audio_params_t params;
	uint32_t state;
	uint32_t channels_min;
	uint32_t channels_max;
	uint32_t rate_min;
	uint32_t rate_max;
	uint32_t formats; /* bit (1 << NXU_AUDIO_FORMAT_x) for each format the device takes */
	uint32_t latency_bytes;
	uint32_t queued_periods;
} nxu_audio_info_t;

#define NXU_AUDIO_IOC_GROUP 0x41U

#define NXU_AUDIO_GET_INFO NXU_IOR(NXU_AUDIO_IOC_GROUP, 1U, nxu_audio_info_t)
#define NXU_AUDIO_SET_PARAMS NXU_IOWR(NXU_AUDIO_IOC_GROUP, 2U, nxu_audio_params_t)
#define NXU_AUDIO_DRAIN NXU_IO(NXU_AUDIO_IOC_GROUP, 3U)
#define NXU_AUDIO_STOP NXU_IO(NXU_AUDIO_IOC_GROUP, 4U)
#define NXU_AUDIO_SET_NONBLOCK NXU_IOW(NXU_AUDIO_IOC_GROUP, 5U, uint32_t)

_Static_assert(sizeof(nxu_audio_info_t) <= NXU_IOC_SIZE_MAX, "an ioctl argument must fit the kernel's copy buffer");

#endif
