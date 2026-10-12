#include <frameworks/Recovery.framework/triageOS/ui.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

uint32_t
startup_options_ui_mono_line_height(uint32_t scale)
{
	(void)scale;
	return 22;
}

int
main(void)
{
	static triage_ui_t ui;

	ui.canvas.width = 1280;
	ui.canvas.height = 800;
	ui.terminal_active = true;
	ui.terminal_zoom = 2;
	for (uint32_t i = 0; i < 300; i++) {
		char line[32];

		snprintf(line, sizeof(line), "line %u", i);
		triage_ui_terminal_write(&ui, line);
	}

	assert(ui.terminal_line_count == 256);
	assert(!strcmp(ui.terminal_lines[0], "line 44"));
	assert(!strcmp(ui.terminal_lines[255], "line 299"));
	assert(triage_ui_scroll(&ui, 1) && ui.terminal_scroll_offset == 3);
	assert(triage_ui_scroll(&ui, INT32_MAX) && ui.terminal_scroll_offset < 256);

	uint32_t top = ui.terminal_scroll_offset;

	assert(!triage_ui_scroll(&ui, 1) && ui.terminal_scroll_offset == top);
	assert(triage_ui_scroll(&ui, INT32_MIN) && !ui.terminal_scroll_offset);
	assert(!triage_ui_scroll(&ui, -1));
	assert(triage_ui_scroll(&ui, 10));
	triage_ui_terminal_write(&ui, "new output");
	assert(!ui.terminal_scroll_offset && ui.dirty_terminal_body);
	ui.terminal_active = false;
	assert(!triage_ui_scroll(&ui, 1));
	puts("PASS recovery scrollback retention, bounds, signed wheel and output follow");
}
