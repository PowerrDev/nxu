#ifndef NXU_DRIVERS_VIRTIO_VIRTIO_SND_H
#define NXU_DRIVERS_VIRTIO_VIRTIO_SND_H

#include <stdint.h>

/*
 * VirtIO Sound wire format, OASIS VirtIO 1.2 section 5.14 (device ID 25).
 *
 * Every field is little endian on the wire. NXU runs little-endian machines
 * only (arm64 and x86), so the structures below are used as they are. The
 * layout is the specification's, byte for byte; the assertions at the end
 * pin every size the device parses.
 */

/* Device-specific feature bit. The transport bits (VERSION_1 and friends) live in virtio_transport.h. */
#define VIRTIO_SND_F_CTLS 0U

/* Virtqueues. */
#define VIRTIO_SND_VQ_CONTROL 0U
#define VIRTIO_SND_VQ_EVENT 1U
#define VIRTIO_SND_VQ_TX 2U
#define VIRTIO_SND_VQ_RX 3U
#define VIRTIO_SND_VQ_COUNT 4U

/* Dataflow direction of a stream or channel map. */
#define VIRTIO_SND_D_OUTPUT 0U
#define VIRTIO_SND_D_INPUT 1U

/* Request types (control queue). */
#define VIRTIO_SND_R_JACK_INFO 0x0001U
#define VIRTIO_SND_R_JACK_REMAP 0x0002U
#define VIRTIO_SND_R_PCM_INFO 0x0100U
#define VIRTIO_SND_R_PCM_SET_PARAMS 0x0101U
#define VIRTIO_SND_R_PCM_PREPARE 0x0102U
#define VIRTIO_SND_R_PCM_RELEASE 0x0103U
#define VIRTIO_SND_R_PCM_START 0x0104U
#define VIRTIO_SND_R_PCM_STOP 0x0105U
#define VIRTIO_SND_R_CHMAP_INFO 0x0200U

/* Event types (event queue). */
#define VIRTIO_SND_EVT_JACK_CONNECTED 0x1000U
#define VIRTIO_SND_EVT_JACK_DISCONNECTED 0x1001U
#define VIRTIO_SND_EVT_PCM_PERIOD_ELAPSED 0x1100U
#define VIRTIO_SND_EVT_PCM_XRUN 0x1101U
#define VIRTIO_SND_EVT_CTL_NOTIFY 0x1200U

/* Status codes: the response to a control request and the status of an I/O message. */
#define VIRTIO_SND_S_OK 0x8000U
#define VIRTIO_SND_S_BAD_MSG 0x8001U
#define VIRTIO_SND_S_NOT_SUPP 0x8002U
#define VIRTIO_SND_S_IO_ERR 0x8003U

/* Stream feature bits (virtio_snd_pcm_info.features and set_params.features). */
#define VIRTIO_SND_PCM_F_SHMEM_HOST 0U
#define VIRTIO_SND_PCM_F_SHMEM_GUEST 1U
#define VIRTIO_SND_PCM_F_MSG_POLLING 2U
#define VIRTIO_SND_PCM_F_EVT_SHMEM_PERIODS 3U
#define VIRTIO_SND_PCM_F_EVT_XRUNS 4U

