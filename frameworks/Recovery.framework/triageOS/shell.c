#include "shell.h"

#include <Recovery/RecoveryServices.h>

#include <stdbool.h>
#include <stdint.h>

#define SHELL_COMMAND_MAX 192U
#define SHELL_ARG_MAX 12U
#define SHELL_LINE_MAX 112U

static bool shell_device_index(const char *name, uint32_t *index_out);

static uint32_t
shell_length(const char *text)
{
	uint32_t length = 0U;
	if (text != 0) {
		while (text[length] != '\0') {
			length++;
		}
	}

	return length;
}

static bool
shell_equal(const char *left, const char *right)
{
	uint32_t index = 0U;
	if (left == 0 || right == 0) {
		return false;
	}

	while (left[index] != '\0' && right[index] != '\0') {
		if (left[index] != right[index]) {
			return false;
		}

		index++;
	}

	return left[index] == right[index];
}

static bool
shell_prefix(const char *text, const char *prefix)
{
	uint32_t index = 0U;
	if (text == 0 || prefix == 0) {
		return false;
	}

	while (prefix[index] != '\0') {
		if (text[index] != prefix[index]) {
			return false;
		}

		index++;
	}

	return true;
}

static bool
shell_copy(char *destination, uint32_t capacity, const char *source)
{
	if (destination == 0 || source == 0 || capacity == 0U) {
		return false;
	}

	uint32_t index = 0U;
	while (source[index] != '\0') {
		if (index + 1U >= capacity) {
			return false;
		}

		destination[index] = source[index];
		index++;
	}

	destination[index] = '\0';
	return true;
}

static bool
shell_append(char *destination, uint32_t capacity, const char *source)
{
	uint32_t length = shell_length(destination);
	uint32_t index = 0U;
	while (source[index] != '\0') {
		if (length + index + 1U >= capacity) {
			return false;
		}

		destination[length + index] = source[index];
		index++;
	}

	destination[length + index] = '\0';
	return true;
}

static void
shell_write(triage_shell_t *shell, const char *text)
{
	if (shell != 0 && shell->write != 0) {
		shell->write(shell->write_context, text);
	}
}

static void
shell_u64(char *buffer, uint32_t capacity, uint64_t value)
{
	char reverse[24];
	uint32_t count = 0U;
	if (capacity == 0U) {
		return;
	}

	if (value == 0ULL) {
		reverse[count++] = '0';
	}

	while (value != 0ULL && count < sizeof(reverse)) {
		reverse[count++] = (char)('0' + value % 10ULL);
		value /= 10ULL;
	}

	uint32_t out = 0U;
	while (count != 0U && out + 1U < capacity) {
		buffer[out++] = reverse[--count];
	}

	buffer[out] = '\0';
}

static bool
shell_u64_parse(const char *text, uint64_t *value_out)
{
	if (text == 0 || value_out == 0 || text[0] == '\0') {
		return false;
	}

	uint64_t value = 0ULL;
	for (uint32_t index = 0U; text[index] != '\0'; index++) {
		if (text[index] < '0' || text[index] > '9') {
			return false;
		}

		uint64_t digit = (uint64_t)(text[index] - '0');
		if (value > (UINT64_MAX - digit) / 10ULL) {
			return false;
		}

		value = value * 10ULL + digit;
	}

	*value_out = value;
	return true;
}


static char
shell_hex_digit(uint32_t value)
{
	return (char)(value < 10U ? '0' + value : 'A' + (value - 10U));
}

static void
shell_hex_u64(char *buffer, uint32_t capacity, uint64_t value)
{
	if (capacity < 3U) {
		return;
	}

	buffer[0] = '0';
	buffer[1] = 'x';
	uint32_t digits = capacity - 3U;
	if (digits > 16U) {
		digits = 16U;
	}

	for (uint32_t i = 0U; i < digits; i++) {
		uint32_t shift = (digits - 1U - i) * 4U;
		buffer[2U + i] = shell_hex_digit((uint32_t)((value >> shift) & 0xFULL));
	}

	buffer[2U + digits] = '\0';
}

static void
shell_error(triage_shell_t *shell, const char *command, int64_t result)
{
	char line[SHELL_LINE_MAX];
	line[0] = '\0';
	(void)shell_append(line, sizeof(line), command);
	(void)shell_append(line, sizeof(line), ": ");
	(void)shell_append(line, sizeof(line), recovery_error_name(result));
	shell_write(shell, line);
}

static uint32_t
shell_split(char *buffer, char *argv[SHELL_ARG_MAX])
{
	uint32_t argc = 0U;
	char *cursor = buffer;
	while (*cursor != '\0' && argc < SHELL_ARG_MAX) {
		while (*cursor == ' ' || *cursor == '\t') {
			cursor++;
		}

		if (*cursor == '\0') {
			break;
		}

		argv[argc++] = cursor;
		while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
			cursor++;
		}

		if (*cursor == '\0') {
			break;
		}

		*cursor++ = '\0';
	}

	return argc;
}

