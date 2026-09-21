#include <drivers/virtio/virtio_sound.h>

#include <drivers/virtio/virtio_sound_internal.h>
#include <kern/machine/barrier.h>
#include <kern/machine/machine_routines.h>
#include <kern/machine/timer.h>
#include <kern/process/proc.h>
#include <kern/process/signal.h>
#include <kern/process/thread.h>
#include <kern/sched_prism/sched.h>
#include <kern/sched_prism/waitq.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * The PCM half of the VirtIO Sound driver: the output stream's state machine
 * on the device, the ring of periods on the tx queue, and the interrupt that
 * hands finished periods back.
 *
 * Who runs what. A writer (a system call, or a kernel thread) fills the
 * period at the head of the ring and submits it when it is full; the device
 * finishes periods in the order it plays them and says so on the used ring;
 * the interrupt handler, or a writer that has nothing better to do while it
 * waits, collects them and frees their slots. All of that touches the ring's
 * state, so every path holds interrupts masked while it does: on this
 * single-CPU kernel that is the lock. A writer that has to wait for room
 * sleeps on a wait queue with interrupts masked, which makes joining the
 * queue and sleeping one step the handler cannot slip into between the
 * check and the sleep, and the handler wakes it.
 */

/* ---- polling versus sleeping ------------------------------------------ */

/*
 * virtio_snd_may_sleep:
 *
 * A writer may sleep only where something will wake it: the device's
 * interrupt has to be bound, and the caller has to be a thread the scheduler
 * can run something else in place of. The boot context (the bootstrap thread,
 * or any time before the scheduler exists) polls instead.
 */
static bool virtio_snd_may_sleep(const virtio_snd_device_t *device)
{
	if (!device->transport.irq_bound || !sched_is_initialized()) return false;

	thread_t thread = current_thread();

	return thread != 0 && thread != sched_bootstrap_thread();
}

static void virtio_snd_yield(void)
{
	thread_t thread = current_thread();

	if (sched_is_initialized() && thread != 0 && thread != sched_bootstrap_thread()) {
		(void)sched_yield();
	} else {
		ml_cpu_relax();
	}
}

/* ---- events ------------------------------------------------------------ */

bool virtio_snd_post_events(virtio_snd_device_t *device)
{
	for (uint32_t index = 0U; index < VIRTIO_SND_EVENT_QUEUE_SIZE; index++) {
		uint16_t desc;

		if (!virtqueue_alloc_descriptor(&device->eventq, &desc)) return false;

		device->event_desc[index] = desc;

		virtq_desc_t *descriptor = &device->eventq.descriptors[desc];

		descriptor->address = device->event_page.physical + (uint64_t)index * sizeof(virtio_snd_event_t);
		descriptor->length = sizeof(virtio_snd_event_t);
		descriptor->flags = VIRTQ_DESC_F_WRITE;
		descriptor->next = 0U;

		ml_dma_wmb();

		if (!virtqueue_submit(&device->eventq, desc)) return false;
	}

	return true;
}

/*
 * virtio_snd_process_events:
 *
 * Take the events the device has reported and give each buffer back. The
 * counts and the last event are kept for the summary; nothing is logged from
 * here, it may be the interrupt handler.
 */
static void virtio_snd_process_events(virtio_snd_device_t *device)
{
	uint32_t id;
	uint32_t length;
	bool reposted = false;

	while (virtqueue_pop_used(&device->eventq, &id, &length)) {
		for (uint32_t index = 0U; index < VIRTIO_SND_EVENT_QUEUE_SIZE; index++) {
			if (device->event_desc[index] != id) continue;

			volatile virtio_snd_event_t *event = (volatile virtio_snd_event_t *)(void *)(device->event_page.virtual_address + (size_t)index * sizeof(virtio_snd_event_t));

			ml_dma_rmb();

			uint32_t code = event->hdr.code;
			uint32_t data = event->data;

			device->event_count++;
			device->last_event = code;
			device->last_event_data = data;

			if (code == VIRTIO_SND_EVT_PCM_XRUN) device->xrun_events++;
			if (code == VIRTIO_SND_EVT_JACK_CONNECTED || code == VIRTIO_SND_EVT_JACK_DISCONNECTED) device->jack_events++;

			event->hdr.code = 0U;
			event->data = 0U;
			ml_dma_wmb();

			if (virtqueue_submit(&device->eventq, (uint16_t)id)) reposted = true;
			break;
		}
	}

	if (reposted) virtio_device_notify(&device->transport, VIRTIO_SND_VQ_EVENT);
}