/* Sample formats: bit positions in virtio_snd_pcm_info.formats, values in set_params.format. */
#define VIRTIO_SND_PCM_FMT_IMA_ADPCM 0U
#define VIRTIO_SND_PCM_FMT_MU_LAW 1U
#define VIRTIO_SND_PCM_FMT_A_LAW 2U
#define VIRTIO_SND_PCM_FMT_S8 3U
#define VIRTIO_SND_PCM_FMT_U8 4U
#define VIRTIO_SND_PCM_FMT_S16 5U
#define VIRTIO_SND_PCM_FMT_U16 6U
#define VIRTIO_SND_PCM_FMT_S18_3 7U
#define VIRTIO_SND_PCM_FMT_U18_3 8U
#define VIRTIO_SND_PCM_FMT_S20_3 9U
#define VIRTIO_SND_PCM_FMT_U20_3 10U
#define VIRTIO_SND_PCM_FMT_S24_3 11U
#define VIRTIO_SND_PCM_FMT_U24_3 12U
#define VIRTIO_SND_PCM_FMT_S20 13U
#define VIRTIO_SND_PCM_FMT_U20 14U
#define VIRTIO_SND_PCM_FMT_S24 15U
#define VIRTIO_SND_PCM_FMT_U24 16U
#define VIRTIO_SND_PCM_FMT_S32 17U
#define VIRTIO_SND_PCM_FMT_U32 18U
#define VIRTIO_SND_PCM_FMT_FLOAT 19U
#define VIRTIO_SND_PCM_FMT_FLOAT64 20U
#define VIRTIO_SND_PCM_FMT_DSD_U8 21U
#define VIRTIO_SND_PCM_FMT_DSD_U16 22U
#define VIRTIO_SND_PCM_FMT_DSD_U32 23U
#define VIRTIO_SND_PCM_FMT_IEC958_SUBFRAME 24U
#define VIRTIO_SND_PCM_FMT_COUNT 25U

/* Frame rates: bit positions in virtio_snd_pcm_info.rates, values in set_params.rate. */
#define VIRTIO_SND_PCM_RATE_5512 0U
#define VIRTIO_SND_PCM_RATE_8000 1U
#define VIRTIO_SND_PCM_RATE_11025 2U
#define VIRTIO_SND_PCM_RATE_16000 3U
#define VIRTIO_SND_PCM_RATE_22050 4U
#define VIRTIO_SND_PCM_RATE_32000 5U
#define VIRTIO_SND_PCM_RATE_44100 6U
#define VIRTIO_SND_PCM_RATE_48000 7U
#define VIRTIO_SND_PCM_RATE_64000 8U
#define VIRTIO_SND_PCM_RATE_88200 9U
#define VIRTIO_SND_PCM_RATE_96000 10U
#define VIRTIO_SND_PCM_RATE_176400 11U
#define VIRTIO_SND_PCM_RATE_192000 12U
#define VIRTIO_SND_PCM_RATE_384000 13U
#define VIRTIO_SND_PCM_RATE_COUNT 14U

/* Channel positions (virtio_snd_chmap_info.positions). */
#define VIRTIO_SND_CHMAP_NONE 0U
#define VIRTIO_SND_CHMAP_NA 1U
#define VIRTIO_SND_CHMAP_MONO 2U
#define VIRTIO_SND_CHMAP_FL 3U
#define VIRTIO_SND_CHMAP_FR 4U
#define VIRTIO_SND_CHMAP_RL 5U
#define VIRTIO_SND_CHMAP_RR 6U
#define VIRTIO_SND_CHMAP_FC 7U
#define VIRTIO_SND_CHMAP_LFE 8U
#define VIRTIO_SND_CHMAP_SL 9U
#define VIRTIO_SND_CHMAP_SR 10U
#define VIRTIO_SND_CHMAP_RC 11U
#define VIRTIO_SND_CHMAP_COUNT 37U

#define VIRTIO_SND_CHMAP_MAX_SIZE 18U

/* Jack feature bit (virtio_snd_jack_info.features). */
#define VIRTIO_SND_JACK_F_REMAP 0U

/* Device configuration space. */
typedef struct {
	uint32_t jacks;
	uint32_t streams;
	uint32_t chmaps;
	uint32_t controls; /* present only when VIRTIO_SND_F_CTLS was negotiated */
} virtio_snd_config_t;

#define VIRTIO_SND_CONFIG_JACKS 0U
#define VIRTIO_SND_CONFIG_STREAMS 4U
#define VIRTIO_SND_CONFIG_CHMAPS 8U
#define VIRTIO_SND_CONFIG_CONTROLS 12U