static bool
shell_resolve(const triage_shell_t *shell, const char *input, char output[RECOVERY_FS_PATH_MAX])
{
	char joined[RECOVERY_FS_PATH_MAX];
	joined[0] = '\0';
	if (input == 0 || input[0] == '\0') {
		input = ".";
	}

	if (input[0] == '/') {
		if (!shell_copy(joined, sizeof(joined), input)) {
			return false;
		}
	} else {
		if (!shell_copy(joined, sizeof(joined), shell->cwd)) {
			return false;
		}

		if (!shell_equal(joined, "/") && !shell_append(joined, sizeof(joined), "/")) {
			return false;
		}

		if (!shell_append(joined, sizeof(joined), input)) {
			return false;
		}
	}

	uint32_t starts[64];
	uint32_t depth = 0U;
	uint32_t out = 0U;
	output[out++] = '/';
	uint32_t cursor = 0U;
	while (joined[cursor] != '\0') {
		while (joined[cursor] == '/') {
			cursor++;
		}

		if (joined[cursor] == '\0') {
			break;
		}

		uint32_t start = cursor;
		while (joined[cursor] != '\0' && joined[cursor] != '/') {
			cursor++;
		}

		uint32_t length = cursor - start;
		if (length == 1U && joined[start] == '.') {
			continue;
		}

		if (length == 2U && joined[start] == '.' && joined[start + 1U] == '.') {
			if (depth != 0U) {
				out = starts[--depth];
			}

			if (out == 0U) {
				output[out++] = '/';
			}

			continue;
		}

		if (depth >= 64U || length > RECOVERY_FS_NAME_MAX) {
			return false;
		}

		if (out > 1U && out + 1U >= RECOVERY_FS_PATH_MAX) {
			return false;
		}

		if (out > 1U) {
			output[out++] = '/';
		}

		starts[depth++] = out > 1U ? out - 1U : 1U;
		if (out + length >= RECOVERY_FS_PATH_MAX) {
			return false;
		}

		for (uint32_t i = 0U; i < length; i++) {
			output[out++] = joined[start + i];
		}
	}

	if (out > 1U && output[out - 1U] == '/') {
		out--;
	}

	output[out] = '\0';
	return true;
}

static const char *
shell_type(uint32_t type)
{
	if (type == RECOVERY_FS_TYPE_FILE) {
		return "file";
	}

	if (type == RECOVERY_FS_TYPE_DIRECTORY) {
		return "directory";
	}

	if (type == RECOVERY_FS_TYPE_CHARACTER) {
		return "character";
	}

	if (type == RECOVERY_FS_TYPE_BLOCK) {
		return "block";
	}

	return "unknown";
}

static void
shell_ls(triage_shell_t *shell, const char *argument)
{
	char path[RECOVERY_FS_PATH_MAX];
	if (!shell_resolve(shell, argument, path)) {
		shell_write(shell, "ls: path too long");
		return;
	}

	recovery_fs_stat_t stat;
	int64_t result = recovery_fs_stat(path, &stat);
	if (result < 0) {
		shell_error(shell, "ls", result);
		return;
	}

	if (stat.type != RECOVERY_FS_TYPE_DIRECTORY) {
		shell_write(shell, path);
		return;
	}

	for (uint32_t index = 0U; ; index++) {
		recovery_fs_dirent_t entry;
		result = recovery_fs_readdir(path, index, &entry);
		if (result == -(int64_t)RECOVERY_ERROR_NOT_FOUND) {
			break;
		}

		if (result < 0) {
			shell_error(shell, "ls", result);
			break;
		}

		char line[SHELL_LINE_MAX];
		line[0] = entry.type == RECOVERY_FS_TYPE_DIRECTORY ? 'd' : '-';
		line[1] = ' ';
		line[2] = '\0';
		(void)shell_append(line, sizeof(line), entry.name);
		if (entry.type != RECOVERY_FS_TYPE_DIRECTORY) {
			char size[24];
			shell_u64(size, sizeof(size), entry.size);
			(void)shell_append(line, sizeof(line), "  ");
			(void)shell_append(line, sizeof(line), size);
			(void)shell_append(line, sizeof(line), " B");
		}

		shell_write(shell, line);
	}
}

static void
shell_stat(triage_shell_t *shell, const char *argument)
{
	char path[RECOVERY_FS_PATH_MAX];
	if (!shell_resolve(shell, argument, path)) {
		shell_write(shell, "stat: path too long");
		return;
	}

	recovery_fs_stat_t stat;
	int64_t result = recovery_fs_stat(path, &stat);
	if (result < 0) {
		shell_error(shell, "stat", result);
		return;
	}

	char line[SHELL_LINE_MAX];
	char number[24];
	line[0] = '\0';
	(void)shell_append(line, sizeof(line), "path: ");
	(void)shell_append(line, sizeof(line), path);
	shell_write(shell, line);
	line[0] = '\0';
	(void)shell_append(line, sizeof(line), "type: ");
	(void)shell_append(line, sizeof(line), shell_type(stat.type));
	shell_write(shell, line);
	shell_u64(number, sizeof(number), stat.size);
	line[0] = '\0';
	(void)shell_append(line, sizeof(line), "size: ");
	(void)shell_append(line, sizeof(line), number);
	(void)shell_append(line, sizeof(line), " bytes");
	shell_write(shell, line);
	shell_u64(number, sizeof(number), stat.id);
	line[0] = '\0';
	(void)shell_append(line, sizeof(line), "id: ");
	(void)shell_append(line, sizeof(line), number);
	shell_write(shell, line);
}

