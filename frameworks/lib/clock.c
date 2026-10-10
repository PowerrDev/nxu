#include <nxu/syscall.h>

/* Compatibility clock import for freestanding GUI frameworks in userland. */
uint64_t timer_get_microseconds(void)
{
	int64_t value = nxu_uptime_us();
	return value < 0 ? 0 : (uint64_t)value;
}
