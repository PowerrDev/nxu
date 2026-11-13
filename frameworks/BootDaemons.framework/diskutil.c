/*
 * diskutil -- disks and volumes, through diskd and storaged:
 *
 *   diskutil list                      disks, their partitions, mounted volumes
 *   diskutil info MOUNT_POINT          a volume's size, free space and usage
 *   diskutil mount DEVICE FS /Volumes/NAME   (root)
 *   diskutil unmount /Volumes/NAME           (root)
 */
#include <nxu/services.h>
#include <nxu/string.h>
#include <nxu/syscall.h>

static void
say(const char *text)
{
	(void)nxu_write(1, text, nxu_strlen(text));
}

static void
number(uint64_t value)
{
	char reversed[24], out[24];
	uint32_t length = 0, at = 0;

	do {
		reversed[length++] = (char)('0' + value % 10U);
		value /= 10U;
	} while (value);
	while (length) out[at++] = reversed[--length];
	out[at] = 0;
	say(out);
}

/* Bytes as "12.3 MB" (decimal units, as Finder counts). */
static void
size(uint64_t bytes)
{
	static const char *const units[] = {"bytes", "KB", "MB", "GB", "TB"};
	uint32_t unit = 0;
	uint64_t whole = bytes, tenths = 0;

	while (whole >= 1000U && unit < 4U) {
		tenths = (whole % 1000U) / 100U;
		whole /= 1000U;
		unit++;
	}
	number(whole);
	if (unit) {
		say(".");
		number(tenths);
	}
	say(" ");
	say(units[unit]);
}

static int
fail(const char *what, int64_t status)
{
	say("diskutil: ");
	say(what);
	say(status == -NXU_SYS_E_DENIED		 ? ": permission denied (run it with sudo)\n"
		: status == -NXU_SYS_E_NOT_FOUND ? ": not found\n"
										 : ": failed\n");
	return 1;
}

static int
list(void)
{
	nxu_disk_info_t disk;

	for (uint32_t index = 0; nxu_diskd_disk(index, &disk) == 0; index++) {
		say("/dev/");
		say(disk.name);
		say(disk.scheme == NXU_RECOVERY_BLOCK_LAYOUT_GPT	  ? " (GUID partition map, "
			: disk.scheme == NXU_RECOVERY_BLOCK_LAYOUT_MBR ? " (MBR, "
														   : " (");
		size(disk.sector_count * disk.sector_size);
		say(disk.read_only ? ", read-only)\n" : ")\n");
		for (uint32_t part = 0; part < disk.partition_count; part++) {
			nxu_partition_info_t partition;

			if (nxu_diskd_partition(index, part, &partition) != 0) continue;
			say("   ");
			number(partition.index);
			say(": ");
			say(partition.name[0] ? partition.name : "(unnamed)");
			say("  ");
			size(partition.sector_count * disk.sector_size);
			say("\n");
		}
	}

	nxu_volume_info_t volume;

	say("\nMounted volumes:\n");
	for (uint32_t index = 0; nxu_storaged_volume(index, &volume) == 0; index++) {
		say("   ");
		say(volume.path);
		say("  ");
		say(volume.filesystem);
		if (volume.device[0]) {
			say(" on ");
			say(volume.device);
		}
		if (volume.total_bytes) {
			say("  ");
			size(volume.free_bytes);
			say(" free of ");
			size(volume.total_bytes);
		}
		say(volume.read_only ? "  (read-only)\n" : "\n");
	}
	return 0;
}

static int
info(const char *mount)
{
	nxu_storage_usage_t usage;
	int64_t status = nxu_storaged_usage(mount, &usage);

	if (status < 0) return fail(mount, status);
	say(mount);
	say(":\n   Capacity:   ");
	size(usage.total_bytes);
	say("\n   Available:  ");
	size(usage.free_bytes);
	say("\n");
	for (uint32_t index = 0; index < usage.category_count && index < NXU_STORAGE_CATEGORY_MAX; index++) {
		say("   ");
		say(usage.categories[index].name);
		say(": ");
		size(usage.categories[index].bytes);
		say("\n");
	}
	say("   (");
	number(usage.files_counted);
	say(" files)\n");
	return 0;
}

int
main(uint64_t argc, const char *const *argv)
{
	if (argc == 2 && nxu_streq(argv[1], "list")) return list();
	if (argc == 3 && nxu_streq(argv[1], "info")) return info(argv[2]);
	if (argc == 3 && nxu_streq(argv[1], "unmount")) {
		int64_t status = nxu_diskd_unmount(argv[2]);
		return status < 0 ? fail(argv[2], status) : 0;
	}
	if (argc == 5 && nxu_streq(argv[1], "mount")) {
		uint32_t device = 0;

		for (const char *digit = argv[2]; *digit; digit++) {
			if (*digit < '0' || *digit > '9') {
				say("diskutil: DEVICE is a disk number (see diskutil list)\n");
				return 2;
			}
			device = device * 10U + (uint32_t)(*digit - '0');
		}
		int64_t status = nxu_diskd_mount(device, argv[3], argv[4]);
		return status < 0 ? fail(argv[4], status) : 0;
	}
	say("usage: diskutil list | info MOUNT_POINT | mount DEVICE FS /Volumes/NAME | unmount /Volumes/NAME\n");
	return 2;
}