static void
shell_cat(triage_shell_t *shell, const char *argument)
{
	char path[RECOVERY_FS_PATH_MAX];
	if (!shell_resolve(shell, argument, path)) {
		shell_write(shell, "cat: path too long");
		return;
	}

	uint8_t buffer[RECOVERY_FS_READ_MAX];
	char line[SHELL_LINE_MAX];
	uint32_t line_length = 0U;
	uint64_t offset = 0ULL;
	for (;;) {
		int64_t result = recovery_fs_read(path, offset, buffer, sizeof(buffer));
		if (result < 0) {
			shell_error(shell, "cat", result);
			return;
		}

		if (result == 0) {
			break;
		}

		for (uint32_t i = 0U; i < (uint32_t)result; i++) {
			char c = (char)buffer[i];
			if (c == '\n' || line_length + 1U >= sizeof(line)) {
				line[line_length] = '\0';
				shell_write(shell, line);
				line_length = 0U;
				if (c == '\n') {
					continue;
				}
			}

			line[line_length++] = (c >= 32 && c <= 126) || c == '\t' ? c : '.';
		}

		offset += (uint64_t)result;
	}

	if (line_length != 0U) {
		line[line_length] = '\0';
		shell_write(shell, line);
	}
}

static void
shell_mounts(triage_shell_t *shell)
{
	for (uint32_t index = 0U; ; index++) {
		recovery_fs_mount_info_t info;
		int64_t result = recovery_fs_mount_info(index, &info);
		if (result == -(int64_t)RECOVERY_ERROR_NOT_FOUND) {
			break;
		}

		if (result < 0) {
			shell_error(shell, "mount", result);
			break;
		}

		char line[SHELL_LINE_MAX];
		line[0] = '\0';
		(void)shell_append(line, sizeof(line), info.device);
		(void)shell_append(line, sizeof(line), " on ");
		(void)shell_append(line, sizeof(line), info.path);
		(void)shell_append(line, sizeof(line), " type ");
		(void)shell_append(line, sizeof(line), info.filesystem);
		if (info.read_only != 0U) {
			(void)shell_append(line, sizeof(line), " (ro)");
		}

		shell_write(shell, line);
	}
}

static void
shell_disks(triage_shell_t *shell)
{
	for (uint32_t index = 0U; ; index++) {
		recovery_block_info_t info;
		int64_t result = recovery_block_info(index, &info);
		if (result == -(int64_t)RECOVERY_ERROR_NOT_FOUND) {
			break;
		}

		if (result < 0) {
			shell_error(shell, "disks", result);
			break;
		}

		char line[SHELL_LINE_MAX];
		char sectors[24];
		shell_u64(sectors, sizeof(sectors), info.sector_count);
		line[0] = '\0';
		(void)shell_append(line, sizeof(line), info.name);
		(void)shell_append(line, sizeof(line), ": ");
		(void)shell_append(line, sizeof(line), sectors);
		(void)shell_append(line, sizeof(line), " sectors");
		if (info.read_only != 0U) {
			(void)shell_append(line, sizeof(line), " ro");
		}

		shell_write(shell, line);
	}
}


static const char *
shell_layout_name(uint32_t scheme)
{
	if (scheme == RECOVERY_BLOCK_LAYOUT_GPT) {
		return "gpt";
	}

	if (scheme == RECOVERY_BLOCK_LAYOUT_MBR) {
		return "mbr";
	}

	return "raw";
}

static void
shell_partitions(triage_shell_t *shell)
{
	for (uint32_t device = 0U; ; device++) {
		recovery_block_info_t disk;
		int64_t result = recovery_block_info(device, &disk);
		if (result == -(int64_t)RECOVERY_ERROR_NOT_FOUND) {
			break;
		}

		if (result < 0) {
			shell_error(shell, "partitions", result);
			break;
		}

		recovery_block_layout_info_t layout;
		result = recovery_block_layout_info(device, &layout);
		if (result < 0) {
			shell_error(shell, "partitions", result);
			continue;
		}

		char line[SHELL_LINE_MAX];
		line[0] = '\0';
		(void)shell_append(line, sizeof(line), disk.name);
		(void)shell_append(line, sizeof(line), ": ");
		(void)shell_append(line, sizeof(line), shell_layout_name(layout.scheme));
		shell_write(shell, line);

		if (layout.partition_count == 0U) {
			shell_write(shell, "  no partition table");
			continue;
		}

		for (uint32_t index = 0U; index < layout.partition_count; index++) {
			recovery_block_partition_info_t partition;
			result = recovery_block_partition_info(device, index, &partition);
			if (result < 0) {
				continue;
			}

			char sectors[24];
			shell_u64(sectors, sizeof(sectors), partition.sector_count);
			line[0] = '\0';
			(void)shell_append(line, sizeof(line), "  ");
			(void)shell_append(line, sizeof(line), partition.name);
			(void)shell_append(line, sizeof(line), "  ");
			(void)shell_append(line, sizeof(line), sectors);
			(void)shell_append(line, sizeof(line), " sectors");
			shell_write(shell, line);
		}
	}
}

