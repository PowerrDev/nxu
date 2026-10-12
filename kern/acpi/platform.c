#include <kern/acpi/platform.h>
#include <boot/handoff.h>
#include <kern/i386/vm_param.h>
#include <kern/console/console.h>
#include <uacpi/uacpi.h>
#include <uacpi/acpi.h>
#include <uacpi/tables.h>
#include <uacpi/utilities.h>
#include <uacpi/kernel_api.h>
#include <libk/string.h>

extern uint64_t nxu_acpi_rsdp;
static bool ready;
static unsigned devices;
static unsigned tables;

static uint8_t
checksum(const void *data, unsigned size)
{
	const uint8_t *p = data;
	uint8_t sum = 0;

	while (size--) {
		sum += *p++;
	}

	return sum;
}

static uint64_t
legacy_rsdp(void)
{
	const uint8_t *low = (const uint8_t *)(uintptr_t)VMM_HIGHER_HALF_BASE;
	uint16_t segment;

	memcpy(&segment, low + 0x40e, 2);

	uint32_t base = (uint32_t)segment << 4;

	for (unsigned region = 0; region < 2; region++) {
		uint32_t start = region ? 0xe0000 : base, size = region ? 0x20000 : 1024;

		if (!start || start + size > 0x100000) {
			continue;
		}

		for (uint32_t offset = 0; offset + 20 <= size; offset += 16) {
			const uint8_t *p = low + start + offset;

			if (!memcmp(p, "RSD PTR ", 8) && !checksum(p, 20)) {
				return start + offset;
			}
		}
	}

	return 0;
}

static void
log_madt(const uint8_t *bytes, uint32_t length)
{
	if (length < 44) {
		return;
	}

	uint32_t cursor = 44;

	while (cursor + 2 <= length) {
		uint8_t type = bytes[cursor], size = bytes[cursor + 1];

		if (size < 2 || size > length - cursor) {
			kputln("NxuACPIPlatform: malformed MADT entry rejected");
			break;
		}

		const uint8_t *entry = bytes + cursor;

		if (type == 0 && size >= 8) {
			uint32_t flags;

			memcpy(&flags, entry + 4, 4);
			kprintf("NxuACPIPlatform: local APIC processor=%u id=%u flags=0x%x\n", entry[2], entry[3], flags);
		} else if (type == 1 && size >= 12) {
			uint32_t address;
			uint32_t gsi;

			memcpy(&address, entry + 4, 4);
			memcpy(&gsi, entry + 8, 4);
			kprintf("NxuACPIPlatform: IOAPIC id=%u address=0x%x GSI-base=%u\n", entry[2], address, gsi);
		} else if (type == 2 && size >= 10) {
			uint32_t gsi;

			memcpy(&gsi, entry + 4, 4);
			kprintf("NxuACPIPlatform: interrupt override bus=%u IRQ=%u GSI=%u\n", entry[2], entry[3], gsi);
		} else {
			kprintf("NxuACPIPlatform: MADT entry type=%u size=%u\n", type, size);
		}

		cursor += size;
	}
}

