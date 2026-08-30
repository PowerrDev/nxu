#include <kern/sched_prism/processor.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static struct processor g_boot_processor;
static bool g_processor_initialized;

/*
 * processor_bootstrap
 */
bool processor_bootstrap(void)
{
	if (g_processor_initialized) return true;

	memset(&g_boot_processor, 0, sizeof(g_boot_processor));

	g_boot_processor.cpu_id = 0U;
	g_boot_processor.state = PROCESSOR_STARTING;
	run_queue_init(&g_boot_processor.runq);
	g_boot_processor.state = PROCESSOR_RUNNING;

	g_processor_initialized = true;
	return true;
}

/*
 * current_processor
 */
processor_t current_processor(void)
{
	return g_processor_initialized ? &g_boot_processor : 0;
}

/*
 * processor_validate
 */
bool processor_validate(processor_t processor)
{
	if (
		processor == 0 ||
		processor->cpu_id != 0U ||
		processor->state == PROCESSOR_OFFLINE ||
		processor->state == PROCESSOR_SHUTDOWN ||
		!run_queue_validate(&processor->runq)
	) {
		return false;
	}

	if (
		processor->idle_thread != 0 &&
		(!thread_is_active(processor->idle_thread) ||
		!thread_is_idle(processor->idle_thread) ||
		!thread_is_started(processor->idle_thread) ||
		processor->idle_thread->runq != 0)
	) {
		return false;
	}

	if (
		processor->active_thread != 0 &&
		(!thread_is_active(processor->active_thread) ||
		processor->active_thread->runq != 0)
	) {
		return false;
	}

	return true;
}