static void
shell_disk_health(triage_shell_t *shell, const char *device_name)
{
	uint32_t first = 0U;
	uint32_t last = UINT32_MAX;
	if (device_name != 0) {
		if (!shell_device_index(device_name, &first) || first == RECOVERY_FS_NO_DEVICE) {
			shell_write(shell, "disk-health: device not found");
			return;
		}

		last = first;
	}

	for (uint32_t device = first; device <= last; device++) {
		recovery_block_info_t disk;
		int64_t result = recovery_block_info(device, &disk);
		if (result == -(int64_t)RECOVERY_ERROR_NOT_FOUND) {
			break;
		}

		if (result < 0) {
			shell_error(shell, "disk-health", result);
			break;
		}

		recovery_block_health_info_t health;
		result = recovery_block_health_info(device, &health);
		if (result < 0) {
			shell_error(shell, "disk-health", result);
			continue;
		}

		uint64_t errors = health.read_errors + health.write_errors + health.flush_errors;
		char error_count[24];
		shell_u64(error_count, sizeof(error_count), errors);

		char line[SHELL_LINE_MAX];
		line[0] = '\0';
		(void)shell_append(line, sizeof(line), disk.name);
		(void)shell_append(line, sizeof(line), ": ");
		(void)shell_append(line, sizeof(line), health.healthy != 0U ? "healthy" : "attention");
		(void)shell_append(line, sizeof(line), ", errors ");
		(void)shell_append(line, sizeof(line), error_count);
		(void)shell_append(line, sizeof(line), health.read_only != 0U ? ", read-only" : ", writable");
		(void)shell_append(line, sizeof(line), health.flush_supported != 0U ? ", flush" : ", no-flush");
		shell_write(shell, line);
	}
}

static void
shell_gpt_initialize(triage_shell_t *shell, const char *device_name, bool force)
{
	if (device_name == 0) {
		shell_write(shell, "usage: gpt-init DEVICE --force");
		return;
	}

	if (!force) {
		shell_write(shell, "gpt-init: destructive; add --force to continue");
		return;
	}

	uint32_t device;
	if (!shell_device_index(device_name, &device) || device == RECOVERY_FS_NO_DEVICE) {
		shell_write(shell, "gpt-init: device not found");
		return;
	}

	int64_t result = recovery_block_gpt_initialize(device);
	if (result < 0) {
		shell_error(shell, "gpt-init", result);
		return;
	}

	shell_write(shell, "gpt-init: GPT initialized");
}

static void
shell_partition_create(
	triage_shell_t *shell,
	const char *device_name,
	const char *size_text,
	const char *name,
	const char *role_text
)
{
	if (device_name == 0 || size_text == 0) {
		shell_write(shell, "usage: partition-create DEVICE SECTORS|all [NAME] [data|system|efi]");
		return;
	}

	uint32_t device;
	if (!shell_device_index(device_name, &device) || device == RECOVERY_FS_NO_DEVICE) {
		shell_write(shell, "partition-create: device not found");
		return;
	}

	uint64_t sectors = 0ULL;
	if (!shell_equal(size_text, "all") && (!shell_u64_parse(size_text, &sectors) || sectors == 0ULL)) {
		shell_write(shell, "partition-create: invalid sector count");
		return;
	}

	recovery_block_partition_role_t role = RECOVERY_BLOCK_PARTITION_ROLE_DATA;
	if (role_text != 0) {
		if (shell_equal(role_text, "system")) {
			role = RECOVERY_BLOCK_PARTITION_ROLE_SYSTEM;
		} else if (shell_equal(role_text, "efi")) {
			role = RECOVERY_BLOCK_PARTITION_ROLE_EFI;
		} else if (!shell_equal(role_text, "data")) {
			shell_write(shell, "partition-create: role must be data, system, or efi");
			return;
		}
	}

	recovery_block_partition_info_t partition;
	int64_t result = recovery_block_partition_create(
		device,
		sectors,
		name != 0 ? name : "Untitled",
		role,
		&partition
	);
	if (result < 0) {
		shell_error(shell, "partition-create", result);
		return;
	}

	char start[24];
	char count[24];
	shell_u64(start, sizeof(start), partition.start_sector);
	shell_u64(count, sizeof(count), partition.sector_count);

	char line[SHELL_LINE_MAX];
	line[0] = '\0';
	(void)shell_append(line, sizeof(line), "partition-create: start ");
	(void)shell_append(line, sizeof(line), start);
	(void)shell_append(line, sizeof(line), ", sectors ");
	(void)shell_append(line, sizeof(line), count);
	shell_write(shell, line);
}

static void
shell_partition_delete(
	triage_shell_t *shell,
	const char *device_name,
	const char *index_text,
	bool force
)
{
	if (device_name == 0 || index_text == 0) {
		shell_write(shell, "usage: partition-delete DEVICE INDEX --force");
		return;
	}

	if (!force) {
		shell_write(shell, "partition-delete: destructive; add --force to continue");
		return;
	}

	uint32_t device;
	if (!shell_device_index(device_name, &device) || device == RECOVERY_FS_NO_DEVICE) {
		shell_write(shell, "partition-delete: device not found");
		return;
	}

	uint64_t parsed_index;
	if (!shell_u64_parse(index_text, &parsed_index) || parsed_index > UINT32_MAX) {
		shell_write(shell, "partition-delete: invalid partition index");
		return;
	}

	int64_t result = recovery_block_partition_delete(device, (uint32_t)parsed_index);
	if (result < 0) {
		shell_error(shell, "partition-delete", result);
		return;
	}

	shell_write(shell, "partition-delete: partition deleted");
}

