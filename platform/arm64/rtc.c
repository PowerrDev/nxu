#include <platform/rtc.h>
#include <platform/platform.h>
#include <vm/vmm.h>

#include <stdbool.h>
#include <stdint.h>

#define RTC_DR_OFFSET 0x00ULL

static volatile uint32_t *g_rtc_dr;
static bool g_rtc_ready;

bool rtc_init(void)
{
	if (g_rtc_ready) return true;

	/*
	 * Unlike UART/DTB/GIC, nothing before this needs the RTC, so there is
	 * no separate "early physical, later higher-half" phase to support --
	 * this always runs after kern_prepare_higher_half_runtime(), by which
	 * point platform_get() and the TTBR1 direct map both already cover
	 * the PL031 region discovered from the Device Tree ("arm,pl031"; see
	 * platform_property()/platform_end_node() in platform/arm64/platform.c
	 * and vmm_map_higher_half_direct_map() in vm/vmm_ttbr1.c).
	 */
	const platform_t *platform = platform_get();
	if (platform == 0 || platform->rtc.size == 0ULL) return false;

	uint64_t virtual_address;
	if (!vmm_physical_to_higher_half(platform->rtc.base, &virtual_address)) return false;

	g_rtc_dr = (volatile uint32_t *)(virtual_address + RTC_DR_OFFSET);
	g_rtc_ready = true;
	return true;
}

uint64_t rtc_unix_time(void)
{
	if (!g_rtc_ready) return 0ULL;

	/* PL031's DR is a free-running seconds-since-epoch counter -- no
	 * latch/read-twice dance needed for a single 32-bit register. */
	return (uint64_t)*g_rtc_dr;
}
