/* A display-capable process must still be excluded by trusted UI. */
#include <nxu/gui_host_protocol.h>
#include <nxu/string.h>
#include <nxu/syscall.h>

int main(void)
{
	bool blocked = false;
	uint32_t pixel = 0xffff0000;

	for (;;) {
		nxu_recovery_input_event_t event;
		int64_t input = nxu_recovery_input(&event);

		if (input == -NXU_SYS_E_DENIED) {
			nxu_gui_host_hello_t hello = {0};
			hello.header = (nxu_gui_host_header_t){sizeof(hello), 1, 1, NXU_GUI_HOST_HELLO, 0, 0, 1};
			hello.supported_features = hello.required_features = NXU_GUI_HOST_FEATURE_LEASE;
			hello.max_record_size = NXU_GUI_HOST_MAX_RECORD;

			int64_t present = nxu_recovery_present(&pixel, 1, 0, 0, 1, 1);
			hello.header.operation = NXU_GUI_HOST_DISPLAY_ATTACH;
			int64_t host = nxu_gui_host(&hello, sizeof(hello));

			/* The overlay may finish between calls. Only compare denials while
			 * input is still excluded; none of these probes can claim a lease. */
			if (nxu_recovery_input(&event) == -NXU_SYS_E_DENIED &&
				(present != -NXU_SYS_E_DENIED || host != -NXU_SYS_E_DENIED)) {
				static const char failure[] = "trusted-probe: FAILED exclusion\n";
				(void)nxu_write(1, failure, sizeof(failure) - 1);
				return 1;
			}

			if (!blocked) {
				static const char message[] = "trusted-probe: scanout/input/lease/host excluded\n";
				(void)nxu_write(1, message, sizeof(message) - 1);
			}

			blocked = true;
		} else {
			blocked = false;
		}

		(void)nxu_sleep_us(10000);
	}
}