/* ---- completions ------------------------------------------------------- */

/*
 * virtio_snd_reap:
 *
 * Collect the periods the device has finished with: read each status, free
 * the slot, and wake a writer waiting for room. A period that completes with
 * nothing left queued while the stream runs and the writer is not draining
 * is an underrun: the device ran dry, the host plays silence until more
 * arrives. Called with interrupts masked.
 */
static void virtio_snd_reap(virtio_snd_device_t *device)
{
	virtio_snd_playback_t *playback = &device->playback;
	uint32_t id;
	uint32_t length;
	uint32_t completed = 0U;

	while (virtqueue_pop_used(&device->txq, &id, &length)) {
		for (uint32_t index = 0U; index < VIRTIO_SND_TX_SLOTS; index++) {
			virtio_snd_slot_t *slot = &playback->slots[index];

			if (!playback->slots_allocated || slot->state != VIRTIO_SND_SLOT_QUEUED || slot->data_desc != id) continue;

			ml_dma_rmb();

			volatile const virtio_snd_pcm_status_t *status = (volatile const virtio_snd_pcm_status_t *)(const void *)(slot->page.virtual_address + VIRTIO_SND_SLOT_STATUS_OFFSET);

			if (status->status != VIRTIO_SND_S_OK) playback->io_errors++;

			playback->latency_bytes = status->latency_bytes;
			slot->state = VIRTIO_SND_SLOT_FREE;
			slot->bytes = 0U;
			playback->queued--;
			playback->periods_completed++;
			playback->total_periods++;
			completed++;
			break;
		}
	}

	if (completed == 0U) return;

	playback->end_us = timer_get_microseconds();

	if (playback->state == VIRTIO_SND_PCM_STATE_STARTED && playback->queued == 0U && !playback->draining) {
		playback->xruns++;
		playback->total_xruns++;
	}

	waitq_wake_all(&playback->waitq);
}

/* Take what the device has finished with; the caller has interrupts masked. */
static void virtio_snd_service_locked(virtio_snd_device_t *device)
{
	virtio_snd_process_events(device);
	virtio_snd_reap(device);
}

/*
 * virtio_snd_irq:
 *
 * The device's interrupt. Reading the status acknowledges it on PCI (the ISR
 * byte clears on read), writing it back does on MMIO; either way it is done
 * before the queues are looked at, so a completion that lands in between
 * raises a fresh interrupt instead of being lost.
 */
void virtio_snd_irq(uint32_t intid, void *context)
{
	virtio_snd_device_t *device = context;

	if (device == 0 || !device->ready || device->transport.intid != intid) return;

	uint32_t status = virtio_device_interrupt_status(&device->transport);

	if (status == 0U) return;

	virtio_device_interrupt_ack(&device->transport, status);
	device->irq_count++;

	if ((status & VIRTIO_INTERRUPT_USED_BUFFER) != 0U) virtio_snd_service_locked(device);
}

/*
 * virtio_snd_wait_step:
 *
 * One step of waiting for the device: sleep until the interrupt has woken
 * the playback wait queue, or, where that cannot be relied on, look at the
 * rings ourselves and let something else run. Called with interrupts masked
 * (they stay masked across the sleep; what runs meanwhile has its own state).
 * False when a signal interrupted the wait.
 */
static bool virtio_snd_wait_step(virtio_snd_device_t *device)
{
	if (virtio_snd_may_sleep(device)) {
		if (signal_pending_for(current_proc(), current_thread())) return false;

		return waitq_block(&device->playback.waitq, true);
	}

	virtio_snd_service_locked(device);
	virtio_snd_yield();
	return true;
}