/* First word of every request, response and event. */
typedef struct {
	uint32_t code;
} virtio_snd_hdr_t;

typedef struct {
	virtio_snd_hdr_t hdr;
	uint32_t data;
} virtio_snd_event_t;

/* Query of jack, stream or channel-map information: an array of `count` items of `size` bytes follows the response header. */
typedef struct {
	virtio_snd_hdr_t hdr;
	uint32_t start_id;
	uint32_t count;
	uint32_t size;
} virtio_snd_query_info_t;

/* Header of every item information structure. */
typedef struct {
	uint32_t hda_fn_nid;
} virtio_snd_info_t;

typedef struct {
	virtio_snd_hdr_t hdr;
	uint32_t jack_id;
} virtio_snd_jack_hdr_t;

typedef struct {
	virtio_snd_info_t hdr;
	uint32_t features;
	uint32_t hda_reg_defconf;
	uint32_t hda_reg_caps;
	uint8_t connected;
	uint8_t padding[7];
} virtio_snd_jack_info_t;

/* Header of every stream request: PCM_SET_PARAMS extends it, PREPARE/RELEASE/START/STOP are just this. */
typedef struct {
	virtio_snd_hdr_t hdr;
	uint32_t stream_id;
} virtio_snd_pcm_hdr_t;

typedef struct {
	virtio_snd_info_t hdr;
	uint32_t features;
	uint64_t formats;
	uint64_t rates;
	uint8_t direction;
	uint8_t channels_min;
	uint8_t channels_max;
	uint8_t padding[5];
} virtio_snd_pcm_info_t;

typedef struct {
	virtio_snd_pcm_hdr_t hdr;
	uint32_t buffer_bytes;
	uint32_t period_bytes;
	uint32_t features;
	uint8_t channels;
	uint8_t format;
	uint8_t rate;
	uint8_t padding;
} virtio_snd_pcm_set_params_t;

/* tx/rx message: this header, then the samples (tx) or the room for them (rx), then the status the device fills in. */
typedef struct {
	uint32_t stream_id;
} virtio_snd_pcm_xfer_t;

typedef struct {
	uint32_t status;
	uint32_t latency_bytes;
} virtio_snd_pcm_status_t;

typedef struct {
	virtio_snd_hdr_t hdr;
	uint32_t chmap_id;
} virtio_snd_chmap_hdr_t;

typedef struct {
	virtio_snd_info_t hdr;
	uint8_t direction;
	uint8_t channels;
	uint8_t positions[VIRTIO_SND_CHMAP_MAX_SIZE];
} virtio_snd_chmap_info_t;

_Static_assert(sizeof(virtio_snd_config_t) == 16U, "VirtIO Sound config size mismatch");
_Static_assert(sizeof(virtio_snd_hdr_t) == 4U, "VirtIO Sound header size mismatch");
_Static_assert(sizeof(virtio_snd_event_t) == 8U, "VirtIO Sound event size mismatch");
_Static_assert(sizeof(virtio_snd_query_info_t) == 16U, "VirtIO Sound query size mismatch");
_Static_assert(sizeof(virtio_snd_jack_info_t) == 24U, "VirtIO Sound jack info size mismatch");
_Static_assert(sizeof(virtio_snd_pcm_hdr_t) == 8U, "VirtIO Sound PCM header size mismatch");
_Static_assert(sizeof(virtio_snd_pcm_info_t) == 32U, "VirtIO Sound PCM info size mismatch");
_Static_assert(sizeof(virtio_snd_pcm_set_params_t) == 24U, "VirtIO Sound set-params size mismatch");
_Static_assert(sizeof(virtio_snd_pcm_xfer_t) == 4U, "VirtIO Sound xfer size mismatch");
_Static_assert(sizeof(virtio_snd_pcm_status_t) == 8U, "VirtIO Sound status size mismatch");
_Static_assert(sizeof(virtio_snd_chmap_info_t) == 24U, "VirtIO Sound chmap info size mismatch");

#endif