static uacpi_iteration_decision
log_table(void *ctx, uacpi_table_info *info)
{
	(void)ctx;

	char signature[5];

	memcpy(signature, info->signature, 4);
	signature[4] = 0;
	kprintf("NxuACPIPlatform: table %s address=0x%llx size=%u checksum=%s\n", signature,
			(unsigned long long)info->phys_addr, (unsigned)info->size,
			(info->flags & UACPI_TABLE_CSUM_BAD)	   ? "invalid"
			: (info->flags & UACPI_TABLE_CSUM_CHECKED) ? "valid"
													   : "not applicable");
	if (!memcmp(info->signature, "APIC", 4) && !(info->flags & UACPI_TABLE_CSUM_BAD)) {
		uacpi_table table;

		if (uacpi_table_get_by_index(info->idx, &table) == UACPI_STATUS_OK) {
			log_madt(table.ptr, info->size);
			(void)uacpi_table_unref(&table);
		}
	}

	if (!memcmp(info->signature, "FACP", 4) && !(info->flags & UACPI_TABLE_CSUM_BAD)) {
		uacpi_table table;

		if (uacpi_table_get_by_index(info->idx, &table) == UACPI_STATUS_OK) {
			uint64_t facs = 0;
			uint32_t legacy = 0;

			if (info->size >= 40) {
				memcpy(&legacy, (const uint8_t *)table.ptr + 36, 4);
			}

			if (info->size >= 140) {
				memcpy(&facs, (const uint8_t *)table.ptr + 132, 8);
			}

			if (!facs) {
				facs = legacy;
			}

			if (facs) {
				const uint8_t *header = uacpi_kernel_map(facs, 8);

				if (header != UACPI_MAP_FAILED) {
					uint32_t length;

					memcpy(&length, header + 4, 4);
					if (!memcmp(header, "FACS", 4) && length >= 64 && length <= 1024 * 1024) {
						kprintf("NxuACPIPlatform: table FACS address=0x%llx size=%u checksum=not applicable\n",
								(unsigned long long)facs, length);
						tables++;
					} else {
						kputln("NxuACPIPlatform: invalid FACS rejected");
					}

					uacpi_kernel_unmap((void *)header, 8);
				} else {
					kputln("NxuACPIPlatform: FACS outside supported physical address range");
				}
			}

			(void)uacpi_table_unref(&table);
		}
	}

	tables++;
	return UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_iteration_decision
log_device(void *ctx, uacpi_namespace_node *node, uacpi_u32 depth)
{
	(void)ctx;
	(void)depth;

	const char *path = uacpi_namespace_node_generate_absolute_path(node);
	uacpi_namespace_node_info *info = 0;
	uacpi_status result = uacpi_get_namespace_node_info(node, &info);
	uint32_t sta = 0;
	uacpi_status status = uacpi_eval_sta(node, &sta);

	kprintf("NxuACPIPlatform: device %s HID=%s UID=%s status=%s STA=0x%x\n", path ? path : "<unavailable>",
			info && (info->flags & UACPI_NS_NODE_INFO_HAS_HID) ? info->hid.value : "<none>",
			info && (info->flags & UACPI_NS_NODE_INFO_HAS_UID) ? info->uid.value : "<none>",
			status != UACPI_STATUS_OK ? "unknown"
			: (sta & 1)				  ? "present"
									  : "absent",
			sta);
	if (result != UACPI_STATUS_OK) {
		kprintf("NxuACPIPlatform: device metadata failed: %s\n", uacpi_status_to_string(result));
	}

	if (info) {
		if (info->flags & UACPI_NS_NODE_INFO_HAS_ADR) {
			kprintf("NxuACPIPlatform: device %s ADR=0x%llx\n", path ? path : "<unavailable>",
					(unsigned long long)info->adr);
		}

		if (info->flags & UACPI_NS_NODE_INFO_HAS_CID) {
			for (uint32_t i = 0; i < info->cid.num_ids; i++) {
				kprintf("NxuACPIPlatform: device %s CID=%s\n", path ? path : "<unavailable>", info->cid.ids[i].value);
			}
		}

		uacpi_free_namespace_node_info(info);
	}

	if (path) {
		uacpi_free_absolute_path(path);
	}

	devices++;
	return UACPI_ITERATION_DECISION_CONTINUE;
}

const void *
nxu_acpi_find_table(const char *signature)
{
	if (!ready) {
		return 0;
	}

	uacpi_table table;

	if (uacpi_table_find_by_signature(signature, &table) != UACPI_STATUS_OK) {
		return 0;
	}

	/* Firmware mappings are retained while SMP consumes this table. */
	return table.ptr;
}

void
nxu_acpi_discover(const i386_boot_info_t *boot)
{
	if (boot && boot->multiboot && (boot->multiboot->flags & NXU_BOOT_FIRMWARE_FLAG)) {
		const struct nxu_boot_firmware *fw =
			(const void *)((const uint8_t *)boot->multiboot + NXU_BOOT_FIRMWARE_OFFSET);
		if (fw->magic == NXU_BOOT_FIRMWARE_MAGIC && fw->version == 1 && fw->size == sizeof(*fw)) {
			nxu_acpi_rsdp = fw->rsdp;
		}
	}

	if (!nxu_acpi_rsdp) {
		nxu_acpi_rsdp = legacy_rsdp();
	}

	if (!nxu_acpi_rsdp) {
		kputln("NxuACPIPlatform: firmware has no discoverable RSDP");
		return;
	}

	kprintf("NxuACPIPlatform: RSDP address=0x%llx\n", (unsigned long long)nxu_acpi_rsdp);
	uacpi_status status = uacpi_initialize(UACPI_FLAG_NO_ACPI_MODE | UACPI_FLAG_BAD_CSUM_FATAL |
										   UACPI_FLAG_BAD_TBL_SIGNATURE_FATAL | UACPI_FLAG_PROACTIVE_TBL_CSUM);
	if (status != UACPI_STATUS_OK) {
		kprintf("NxuACPIPlatform: initialization failed: %s\n", uacpi_status_to_string(status));
		return;
	}

	ready = true;

	const uint8_t *rsdp = uacpi_kernel_map(nxu_acpi_rsdp, 36);

	if (rsdp != UACPI_MAP_FAILED) {
		uint64_t root = 0;
		uint32_t rsdt = 0;

		memcpy(&rsdt, rsdp + 16, 4);
		if (rsdp[15] >= 2) {
			memcpy(&root, rsdp + 24, 8);
		}

		if (!root) {
			root = rsdt;
		}

		const struct acpi_sdt_hdr *header = uacpi_kernel_map(root, sizeof(*header));

		if (header != UACPI_MAP_FAILED) {
			char signature[5];

			memcpy(signature, header->signature, 4);
			signature[4] = 0;
			kprintf("NxuACPIPlatform: root table %s address=0x%llx size=%u\n", signature, (unsigned long long)root,
					header->length);
			uacpi_kernel_unmap((void *)header, sizeof(*header));
		}

		uacpi_kernel_unmap((void *)rsdp, 36);
	}

	status = uacpi_namespace_load();
	/* Include tables installed by module-level AML during namespace loading. */
	(void)uacpi_for_each_table(log_table, 0);
	if (status != UACPI_STATUS_OK) {
		kprintf("NxuACPIPlatform: namespace load failed: %s\n", uacpi_status_to_string(status));
		return;
	}

	status = uacpi_namespace_for_each_child(
		uacpi_namespace_root(), log_device, 0,
		UACPI_OBJECT_DEVICE_BIT | UACPI_OBJECT_PROCESSOR_BIT | UACPI_OBJECT_THERMAL_ZONE_BIT, UACPI_MAX_DEPTH_ANY, 0);
	kprintf("NxuACPIPlatform: discovery complete tables=%u devices=%u status=%s\n", tables, devices,
			uacpi_status_to_string(status));
}