/* Let `microseconds` go by, collecting completions meanwhile. Interrupts masked by the caller. */
static void virtio_snd_delay(virtio_snd_device_t *device, uint64_t microseconds)
{
	uint64_t deadline = timer_get_microseconds() + microseconds;

	while (timer_get_microseconds() < deadline) {
		virtio_snd_service_locked(device);
		virtio_snd_yield();
	}
}

/* ---- the stream on the device ----------------------------------------- */

/*
 * virtio_snd_stream_op:
 *
 * One request of the stream lifecycle. The state machine is checked first: a
 * request that is illegal in the current state is refused here and never
 * sent, because the device treats it as a fatal driver error. The state
 * changes only when the device has said OK.
 */
static virtio_snd_error_t virtio_snd_stream_op(virtio_snd_device_t *device, virtio_snd_pcm_op_t op)
{
	virtio_snd_playback_t *playback = &device->playback;
	uint32_t stream = device->playback_stream;
	virtio_snd_pcm_state_t next;

	if (!virtio_snd_pcm_transition(playback->state, op, &next)) {
		VIRTIO_SND_LOG("stream %u: %s is illegal in state %s, not sent\n", stream, virtio_snd_pcm_op_name(op), virtio_snd_pcm_state_name(playback->state));
		return VIRTIO_SND_E_ILLEGAL;
	}

	virtio_snd_error_t error;

	if (op == VIRTIO_SND_PCM_OP_SET_PARAMS) {
		error = virtio_snd_set_params_request(device, stream, playback->params.buffer_bytes, playback->params.period_bytes, &playback->format);
	} else {
		error = virtio_snd_pcm_request(device, virtio_snd_pcm_op_request(op), stream);
	}

	if (error != VIRTIO_SND_E_NONE) {
		VIRTIO_SND_LOG("stream %u: %s failed: %s\n", stream, virtio_snd_pcm_op_name(op), virtio_snd_error_name(error));
		return error;
	}

	kverbosef("virtio_snd_stream_op: stream %u: %s, %s to %s\n", stream, virtio_snd_pcm_op_name(op), virtio_snd_pcm_state_name(playback->state), virtio_snd_pcm_state_name(next));
	playback->state = next;
	return VIRTIO_SND_E_NONE;
}

/* The period the ring is cut into for the current format: the largest whole number of frames in a page's payload. */
static void virtio_snd_compute_geometry(virtio_snd_playback_t *playback)
{
	uint32_t sample_bytes = virtio_snd_format_bytes(playback->format.format);

	playback->frame_bytes = sample_bytes * playback->format.channels;
	playback->params.rate_hz = virtio_snd_rate_hz(playback->format.rate);
	playback->params.channels = playback->format.channels;
	playback->params.format = playback->format.format;
	playback->params.period_bytes = playback->frame_bytes == 0U ? 0U : (VIRTIO_SND_PERIOD_MAX_BYTES / playback->frame_bytes) * playback->frame_bytes;
	playback->params.buffer_bytes = playback->params.period_bytes * VIRTIO_SND_TX_SLOTS;
}

static void virtio_snd_slots_free(virtio_snd_device_t *device)
{
	virtio_snd_playback_t *playback = &device->playback;

	for (uint32_t index = 0U; index < VIRTIO_SND_TX_SLOTS; index++) {
		virtio_snd_slot_t *slot = &playback->slots[index];

		if (slot->descriptors > 1U) (void)virtqueue_free_descriptor(&device->txq, slot->status_desc);
		if (slot->descriptors > 0U) (void)virtqueue_free_descriptor(&device->txq, slot->data_desc);
		if (slot->page.physical != 0ULL) virtio_snd_free_page(&slot->page);

		memset(slot, 0, sizeof(*slot));
	}

	playback->slots_allocated = false;
	playback->fill_slot = VIRTIO_SND_NO_SLOT;
	playback->queued = 0U;
}

