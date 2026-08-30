#ifndef RECOVERY_USER_TRIAGEOS_UI_H
#define RECOVERY_USER_TRIAGEOS_UI_H

#include <Recovery/RecoveryServices.h>
#include <Recovery/drawing.h>
#include "shell.h"

#include <stdbool.h>
#include <stdint.h>

#define TRIAGE_TERMINAL_INPUT_MAX 160U
#define TRIAGE_TERMINAL_LINES 22U
#define TRIAGE_TERMINAL_LINE_MAX 112U
#define TRIAGE_TERMINAL_HISTORY 8U
#define TRIAGE_DISK_PARTITIONS_MAX 8U
#define TRIAGE_SHEET_NAME_MAX (RECOVERY_BLOCK_PARTITION_NAME_MAX + 1U)
#define TRIAGE_SHEET_SIZE_MAX 12U
#define TRIAGE_SHEET_MESSAGE_MAX 96U

typedef enum {
	TRIAGE_ACTION_NONE = 0,
	TRIAGE_ACTION_REINSTALL,
	TRIAGE_ACTION_DISK_UTILITY,
	TRIAGE_ACTION_TERMINAL,
	TRIAGE_ACTION_RESTART
} triage_action_t;

typedef struct {
	int32_t x;
	int32_t y;
	uint32_t width;
	uint32_t height;
	triage_action_t action;
	const char *title;
	const char *detail;
} triage_button_t;

typedef struct {
	int32_t x;
	int32_t y;
	uint32_t width;
	uint32_t height;
	bool full;
	bool valid;
} triage_damage_t;

typedef struct {
	int32_t x;
	int32_t y;
	uint32_t width;
	uint32_t height;
} triage_rect_t;

/*
 * Action buttons along the bottom of Disk Utility, left to right.
 *
 * They are indexed rather than named so that hover and press state, hit
 * testing and the per-button dirty mask can all be driven by one loop; the
 * order here is the order they are laid out and drawn in.
 */
typedef enum {
	TRIAGE_DISK_BUTTON_REFRESH = 0,
	TRIAGE_DISK_BUTTON_VERIFY,
	TRIAGE_DISK_BUTTON_DELETE,
	TRIAGE_DISK_BUTTON_ADD,
	TRIAGE_DISK_BUTTON_PRIMARY,
	TRIAGE_DISK_BUTTON_COUNT
} triage_disk_button_t;

/*
 * Controls of the Add Partition sheet.
 *
 * Hover, press and focus are all expressed as one of these rather than as a
 * bool per control, because at most one control is ever in each of those
 * states and a single field keeps the event handlers from drifting apart.
 */
typedef enum {
	TRIAGE_SHEET_CONTROL_NONE = 0,
	TRIAGE_SHEET_CONTROL_NAME,
	TRIAGE_SHEET_CONTROL_SIZE,
	TRIAGE_SHEET_CONTROL_UNIT_MIB,
	TRIAGE_SHEET_CONTROL_UNIT_GIB,
	TRIAGE_SHEET_CONTROL_ROLE_DATA,
	TRIAGE_SHEET_CONTROL_ROLE_SYSTEM,
	TRIAGE_SHEET_CONTROL_ROLE_EFI,
	TRIAGE_SHEET_CONTROL_SLIDER,
	TRIAGE_SHEET_CONTROL_USE_ALL,
	TRIAGE_SHEET_CONTROL_CANCEL,
	TRIAGE_SHEET_CONTROL_APPLY
} triage_sheet_control_t;

/*
 * State of the Add Partition sheet.
 *
 * `size_text` is the source of truth while the field has focus so that typing
 * never fights a rounded byte count; every other consumer goes through the
 * parsed byte value.  The rectangles are recomputed by the layout pass and are
 * shared by drawing and hit testing so the two cannot disagree.
 */