static void
shell_df(triage_shell_t *shell, const char *argument)
{
	char path[RECOVERY_FS_PATH_MAX];
	if (!shell_resolve(shell, argument, path)) {
		shell_write(shell, "df: path too long");
		return;
	}

	recovery_fs_space_info_t info;
	int64_t result = recovery_fs_space_info(path, &info);
	if (result < 0) {
		shell_error(shell, "df", result);
		return;
	}

	char total[24];
	char free_bytes[24];
	shell_u64(total, sizeof(total), info.total_bytes);
	shell_u64(free_bytes, sizeof(free_bytes), info.free_bytes);
	char line[SHELL_LINE_MAX];
	line[0] = '\0';
	(void)shell_append(line, sizeof(line), path);
	(void)shell_append(line, sizeof(line), ": total ");
	(void)shell_append(line, sizeof(line), total);
	(void)shell_append(line, sizeof(line), " B, free ");
	(void)shell_append(line, sizeof(line), free_bytes);
	(void)shell_append(line, sizeof(line), " B");
	shell_write(shell, line);
}


static void
shell_hexdump(triage_shell_t *shell, const char *argument)
{
	char path[RECOVERY_FS_PATH_MAX];
	if (!shell_resolve(shell, argument, path)) {
		shell_write(shell, "hexdump: path too long");
		return;
	}

	uint8_t buffer[64];
	uint64_t offset = 0ULL;
	for (;;) {
		int64_t result = recovery_fs_read(path, offset, buffer, sizeof(buffer));
		if (result < 0) {
			shell_error(shell, "hexdump", result);
			return;
		}

		if (result == 0) {
			return;
		}

		for (uint32_t base = 0U; base < (uint32_t)result; base += 16U) {
			char line[SHELL_LINE_MAX];
			char address[19];
			shell_hex_u64(address, sizeof(address), offset + base);
			line[0] = '\0';
			(void)shell_append(line, sizeof(line), address);
			(void)shell_append(line, sizeof(line), "  ");
			uint32_t end = base + 16U;
			if (end > (uint32_t)result) {
				end = (uint32_t)result;
			}

			for (uint32_t i = base; i < end; i++) {
				char byte[4] = { shell_hex_digit(buffer[i] >> 4U), shell_hex_digit(buffer[i] & 0xFU), ' ', '\0' };
				(void)shell_append(line, sizeof(line), byte);
			}

			shell_write(shell, line);
		}

		offset += (uint64_t)result;
	}
}

static void
shell_tree_path(triage_shell_t *shell, const char *path, uint32_t depth)
{
	if (depth > 6U) {
		shell_write(shell, "  ...");
		return;
	}

	for (uint32_t index = 0U; ; index++) {
		recovery_fs_dirent_t entry;
		int64_t result = recovery_fs_readdir(path, index, &entry);
		if (result == -(int64_t)RECOVERY_ERROR_NOT_FOUND) {
			break;
		}

		if (result < 0) {
			shell_error(shell, "tree", result);
			break;
		}

		if (shell_equal(entry.name, ".") || shell_equal(entry.name, "..")) {
			continue;
		}

		char line[SHELL_LINE_MAX];
		uint32_t out = 0U;
		for (uint32_t i = 0U; i < depth && out + 2U < sizeof(line); i++) {
			line[out++] = ' ';
			line[out++] = ' ';
		}

		for (uint32_t i = 0U; entry.name[i] != '\0' && out + 1U < sizeof(line); i++) {
			line[out++] = entry.name[i];
		}

		if (entry.type == RECOVERY_FS_TYPE_DIRECTORY && out + 1U < sizeof(line)) {
			line[out++] = '/';
		}

		line[out] = '\0';
		shell_write(shell, line);
		if (entry.type != RECOVERY_FS_TYPE_DIRECTORY) {
			continue;
		}

		char child[RECOVERY_FS_PATH_MAX];
		if (!shell_copy(child, sizeof(child), path)) {
			continue;
		}

		if (!shell_equal(child, "/")) {
			(void)shell_append(child, sizeof(child), "/");
		}

		if (!shell_append(child, sizeof(child), entry.name)) {
			continue;
		}

		shell_tree_path(shell, child, depth + 1U);
	}
}

static void
shell_tree(triage_shell_t *shell, const char *argument)
{
	char path[RECOVERY_FS_PATH_MAX];
	if (!shell_resolve(shell, argument, path)) {
		shell_write(shell, "tree: path too long");
		return;
	}

	recovery_fs_stat_t stat;
	int64_t result = recovery_fs_stat(path, &stat);
	if (result < 0) {
		shell_error(shell, "tree", result);
		return;
	}

	shell_write(shell, path);
	if (stat.type == RECOVERY_FS_TYPE_DIRECTORY) {
		shell_tree_path(shell, path, 1U);
	}
}

static void
shell_fsinfo(triage_shell_t *shell)
{
	shell_write(shell, "mounts:");
	shell_mounts(shell);
	shell_write(shell, "devices:");
	shell_disks(shell);
}

static bool
shell_device_index(const char *name, uint32_t *index_out)
{
	if (name == 0 || index_out == 0) {
		return false;
	}

	if (shell_equal(name, "none")) {
		*index_out = RECOVERY_FS_NO_DEVICE;
		return true;
	}

	for (uint32_t index = 0U; ; index++) {
		recovery_block_info_t info;
		int64_t result = recovery_block_info(index, &info);
		if (result < 0) {
			break;
		}

		if (shell_equal(info.name, name)) {
			*index_out = index;
			return true;
		}
	}

	return false;
}