static virtio_snd_error_t virtio_snd_slots_allocate(virtio_snd_device_t *device)
{
	virtio_snd_playback_t *playback = &device->playback;

	if (playback->slots_allocated) return VIRTIO_SND_E_NONE;

	for (uint32_t index = 0U; index < VIRTIO_SND_TX_SLOTS; index++) {
		virtio_snd_slot_t *slot = &playback->slots[index];

		if (!virtio_snd_allocate_page(&slot->page)) {
			virtio_snd_slots_free(device);
			return VIRTIO_SND_E_NO_MEMORY;
		}

		if (!virtqueue_alloc_descriptor(&device->txq, &slot->data_desc)) {
			virtio_snd_slots_free(device);
			return VIRTIO_SND_E_NO_MEMORY;
		}

		slot->descriptors = 1U;

		if (!virtqueue_alloc_descriptor(&device->txq, &slot->status_desc)) {
			virtio_snd_slots_free(device);
			return VIRTIO_SND_E_NO_MEMORY;
		}

		slot->descriptors = 2U;
		slot->state = VIRTIO_SND_SLOT_FREE;
		slot->bytes = 0U;

		/* The header never changes: the stream every message is for. */
		*(uint32_t *)(void *)slot->page.virtual_address = device->playback_stream;
	}

	playback->slots_allocated = true;
	playback->fill_slot = VIRTIO_SND_NO_SLOT;
	playback->queued = 0U;
	return VIRTIO_SND_E_NONE;
}

/*
 * virtio_snd_release:
 *
 * Stop what is running and release the stream. The device completes every
 * period it holds before it answers RELEASE, so afterwards the ring is
 * idle, and its pages can be freed unless the device has stopped answering
 * (then it may still own them, and they are kept).
 */
static void virtio_snd_release(virtio_snd_device_t *device)
{
	virtio_snd_playback_t *playback = &device->playback;

	if (playback->state == VIRTIO_SND_PCM_STATE_STARTED) (void)virtio_snd_stream_op(device, VIRTIO_SND_PCM_OP_STOP);

	if (playback->state == VIRTIO_SND_PCM_STATE_PREPARED || playback->state == VIRTIO_SND_PCM_STATE_STOPPED) {
		(void)virtio_snd_stream_op(device, VIRTIO_SND_PCM_OP_RELEASE);
	}

	virtio_snd_service_locked(device);

	if (playback->queued != 0U) {
		VIRTIO_SND_LOG("stream %u: %u period(s) are still queued after RELEASE\n", device->playback_stream, playback->queued);
	}

	if (!device->control_dead && playback->queued == 0U) {
		virtio_snd_slots_free(device);
	} else {
		playback->fill_slot = VIRTIO_SND_NO_SLOT;
	}

	playback->draining = false;
	playback->needs_configure = true;
}

/*
 * virtio_snd_configure:
 *
 * Bring the stream from wherever it is to PREPARED with the parameters the
 * open has chosen: SET_PARAMS, PREPARE, and the ring of periods.
 */
static virtio_snd_error_t virtio_snd_configure(virtio_snd_device_t *device)
{
	virtio_snd_playback_t *playback = &device->playback;
	virtio_snd_error_t error;

	if (playback->state == VIRTIO_SND_PCM_STATE_STARTED || playback->state == VIRTIO_SND_PCM_STATE_STOPPED) virtio_snd_release(device);

	virtio_snd_compute_geometry(playback);

	if (playback->params.period_bytes == 0U) return VIRTIO_SND_E_INVALID;

	error = virtio_snd_stream_op(device, VIRTIO_SND_PCM_OP_SET_PARAMS);
	if (error != VIRTIO_SND_E_NONE) return error;

	error = virtio_snd_stream_op(device, VIRTIO_SND_PCM_OP_PREPARE);
	if (error != VIRTIO_SND_E_NONE) return error;

	error = virtio_snd_slots_allocate(device);

	if (error != VIRTIO_SND_E_NONE) {
		VIRTIO_SND_LOG("stream %u: no memory for the ring of periods\n", device->playback_stream);
		return error;
	}

	playback->needs_configure = false;

	VIRTIO_SND_LOG("stream %u: %u Hz, %u channel(s), %s\n", device->playback_stream, playback->params.rate_hz, playback->params.channels, virtio_snd_format_name(playback->params.format));
	VIRTIO_SND_LOG("stream %u: %u periods of %u bytes\n", device->playback_stream, VIRTIO_SND_TX_SLOTS, playback->params.period_bytes);
	VIRTIO_SND_LOG("stream %u: buffer %u bytes\n", device->playback_stream, playback->params.buffer_bytes);
	return VIRTIO_SND_E_NONE;
}

