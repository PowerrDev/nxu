# NxuACPIPlatform

The i386 kernel inventories firmware tables and walks the AML namespace during
its kernel boot phase, after the heap is available and before secondary CPUs
start. Every message from the subsystem uses `NxuACPIPlatform:`.

NxuBoot supplies the firmware's RSDP from the UEFI configuration tables. Legacy
Multiboot boots scan the EBDA and BIOS ROM search areas instead. uACPI validates
the RSDP, selects XSDT/RSDT, and installs referenced tables, including DSDT and
SSDTs. Checksums and signatures are enforced. The inventory includes unknown
signatures, repeated SSDTs, the root table, and the FADT's FACS reference. FACS
has no checksum. Table data beyond the direct map is mapped through the kernel's
MMIO window; physical addresses above 4 GiB are unsupported by this non-PAE port.

A pinned MIT-licensed uACPI interpreter lives in `EXTERNAL_HEADERS/uacpi`; its upstream
revision is recorded in `REVISION`. AML devices, processors and thermal zones
are enumerated by absolute namespace path. Device lines include HID, UID and
_STA; additional lines identify ADR and compatible IDs where supplied. Both
present and absent namespace devices are logged. An AML device declaration
is not necessarily a physically attached device, and PCI functions without an
AML declaration remain the PCI bus driver's responsibility.

MADT logs identify local APICs, I/O APICs, interrupt overrides and other entries.
SMP consumes the same validated MADT. Local x2APIC declarations with IDs that
fit the existing xAPIC implementation are supported; larger IDs are reported
as unsupported. Discovery does not enable I/O APIC interrupt routing.

This integration is for boot discovery. uACPI is built in reduced-hardware mode
so namespace loading does not install SCI/GPE handlers or switch ACPI mode.
It evaluates identification/status methods, but does not run namespace _INI
initialization or provide battery, sleep, hotplug or shutdown services. Its
host synchronization/thread identity is scoped to the bootstrap thread before
AP dispatch. Do not call the AML interpreter later from arbitrary threads until
runtime synchronization and event delivery are implemented.

IORegistry remains available internally, but its automatic boot dumps are removed.

After building `i386`, `nxuboot`, and `i386-userland`, run:

```sh
python3 tools/check_firmware_boot.py
```

The test uses private firmware-variable, boot and ext4 images. It covers UEFI
XSDT discovery, BIOS RSDT discovery, ACPI absence, AML methods, absent devices,
buffer data that resembles a device, unknown tables, SMP, recovery graphics and
keyboard input, and refusal of an invalid ELF. Supply `--firmware`, `--vars`,
and `--build-root` when using different build locations. On an Intel Mac,
`--modes uefi-virtio --accel hvf --cpu host,phys-bits=32` tests HVF.