static int64_t
shell_mount_system_volume(void)
{
	for (uint32_t index = 0U; ; index++) {
		recovery_fs_mount_info_t mount;
		if (recovery_fs_mount_info(index, &mount) < 0) break;
		if (shell_equal(mount.device, "disk0p2")) return 0;
	}

	uint32_t device;
	if (!shell_device_index("disk0p2", &device)) return -(int64_t)RECOVERY_ERROR_NOT_FOUND;

	return recovery_fs_mount("ext4", device, "/disk");
}

static bool
shell_protected_path(const char *path)
{
	return shell_equal(path, "/") || shell_equal(path, "/System") || shell_prefix(path, "/System/") ||
		shell_equal(path, "/init") || shell_prefix(path, "/init/") || shell_equal(path, "/recovery") ||
		shell_prefix(path, "/recovery/") || shell_equal(path, "/disk") ||
		shell_equal(path, "/disk/System") || shell_prefix(path, "/disk/System/") ||
		shell_equal(path, "/disk/init") || shell_prefix(path, "/disk/init/");
}

/* Create each missing component, matching mkdir -p in the normal userspace. */
static int64_t
shell_mkdir_parents(const char *path)
{
	char component[RECOVERY_FS_PATH_MAX];
	if (!shell_copy(component, sizeof(component), path)) {
		return -(int64_t)RECOVERY_ERROR_INVALID_ARGUMENT;
	}

	for (uint32_t index = 1U; ; index++) {
		if (component[index] != '/' && component[index] != '\0') {
			continue;
		}

		char saved = component[index];
		component[index] = '\0';
		int64_t result = recovery_fs_mkdir(component);
		if (result < 0 && result != -(int64_t)RECOVERY_ERROR_EXISTS) {
			return result;
		}

		component[index] = saved;
		if (saved == '\0') {
			return 0;
		}
	}
}

static bool
shell_join_child(char output[RECOVERY_FS_PATH_MAX], const char *parent, const char *child)
{
	output[0] = '\0';
	if (!shell_copy(output, RECOVERY_FS_PATH_MAX, parent)) {
		return false;
	}

	if (!shell_equal(parent, "/") && !shell_append(output, RECOVERY_FS_PATH_MAX, "/")) {
		return false;
	}

	return shell_append(output, RECOVERY_FS_PATH_MAX, child);
}

/* Recursively empty directories before removing the directory inode itself. */
static int64_t
shell_remove_recursive(const char *path)
{
	recovery_fs_stat_t stat;
	int64_t result = recovery_fs_stat(path, &stat);
	if (result < 0) {
		return result;
	}

	if (stat.type != RECOVERY_FS_TYPE_DIRECTORY) {
		return recovery_fs_unlink(path);
	}

	for (;;) {
		recovery_fs_dirent_t entry;
		result = recovery_fs_readdir(path, 0U, &entry);
		if (result == -(int64_t)RECOVERY_ERROR_NOT_FOUND) {
			break;
		}

		if (result < 0) {
			return result;
		}

		if (shell_equal(entry.name, ".") || shell_equal(entry.name, "..")) {
			return -(int64_t)RECOVERY_ERROR_IO;
		}

		char child[RECOVERY_FS_PATH_MAX];
		if (!shell_join_child(child, path, entry.name)) {
			return -(int64_t)RECOVERY_ERROR_INVALID_ARGUMENT;
		}

		result = shell_remove_recursive(child);
		if (result < 0) {
			return result;
		}
	}

	return recovery_fs_rmdir(path);
}

void
triage_shell_init(triage_shell_t *shell, triage_shell_write_fn write, void *context)
{
	if (shell == 0) {
		return;
	}

	shell->cwd[0] = '/';
	shell->cwd[1] = '\0';
	shell->write = write;
	shell->write_context = context;
}