/* ---- the ring ---------------------------------------------------------- */

static uint32_t virtio_snd_find_free_slot(const virtio_snd_playback_t *playback)
{
	for (uint32_t index = 0U; index < VIRTIO_SND_TX_SLOTS; index++) {
		if (playback->slots[index].state == VIRTIO_SND_SLOT_FREE) return index;
	}

	return VIRTIO_SND_NO_SLOT;
}

static virtio_snd_error_t virtio_snd_start(virtio_snd_device_t *device)
{
	virtio_snd_error_t error = virtio_snd_stream_op(device, VIRTIO_SND_PCM_OP_START);

	if (error == VIRTIO_SND_E_NONE) {
		device->playback.start_us = timer_get_microseconds();
		kverbosef("virtio_snd_start: stream %u started with %u period(s) queued\n", device->playback_stream, device->playback.queued);
	}

	return error;
}

/*
 * virtio_snd_submit_fill:
 *
 * Hand the period being filled to the device: one message, the header and
 * the samples in the first descriptor, the status the device writes in the
 * second. A period that ends in the middle of a frame (the last bytes of a
 * stream) is padded with silence. Once enough is queued the stream starts.
 */
static virtio_snd_error_t virtio_snd_submit_fill(virtio_snd_device_t *device)
{
	virtio_snd_playback_t *playback = &device->playback;
	virtio_snd_slot_t *slot = &playback->slots[playback->fill_slot];

	if (slot->bytes % playback->frame_bytes != 0U) {
		uint32_t pad = playback->frame_bytes - slot->bytes % playback->frame_bytes;

		memset(slot->page.virtual_address + VIRTIO_SND_SLOT_DATA_OFFSET + slot->bytes, 0, pad);
		slot->bytes += pad;
	}

	playback->fill_slot = VIRTIO_SND_NO_SLOT;

	if (slot->bytes == 0U) {
		slot->state = VIRTIO_SND_SLOT_FREE;
		return VIRTIO_SND_E_NONE;
	}

	virtq_desc_t *data = &device->txq.descriptors[slot->data_desc];

	data->address = slot->page.physical;
	data->length = VIRTIO_SND_SLOT_DATA_OFFSET + slot->bytes;
	data->flags = VIRTQ_DESC_F_NEXT;
	data->next = slot->status_desc;

	virtq_desc_t *status = &device->txq.descriptors[slot->status_desc];

	status->address = slot->page.physical + VIRTIO_SND_SLOT_STATUS_OFFSET;
	status->length = sizeof(virtio_snd_pcm_status_t);
	status->flags = VIRTQ_DESC_F_WRITE;
	status->next = 0U;

	memset(slot->page.virtual_address + VIRTIO_SND_SLOT_STATUS_OFFSET, 0, sizeof(virtio_snd_pcm_status_t));
	ml_dma_wmb();

	if (!virtqueue_submit(&device->txq, slot->data_desc)) {
		slot->state = VIRTIO_SND_SLOT_FREE;
		slot->bytes = 0U;
		playback->io_errors++;
		return VIRTIO_SND_E_IO;
	}

	slot->state = VIRTIO_SND_SLOT_QUEUED;
	playback->queued++;
	playback->periods_submitted++;
	virtio_device_notify(&device->transport, VIRTIO_SND_VQ_TX);

	bool startable = playback->state == VIRTIO_SND_PCM_STATE_PREPARED || playback->state == VIRTIO_SND_PCM_STATE_STOPPED;

	if (startable && playback->queued >= VIRTIO_SND_PREBUFFER_SLOTS) return virtio_snd_start(device);

	return VIRTIO_SND_E_NONE;
}