typedef struct {
	bool active;
	char name[TRIAGE_SHEET_NAME_MAX];
	uint32_t name_length;
	char size_text[TRIAGE_SHEET_SIZE_MAX];
	uint32_t size_length;
	bool gibibytes;
	bool use_all;
	uint32_t role;
	uint64_t sector_size;
	uint64_t free_sectors;
	uint64_t disk_sectors;
	uint64_t used_sectors;
	triage_sheet_control_t focus;
	triage_sheet_control_t hovered;
	triage_sheet_control_t pressed;
	bool dragging;
	char message[TRIAGE_SHEET_MESSAGE_MAX];
	triage_rect_t frame;
	triage_rect_t name_field;
	triage_rect_t size_field;
	triage_rect_t unit_mib;
	triage_rect_t unit_gib;
	triage_rect_t role_data;
	triage_rect_t role_system;
	triage_rect_t role_efi;
	triage_rect_t slider;
	triage_rect_t use_all_box;
	triage_rect_t cancel;
	triage_rect_t apply;
} triage_disk_sheet_t;

typedef struct {
	startup_options_ui_canvas_t canvas;
	triage_button_t buttons[4];
	int32_t selected_button;
	int32_t hovered_button;
	int32_t pressed_button;
	int32_t panel_x;
	int32_t panel_y;
	uint32_t panel_width;
	uint32_t panel_height;
	int32_t continue_button_x;
	int32_t continue_button_y;
	uint32_t continue_button_width;
	uint32_t continue_button_height;
	bool continue_button_hovered;
	bool continue_button_pressed;
	triage_action_t modal_action;
	triage_action_t pending_action;
	int32_t modal_button_x;
	int32_t modal_button_y;
	uint32_t modal_button_width;
	uint32_t modal_button_height;
	bool modal_button_hovered;
	bool modal_button_pressed;
	bool modal_custom;
	char modal_title[64];
	char modal_message[160];
	bool terminal_active;
	uint32_t terminal_zoom;
	bool disk_active;
	recovery_block_info_t disk_info;
	recovery_block_health_info_t disk_health;
	bool disk_health_valid;
	recovery_block_layout_info_t disk_layout;
	recovery_block_partition_info_t disk_partitions[TRIAGE_DISK_PARTITIONS_MAX];
	uint32_t disk_partition_count;
	uint32_t disk_selected_partition;
	triage_disk_sheet_t disk_sheet;
	bool disk_initialize_armed;
	bool disk_delete_armed;
	recovery_fs_space_info_t disk_space;
	bool disk_space_valid;
	bool disk_mounted;
	char disk_mount_path[RECOVERY_FS_PATH_MAX];
	char disk_status[96];
	triage_rect_t disk_buttons[TRIAGE_DISK_BUTTON_COUNT];
	bool disk_button_hovered[TRIAGE_DISK_BUTTON_COUNT];
	bool disk_button_pressed[TRIAGE_DISK_BUTTON_COUNT];
	char terminal_input[TRIAGE_TERMINAL_INPUT_MAX];
	uint32_t terminal_input_length;
	char terminal_lines[TRIAGE_TERMINAL_LINES][TRIAGE_TERMINAL_LINE_MAX];
	uint32_t terminal_line_count;
	char terminal_history[TRIAGE_TERMINAL_HISTORY][TRIAGE_TERMINAL_INPUT_MAX];
	uint32_t terminal_history_count;
	int32_t terminal_history_index;
	triage_shell_t shell;
	uint8_t dirty_buttons;
	bool dirty_continue;
	bool dirty_modal;
	bool dirty_disk;
	uint8_t dirty_disk_buttons;
	bool dirty_sheet;
	bool dirty_terminal_body;
	bool dirty_terminal_input;
	bool full_redraw;
	triage_damage_t damage;
} triage_ui_t;

bool triage_ui_init(triage_ui_t *ui, uint32_t *pixels, uint32_t width, uint32_t height, uint32_t stride);
void triage_ui_render(triage_ui_t *ui);
void triage_ui_force_redraw(triage_ui_t *ui);
bool triage_ui_pointer_move(triage_ui_t *ui, int32_t x, int32_t y);
bool triage_ui_pointer_down(triage_ui_t *ui, int32_t x, int32_t y);
bool triage_ui_pointer_up(triage_ui_t *ui, int32_t x, int32_t y);
bool triage_ui_key(triage_ui_t *ui, uint32_t code, int32_t value, uint32_t modifiers);
bool triage_ui_take_damage(triage_ui_t *ui, triage_damage_t *damage);
triage_action_t triage_ui_take_action(triage_ui_t *ui);
void triage_ui_show_modal(triage_ui_t *ui, triage_action_t action);
void triage_ui_show_message(triage_ui_t *ui, const char *title, const char *message);
void triage_ui_terminal_write(triage_ui_t *ui, const char *text);

#endif