triage_shell_result_t
triage_shell_execute(triage_shell_t *shell, const char *command)
{
	if (shell == 0 || command == 0) {
		return TRIAGE_SHELL_OK;
	}

	char buffer[SHELL_COMMAND_MAX];
	if (!shell_copy(buffer, sizeof(buffer), command)) {
		shell_write(shell, "shell: command too long");
		return TRIAGE_SHELL_OK;
	}

	char *argv[SHELL_ARG_MAX];
	uint32_t argc = shell_split(buffer, argv);
	if (argc == 0U) {
		return TRIAGE_SHELL_OK;
	}

	if (shell_equal(argv[0], "help")) {
		shell_write(shell, "help clear echo pwd cd ls tree stat cat hexdump");
		shell_write(shell, "mkdir touch write rm sync mount umount");
		shell_write(shell, "mounts disks partitions disk-health df fsinfo");
		shell_write(shell, "gpt-init partition-create partition-delete reinstall version uname reboot exit");
		return TRIAGE_SHELL_OK;
	}

	if (shell_equal(argv[0], "clear")) {
		return TRIAGE_SHELL_CLEAR;
	}

	if (shell_equal(argv[0], "echo")) {
		char line[SHELL_LINE_MAX]; line[0] = '\0';
		for (uint32_t i = 1U; i < argc; i++) {
			if (i != 1U) {
				(void)shell_append(line, sizeof(line), " ");
			}

			(void)shell_append(line, sizeof(line), argv[i]);
		}

		shell_write(shell, line);
		return TRIAGE_SHELL_OK;
	}

	if (shell_equal(argv[0], "pwd")) {
		shell_write(shell, shell->cwd);
		return TRIAGE_SHELL_OK;
	}

	if (shell_equal(argv[0], "cd")) {
		char path[RECOVERY_FS_PATH_MAX];
		if (!shell_resolve(shell, argc > 1U ? argv[1] : "/", path)) {
			shell_write(shell, "cd: path too long");
			return TRIAGE_SHELL_OK;
		}

		recovery_fs_stat_t stat;
		int64_t result = recovery_fs_stat(path, &stat);
		if (result < 0) {
			shell_error(shell, "cd", result);
		} else if (stat.type != RECOVERY_FS_TYPE_DIRECTORY) {
			shell_write(shell, "cd: not a directory");
		} else {
			(void)shell_copy(shell->cwd, sizeof(shell->cwd), path);
		}

		return TRIAGE_SHELL_OK;
	}

	if (shell_equal(argv[0], "ls")) {
		shell_ls(shell, argc > 1U ? argv[1] : ".");
		return TRIAGE_SHELL_OK;
	}

	if (shell_equal(argv[0], "tree")) {
		shell_tree(shell, argc > 1U ? argv[1] : ".");
		return TRIAGE_SHELL_OK;
	}

	if (shell_equal(argv[0], "stat")) {
		if (argc < 2U) {
			shell_write(shell, "usage: stat PATH");
		} else {
			shell_stat(shell, argv[1]);
		}

		return TRIAGE_SHELL_OK;
	}

	if (shell_equal(argv[0], "cat")) {
		if (argc < 2U) {
			shell_write(shell, "usage: cat PATH");
		} else {
			shell_cat(shell, argv[1]);
		}

		return TRIAGE_SHELL_OK;
	}

	if (shell_equal(argv[0], "hexdump")) {
		if (argc < 2U) {
			shell_write(shell, "usage: hexdump PATH");
		} else {
			shell_hexdump(shell, argv[1]);
		}

		return TRIAGE_SHELL_OK;
	}

	if (shell_equal(argv[0], "mkdir") || shell_equal(argv[0], "touch")) {
		bool parents = shell_equal(argv[0], "mkdir") && argc > 1U && shell_equal(argv[1], "-p");
		uint32_t path_index = parents ? 2U : 1U;
		if (argc <= path_index) {
			shell_write(shell, shell_equal(argv[0], "mkdir") ? "usage: mkdir [-p] PATH" : "usage: touch PATH");
			return TRIAGE_SHELL_OK;
		}

		char path[RECOVERY_FS_PATH_MAX];
		if (!shell_resolve(shell, argv[path_index], path)) {
			shell_write(shell, "path too long");
			return TRIAGE_SHELL_OK;
		}

		int64_t result = shell_equal(argv[0], "mkdir") ? (parents ? shell_mkdir_parents(path) : recovery_fs_mkdir(path)) : recovery_fs_touch(path);
		if (result < 0) {
			shell_error(shell, argv[0], result);
		}

		return TRIAGE_SHELL_OK;
	}

	if (shell_equal(argv[0], "write")) {
		bool force_system = argc > 1U && shell_equal(argv[1], "--force-system");
		uint32_t path_index = force_system ? 2U : 1U;
		uint32_t text_index = path_index + 1U;
		if (argc <= text_index) {
			shell_write(shell, "usage: write [--force-system] PATH TEXT...");
			return TRIAGE_SHELL_OK;
		}

		char path[RECOVERY_FS_PATH_MAX];
		if (!shell_resolve(shell, argv[path_index], path)) {
			shell_write(shell, "write: path too long");
			return TRIAGE_SHELL_OK;
		}

		if (shell_protected_path(path) && !force_system) {
			shell_write(shell, "warning: this command may break your system or destroy data. you have been warned.");
			shell_write(shell, "write: add --force-system to continue");
			return TRIAGE_SHELL_OK;
		}

		char data[RECOVERY_FS_READ_MAX]; data[0] = '\0';
		for (uint32_t i = text_index; i < argc; i++) {
			if (i != text_index) {
				(void)shell_append(data, sizeof(data), " ");
			}

			if (!shell_append(data, sizeof(data), argv[i])) {
				shell_write(shell, "write: data too long");
				return TRIAGE_SHELL_OK;
			}
		}

		int64_t result = recovery_fs_write(path, data, shell_length(data));
		if (result < 0) {
			shell_error(shell, "write", result);
		}

		return TRIAGE_SHELL_OK;
	}

	if (shell_equal(argv[0], "rm")) {
		bool recursive = false;
		bool force_system = false;
		const char *target = 0;
		for (uint32_t i = 1U; i < argc; i++) {
			if (shell_equal(argv[i], "-r") || shell_equal(argv[i], "-rf") || shell_equal(argv[i], "-fr")) {
				recursive = true;
			} else if (shell_equal(argv[i], "-f")) {
				continue;
			} else if (shell_equal(argv[i], "--force-system")) {
				force_system = true;
			} else {
				target = argv[i];
			}
		}

		if (target == 0) {
			shell_write(shell, "usage: rm [-rf] [--force-system] PATH");
			return TRIAGE_SHELL_OK;
		}

		char path[RECOVERY_FS_PATH_MAX];
		if (!shell_resolve(shell, target, path)) {
			shell_write(shell, "rm: path too long");
			return TRIAGE_SHELL_OK;
		}

		if (shell_protected_path(path)) {
			shell_write(shell, "warning: this command may break your system or destroy data. you have been warned.");
			if (!force_system) {
				shell_write(shell, "rm: add --force-system to continue");
				return TRIAGE_SHELL_OK;
			}
		}

		int64_t result = recursive ? shell_remove_recursive(path) : recovery_fs_unlink(path);
		if (result < 0) {
			shell_error(shell, "rm", result);
		}

		return TRIAGE_SHELL_OK;
	}

	if (shell_equal(argv[0], "sync")) {
		int64_t result = recovery_fs_sync();
		if (result < 0) {
			shell_error(shell, "sync", result);
		} else {
			shell_write(shell, "filesystems synced");
		}

		return TRIAGE_SHELL_OK;
	}

	if (shell_equal(argv[0], "mount") || shell_equal(argv[0], "mounts")) {
		if (argc == 1U || shell_equal(argv[0], "mounts")) {
			shell_mounts(shell);
			return TRIAGE_SHELL_OK;
		}

		if (argc != 4U) {
			shell_write(shell, "usage: mount TYPE DEVICE|none PATH");
			return TRIAGE_SHELL_OK;
		}

		uint32_t device;
		if (!shell_device_index(argv[2], &device)) {
			shell_write(shell, "mount: device not found");
			return TRIAGE_SHELL_OK;
		}

		char path[RECOVERY_FS_PATH_MAX];
		if (!shell_resolve(shell, argv[3], path)) {
			shell_write(shell, "mount: path too long");
			return TRIAGE_SHELL_OK;
		}

		int64_t result = recovery_fs_mount(argv[1], device, path);
		if (result < 0) {
			shell_error(shell, "mount", result);
		}

		return TRIAGE_SHELL_OK;
	}

	if (shell_equal(argv[0], "umount")) {
		if (argc < 2U) {
			shell_write(shell, "usage: umount PATH");
			return TRIAGE_SHELL_OK;
		}

		char path[RECOVERY_FS_PATH_MAX];
		if (!shell_resolve(shell, argv[1], path)) {
			shell_write(shell, "umount: path too long");
			return TRIAGE_SHELL_OK;
		}

		int64_t result = recovery_fs_unmount(path);
		if (result < 0) {
			shell_error(shell, "umount", result);
		}

		return TRIAGE_SHELL_OK;
	}

	if (shell_equal(argv[0], "disks")) {
		shell_disks(shell);
		return TRIAGE_SHELL_OK;
	}

	if (shell_equal(argv[0], "partitions")) {
		shell_partitions(shell);
		return TRIAGE_SHELL_OK;
	}

	if (shell_equal(argv[0], "disk-health")) {
		shell_disk_health(shell, argc > 1U ? argv[1] : 0);
		return TRIAGE_SHELL_OK;
	}

	if (shell_equal(argv[0], "gpt-init")) {
		shell_gpt_initialize(
			shell,
			argc > 1U ? argv[1] : 0,
			argc > 2U && shell_equal(argv[2], "--force")
		);
		return TRIAGE_SHELL_OK;
	}

	if (shell_equal(argv[0], "partition-create")) {
		shell_partition_create(
			shell,
			argc > 1U ? argv[1] : 0,
			argc > 2U ? argv[2] : 0,
			argc > 3U ? argv[3] : 0,
			argc > 4U ? argv[4] : 0
		);
		return TRIAGE_SHELL_OK;
	}

	if (shell_equal(argv[0], "partition-delete")) {
		shell_partition_delete(
			shell,
			argc > 1U ? argv[1] : 0,
			argc > 2U ? argv[2] : 0,
			argc > 3U && shell_equal(argv[3], "--force")
		);
		return TRIAGE_SHELL_OK;
	}

	if (shell_equal(argv[0], "df")) {
		shell_df(shell, argc > 1U ? argv[1] : ".");
		return TRIAGE_SHELL_OK;
	}
		if (shell_equal(argv[0], "fsinfo")) {
			shell_fsinfo(shell);
			return TRIAGE_SHELL_OK;
		}
	if (shell_equal(argv[0], "reinstall")) {
		shell_write(shell, "restoring system files...");
		int64_t result = shell_mount_system_volume();
		if (result >= 0) {
			result = recovery_fs_copy("/recovery/System/Recovery/BaseSystem/launchd", "/disk/init/launchd");
		}

		if (result >= 0) {
			result = recovery_fs_sync();
		}

		if (result < 0) {
			shell_error(shell, "reinstall", result);
		} else {
			shell_write(shell, "sevOS restored; reboot to continue");
		}

		return TRIAGE_SHELL_OK;
	}

	if (shell_equal(argv[0], "version") || shell_equal(argv[0], "uname")) {
		char version[64];
		int64_t result = recovery_get_version(version, sizeof(version));
		if (result < 0) {
			shell_error(shell, "version", result);
		} else {
			shell_write(shell, version);
		}

		return TRIAGE_SHELL_OK;
	}

	if (shell_equal(argv[0], "reboot")) {
		shell_write(shell, "syncing filesystems...");
		int64_t result = recovery_system_reset();
		if (result < 0) {
			shell_error(shell, "reboot", result);
		}

		return TRIAGE_SHELL_OK;
	}

	if (shell_equal(argv[0], "exit")) {
		return TRIAGE_SHELL_EXIT;
	}

	shell_write(shell, "command not found; type help");
	return TRIAGE_SHELL_OK;
}