/* ---- the interface ----------------------------------------------------- */

virtio_snd_error_t virtio_snd_playback_open(virtio_snd_device_t *device, bool nonblock)
{
	if (device == 0 || !device->attached || device->playback_stream == VIRTIO_SND_NO_STREAM) return VIRTIO_SND_E_INVALID;
	if (device->control_dead) return VIRTIO_SND_E_DEAD;

	virtio_snd_playback_t *playback = &device->playback;
	uint64_t irq_state = ml_irq_save();

	if (playback->open) {
		ml_irq_restore(irq_state);
		return VIRTIO_SND_E_BUSY;
	}

	playback->open = true;
	playback->nonblock = nonblock;
	playback->needs_configure = true;
	playback->draining = false;
	playback->fill_slot = VIRTIO_SND_NO_SLOT;

	/* Every open starts from 16-bit stereo at 44.1 kHz until it says otherwise. */
	playback->format.channels = 2U;
	playback->format.format = VIRTIO_SND_PCM_FMT_S16;
	playback->format.rate = VIRTIO_SND_PCM_RATE_44100;
	virtio_snd_compute_geometry(playback);

	playback->latency_bytes = 0U;
	playback->bytes_written = 0ULL;
	playback->periods_submitted = 0ULL;
	playback->periods_completed = 0ULL;
	playback->xruns = 0ULL;
	playback->io_errors = 0ULL;
	playback->start_us = 0ULL;
	playback->end_us = 0ULL;
	playback->opens++;

	ml_irq_restore(irq_state);

	VIRTIO_SND_LOG("stream %u opened for playback%s\n", device->playback_stream, nonblock ? " (non-blocking)" : "");
	return VIRTIO_SND_E_NONE;
}

void virtio_snd_playback_set_nonblock(virtio_snd_device_t *device, bool nonblock)
{
	if (device != 0) device->playback.nonblock = nonblock;
}

virtio_snd_error_t virtio_snd_playback_set_params(virtio_snd_device_t *device, const virtio_snd_params_t *wanted, virtio_snd_params_t *actual)
{
	if (device == 0 || wanted == 0 || actual == 0) return VIRTIO_SND_E_INVALID;

	virtio_snd_playback_t *playback = &device->playback;
	uint32_t rate;

	if (!playback->open) return VIRTIO_SND_E_INVALID;

	if (wanted->channels == 0U || wanted->channels > 255U || wanted->format >= VIRTIO_SND_PCM_FMT_COUNT || !virtio_snd_rate_from_hz(wanted->rate_hz, &rate)) {
		VIRTIO_SND_LOG("stream %u: %u Hz, %u channel(s), format %u is not a format the device knows\n", device->playback_stream, wanted->rate_hz, wanted->channels, wanted->format);
		return VIRTIO_SND_E_INVALID;
	}

	virtio_snd_pcm_format_t request = { .channels = (uint8_t)wanted->channels, .format = (uint8_t)wanted->format, .rate = (uint8_t)rate };
	virtio_snd_pcm_format_t picked;

	if (!virtio_snd_pcm_pick(&device->pcm_info[device->playback_stream], &request, &picked)) {
		VIRTIO_SND_LOG("stream %u: nothing the stream offers matches %u Hz, %u channel(s), %s\n", device->playback_stream, wanted->rate_hz, wanted->channels, virtio_snd_format_name(wanted->format));
		return VIRTIO_SND_E_INVALID;
	}

	uint64_t irq_state = ml_irq_save();

	/* A new format abandons what was queued in the old one. */
	if (playback->state == VIRTIO_SND_PCM_STATE_PREPARED || playback->state == VIRTIO_SND_PCM_STATE_STARTED || playback->state == VIRTIO_SND_PCM_STATE_STOPPED) virtio_snd_release(device);

	playback->format = picked;
	playback->needs_configure = true;
	virtio_snd_compute_geometry(playback);
	*actual = playback->params;

	ml_irq_restore(irq_state);

	if (picked.channels != request.channels || picked.format != request.format || picked.rate != request.rate) {
		VIRTIO_SND_LOG("stream %u: asked for %u Hz, %u channel(s), %s; the stream will run %u Hz, %u channel(s), %s\n", device->playback_stream, wanted->rate_hz, wanted->channels, virtio_snd_format_name(wanted->format), actual->rate_hz, actual->channels, virtio_snd_format_name(actual->format));
	}

	return VIRTIO_SND_E_NONE;
}

