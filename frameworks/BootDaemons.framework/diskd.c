/*
 * diskd: the disks, their partitions, and mounting.
 *
 * DISK and PARTITION describe the block devices and their partition tables
 * to anyone; MOUNT and UNMOUNT change what is mounted, for root only, and
 * never take the root file system or the system volume away. It takes
 * NXU_CAP_STORAGE (AllowStorage in its service plist).
 */
#include <nxu/services.h>
#include <nxu/string.h>
#include <nxu/syscall.h>

#define SYSTEM_VOLUME "/Volumes/mainOS"

static void
log_line(const char *first, const char *second)
{
	(void)nxu_write(1, "diskd: ", 7);
	(void)nxu_write(1, first, nxu_strlen(first));
	if (second) (void)nxu_write(1, second, nxu_strlen(second));
	(void)nxu_write(1, "\n", 1);
}

/* Somewhere under /Volumes, one name deep: /Volumes/<name>. */
static bool
mount_point_valid(const char *path)
{
	const char *prefix = "/Volumes/";
	uint64_t prefix_length = nxu_strlen(prefix), length = nxu_strlen(path);

	if (length <= prefix_length || length >= 64U) return false;
	for (uint64_t index = 0; index < prefix_length; index++)
		if (path[index] != prefix[index]) return false;
	for (uint64_t index = prefix_length; index < length; index++)
		if (path[index] == '/' || (path[index] == '.' && index == prefix_length)) return false;
	return !nxu_streq(path, SYSTEM_VOLUME);
}

static void
handle(nxu_service_message_t *message, const nxu_ipc_peer_t *peer)
{
	message->length = 0;

	switch (message->operation) {
	case NXU_DISKD_DISK: {
		nxu_recovery_block_info_t block;
		nxu_recovery_block_layout_info_t layout = {0};

		if (message->arguments[0] > UINT32_MAX) {
			message->status = (int32_t)-NXU_SYS_E_INVALID_ARGUMENT;
			break;
		}
		int64_t status = nxu_recovery_block_info((uint32_t)message->arguments[0], &block);

		if (status < 0) {
			message->status = (int32_t)status;
			break;
		}
		(void)nxu_recovery_block_layout_info((uint32_t)message->arguments[0], &layout);

		nxu_disk_info_t info = {0};

		nxu_service_copy(info.name, sizeof(info.name), block.name);
		info.sector_count = block.sector_count;
		info.sector_size = block.sector_size;
		info.read_only = block.read_only;
		info.scheme = layout.scheme;
		info.partition_count = layout.partition_count;
		memcpy(message->data, &info, sizeof(info));
		message->length = sizeof(info);
		break;
	}
	case NXU_DISKD_PARTITION: {
		nxu_recovery_block_partition_info_t partition;

		if (message->arguments[0] > UINT32_MAX || message->arguments[1] > UINT32_MAX) {
			message->status = (int32_t)-NXU_SYS_E_INVALID_ARGUMENT;
			break;
		}
		int64_t status = nxu_recovery_block_partition_info((uint32_t)message->arguments[0],
															 (uint32_t)message->arguments[1], &partition);

		if (status < 0) {
			message->status = (int32_t)status;
			break;
		}

		nxu_partition_info_t info = {0};

		nxu_service_copy(info.name, sizeof(info.name), partition.name);
		info.start_sector = partition.start_sector;
		info.sector_count = partition.sector_count;
		info.index = partition.index;
		info.type = partition.type;
		memcpy(message->data, &info, sizeof(info));
		message->length = sizeof(info);
		break;
	}
	case NXU_DISKD_MOUNT:
	case NXU_DISKD_UNMOUNT: {
		if (peer->uid != 0U) {
			message->status = (int32_t)-NXU_SYS_E_DENIED;
			break;
		}
		if (!mount_point_valid(message->name)) {
			message->status = (int32_t)-NXU_SYS_E_INVALID_ARGUMENT;
			break;
		}

		int64_t status;

		if (message->operation == NXU_DISKD_MOUNT) {
			if (message->arguments[0] > UINT32_MAX || !message->key[0]) {
				message->status = (int32_t)-NXU_SYS_E_INVALID_ARGUMENT;
				break;
			}
			(void)nxu_mkdir(message->name);
			status = nxu_recovery_fs_mount(message->key, (uint32_t)message->arguments[0], message->name);
			if (status == 0) log_line("mounted ", message->name);
		} else {
			status = nxu_recovery_fs_unmount(message->name);
			if (status == 0) log_line("unmounted ", message->name);
		}
		message->status = (int32_t)status;
		break;
	}
	default:
		message->status = (int32_t)-NXU_SYS_E_NOT_SUPPORTED;
		break;
	}
}

int
main(void)
{
	log_line("ready", 0);
	return nxu_service_run(NXU_DISKD_SERVICE, handle, 0, 1000000U);
}
