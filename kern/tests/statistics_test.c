#include <kern/tests/statistics_test.h>
#include <drivers/virtio/virtqueue.h>
#include <kern/console/console.h>
#include <vm/pmm.h>

bool statistics_self_test(void)
{
	uint64_t free_before = pmm_get_free_page_count();
	nxu_stats_virtio_t before, during, after;
	virtqueue_statistics(&before);
	virtqueue_t queue = {0};
	if (!virtqueue_init(&queue, 8U))
		return false;
	bool ok = true;
	uint16_t descriptor;
	for (uint32_t i = 0U; i < 8U; i++)
		if (!virtqueue_alloc_descriptor(&queue, &descriptor))
			ok = false;
	if (virtqueue_alloc_descriptor(&queue, &descriptor))
		ok = false;
	if (!virtqueue_submit(&queue, 0U))
		ok = false;
	if (virtqueue_submit(&queue, queue.size))
		ok = false;
	/* Simulate a device returning one bad ID and then a valid completion.
	 * No transport ever sees this test queue. Keep the driver's existing
	 * validation contract: pop_used reports the ID even when it is bad. */
	queue.used_ring[0].id = queue.size;
	queue.used_ring[0].length = 64U;
	queue.used_ring[1].id = 0U;
	queue.used_ring[1].length = 32U;
	*queue.used_index = 2U;
	uint32_t id, length;
	if (!virtqueue_pop_used(&queue, &id, &length) || id != queue.size || length != 64U)
		ok = false;
	if (!virtqueue_pop_used(&queue, &id, &length) || id != 0U || length != 32U)
		ok = false;
	if (virtqueue_pop_used(&queue, &id, &length))
		ok = false;
	virtqueue_statistics(&during);
	if (during.submissions < before.submissions + 1ULL || during.completions < before.completions + 2ULL ||
		during.used_bytes < before.used_bytes + 96ULL ||
		during.rejected_submissions < before.rejected_submissions + 1ULL ||
		during.invalid_completions < before.invalid_completions + 1ULL ||
		during.descriptor_exhaustions < before.descriptor_exhaustions + 1ULL ||
		during.live_queues != before.live_queues + 1ULL)
		ok = false;
	if (!virtqueue_destroy(&queue))
		ok = false;
	virtqueue_statistics(&after);
	if (after.live_queues != before.live_queues || pmm_get_free_page_count() != free_before)
		ok = false;
	if (ok)
		kputln("statistics: self-test passed (VirtIO traffic, errors, and queue cleanup)");
	return ok;
}