virtio_snd_error_t virtio_snd_playback_write(virtio_snd_device_t *device, const void *data, uint64_t size, uint64_t *written)
{
	if (written != 0) *written = 0ULL;

	if (device == 0 || written == 0 || (data == 0 && size != 0ULL)) return VIRTIO_SND_E_INVALID;

	virtio_snd_playback_t *playback = &device->playback;

	if (!playback->open) return VIRTIO_SND_E_INVALID;
	if (device->control_dead) return VIRTIO_SND_E_DEAD;

	uint64_t irq_state = ml_irq_save();
	virtio_snd_error_t error = VIRTIO_SND_E_NONE;
	const uint8_t *bytes = data;

	if (playback->needs_configure) error = virtio_snd_configure(device);

	while (error == VIRTIO_SND_E_NONE && size != 0ULL) {
		if (playback->fill_slot == VIRTIO_SND_NO_SLOT) {
			uint32_t free_slot = virtio_snd_find_free_slot(playback);

			if (free_slot == VIRTIO_SND_NO_SLOT) {
				virtio_snd_service_locked(device);
				free_slot = virtio_snd_find_free_slot(playback);
			}

			if (free_slot == VIRTIO_SND_NO_SLOT) {
				if (playback->nonblock) {
					if (*written == 0ULL) error = VIRTIO_SND_E_AGAIN;
					break;
				}

				if (!virtio_snd_wait_step(device)) {
					if (*written == 0ULL) error = VIRTIO_SND_E_INTERRUPTED;
					break;
				}

				continue;
			}

			playback->fill_slot = free_slot;
			playback->slots[free_slot].state = VIRTIO_SND_SLOT_FILLING;
			playback->slots[free_slot].bytes = 0U;
		}

		virtio_snd_slot_t *slot = &playback->slots[playback->fill_slot];
		uint64_t room = playback->params.period_bytes - slot->bytes;
		uint64_t chunk = size < room ? size : room;

		memcpy(slot->page.virtual_address + VIRTIO_SND_SLOT_DATA_OFFSET + slot->bytes, bytes, (size_t)chunk);
		slot->bytes += (uint32_t)chunk;
		bytes += chunk;
		size -= chunk;
		*written += chunk;
		playback->bytes_written += chunk;

		if (slot->bytes == playback->params.period_bytes) error = virtio_snd_submit_fill(device);
	}

	ml_irq_restore(irq_state);
	return error;
}

virtio_snd_error_t virtio_snd_playback_drain(virtio_snd_device_t *device)
{
	if (device == 0) return VIRTIO_SND_E_INVALID;

	virtio_snd_playback_t *playback = &device->playback;

	if (!playback->open) return VIRTIO_SND_E_INVALID;
	if (device->control_dead) return VIRTIO_SND_E_DEAD;

	uint64_t irq_state = ml_irq_save();
	virtio_snd_error_t error = VIRTIO_SND_E_NONE;

	/* Nothing was ever written: nothing to wait for. */
	if (playback->needs_configure || (playback->state != VIRTIO_SND_PCM_STATE_PREPARED && playback->state != VIRTIO_SND_PCM_STATE_STARTED && playback->state != VIRTIO_SND_PCM_STATE_STOPPED)) {
		ml_irq_restore(irq_state);
		return VIRTIO_SND_E_NONE;
	}

	playback->draining = true;

	if (playback->fill_slot != VIRTIO_SND_NO_SLOT) error = virtio_snd_submit_fill(device);

	if (error == VIRTIO_SND_E_NONE && playback->queued != 0U && playback->state != VIRTIO_SND_PCM_STATE_STARTED) error = virtio_snd_start(device);

	while (error == VIRTIO_SND_E_NONE && playback->queued != 0U) {
		if (!virtio_snd_wait_step(device)) error = VIRTIO_SND_E_INTERRUPTED;
	}

	if (error == VIRTIO_SND_E_NONE && playback->state == VIRTIO_SND_PCM_STATE_STARTED) {
		virtio_snd_delay(device, VIRTIO_SND_DRAIN_TAIL_US);
		error = virtio_snd_stream_op(device, VIRTIO_SND_PCM_OP_STOP);
	}

	playback->draining = false;
	playback->end_us = timer_get_microseconds();
	ml_irq_restore(irq_state);
	return error;
}

virtio_snd_error_t virtio_snd_playback_stop(virtio_snd_device_t *device)
{
	if (device == 0 || !device->playback.open) return VIRTIO_SND_E_INVALID;

	uint64_t irq_state = ml_irq_save();

	virtio_snd_release(device);
	ml_irq_restore(irq_state);
	return VIRTIO_SND_E_NONE;
}

void virtio_snd_playback_close(virtio_snd_device_t *device)
{
	if (device == 0 || !device->playback.open) return;

	virtio_snd_playback_t *playback = &device->playback;
	uint64_t irq_state = ml_irq_save();

	virtio_snd_release(device);
	playback->open = false;
	ml_irq_restore(irq_state);

	uint32_t stream = device->playback_stream;

	if (playback->periods_submitted == 0ULL) {
		VIRTIO_SND_LOG("stream %u closed, nothing was played\n", stream);
		return;
	}

	uint64_t milliseconds = playback->end_us > playback->start_us ? (playback->end_us - playback->start_us) / 1000ULL : 0ULL;

	VIRTIO_SND_LOG("stream %u closed\n", stream);
	VIRTIO_SND_LOG("stream %u: %llu byte(s) written\n", stream, (unsigned long long)playback->bytes_written);
	VIRTIO_SND_LOG("stream %u: %llu period(s) played\n", stream, (unsigned long long)playback->periods_completed);
	VIRTIO_SND_LOG("stream %u: %llu underrun(s)\n", stream, (unsigned long long)playback->xruns);
	VIRTIO_SND_LOG("stream %u: %llu I/O error(s)\n", stream, (unsigned long long)playback->io_errors);
	VIRTIO_SND_LOG("stream %u: device latency %u byte(s)\n", stream, playback->latency_bytes);
	VIRTIO_SND_LOG("stream %u: %llu interrupt(s) taken since attach\n", stream, (unsigned long long)device->irq_count);
	VIRTIO_SND_LOG("stream %u: playback took %llu ms\n", stream, (unsigned long long)milliseconds);
}

void virtio_snd_playback_info(virtio_snd_device_t *device, virtio_snd_playback_info_t *info)
{
	if (device == 0 || info == 0) return;

	const virtio_snd_playback_t *playback = &device->playback;
	const virtio_snd_pcm_info_t *stream = &device->pcm_info[device->playback_stream];

	memset(info, 0, sizeof(*info));
	info->state = playback->state;
	info->params = playback->params;
	info->channels_min = stream->channels_min;
	info->channels_max = stream->channels_max;
	info->formats = stream->formats;
	info->rates = stream->rates;
	info->latency_bytes = playback->latency_bytes;
	info->queued_periods = playback->queued;
	info->bytes_written = playback->bytes_written;
	info->periods_played = playback->periods_completed;
	info->xruns = playback->xruns;
	info->io_errors = playback->io_errors;
	info->irq_count = device->irq_count;
	info->start_us = playback->start_us;
	info->end_us = playback->end_us;
}
