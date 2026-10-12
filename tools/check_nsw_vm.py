#!/usr/bin/env python3
"""Verify NSW extraction through NXU's real VFS in ARM64 or i386 QEMU."""

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=["arm64", "i386"], default="arm64")
    options = parser.parse_args()
    i386 = options.arch == "i386"
    scratch = Path(tempfile.mkdtemp(prefix="nxu-nsw-vm-", dir="/private/tmp"))
    print(f"NSW VM artifacts: {scratch}", flush=True)
    user = ROOT / ("BUILD/i386/user" if i386 else "BUILD/userland")
    subprocess.run(
        [
            "make",
            "-j8",
            "i386" if i386 else "all",
            str((user / "bootd").relative_to(ROOT)),
            str((user / "patchd").relative_to(ROOT)),
        ],
        cwd=ROOT,
        check=True,
    )
    root = scratch / "root"
    recovery_root = scratch / "Recovery"
    (recovery_root / "mainOS").mkdir(parents=True)
    services = root / "System/Library/CoreServices"
    services.mkdir(parents=True)
    shutil.copyfile(ROOT / "tools/DiskRoot/hello.txt", root / "hello.txt")
    shutil.copyfile(
        ROOT / "tools/DiskRoot/System/README.txt", root / "System/README.txt"
    )
    jobs = root / "System/Library/BootDaemons"
    jobs.mkdir()
    for name in ("bootd", "bootd.recovery"):
        shutil.copyfile(user / "bootd", services / name)
    (jobs / "com.nxu.nsw-test.plist").write_text(
        "<plist><dict><key>Label</key><string>com.nxu.nsw-test</string>"
        "<key>Program</key><string>/disk/System/Library/CoreServices/nsw-test</string>"
        "<key>RunAtLoad</key><true/><key>KeepAlive</key><false/>"
        "<key>AllowFilesystemWrite</key><true/></dict></plist>\n"
    )
    obj = scratch / "probe.o"
    subprocess.run(
        [
            "clang",
            "--target=" + ("i386" if i386 else "aarch64") + "-none-elf",
            "-ffreestanding",
            "-fno-stack-protector",
            "-fno-pic",
            "-mgeneral-regs-only",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I.",
            "-Iframeworks/include",
            "-c",
            "tools/fixtures/update/nsw_vm.c",
            "-o",
            str(obj),
        ],
        cwd=ROOT,
        check=True,
    )
    objects = [
        "frameworks/crt0_i386.o" if i386 else "frameworks/crt0.o",
        "frameworks/lib/syscall_i386.o" if i386 else "frameworks/lib/syscall.o",
        "frameworks/lib/string.o",
        "frameworks/SoftwareUpdate.framework/lib/nsw.o",
        "frameworks/SoftwareUpdate.framework/lib/manifest.o",
        "frameworks/CoreFoundation.framework/lib/plist/plist.o",
        "libk/sha256.o",
    ]
    if i386:
        objects += ["frameworks/lib/spawn_io_i386.o", "libnxurt.a"]
    subprocess.run(
        [
            "ld.lld",
            *(["-m", "elf_i386"] if i386 else []),
            "-T",
            "makedefs/user-i386.ld" if i386 else "makedefs/user.ld",
            "-nostdlib",
            "-static",
            str(obj),
            *[str(user / name) for name in objects],
            "-o",
            str(services / "nsw-test"),
        ],
        cwd=ROOT,
        check=True,
    )
    image = scratch / "payload.img"
    with image.open("wb") as stream:
        stream.truncate(4 * 1024 * 1024)
    subprocess.run(
        ["mkfs.ext4", "-q", "-F", "-O", "^has_journal", str(image)], check=True
    )
    subprocess.run(
        [
            "python3",
            "tools/make_nsw.py",
            "--build",
            "vm-test",
            "--arch",
            options.arch,
            "--system-image",
            str(image),
            "--recovery-ramdisk",
            str(image),
            "--output",
            str(recovery_root / "mainOS/build-vm-test.nsw"),
        ],
        cwd=ROOT,
        check=True,
    )
    disk = scratch / "disk.img"
    with disk.open("wb") as stream:
        stream.truncate(64 * 1024 * 1024)
    subprocess.run(
        [
            "mkfs.ext4",
            "-q",
            "-F",
            "-b",
            "4096",
            "-I",
            "256",
            "-O",
            "has_journal,metadata_csum,^metadata_csum_seed,^dir_index,^orphan_file,^fast_commit",
            "-E",
            "lazy_itable_init=0,lazy_journal_init=0",
            "-d",
            str(root),
            str(disk),
        ],
        check=True,
    )
    recovery_disk = scratch / "recovery.img"
    with recovery_disk.open("wb") as stream:
        stream.truncate(64 * 1024 * 1024)
    subprocess.run(
        [
            "mkfs.ext4",
            "-q",
            "-F",
            "-b",
            "4096",
            "-I",
            "256",
            "-O",
            "has_journal,metadata_csum,^metadata_csum_seed,^dir_index,^orphan_file,^fast_commit",
            "-E",
            "lazy_itable_init=0,lazy_journal_init=0",
            "-d",
            str(recovery_root),
            str(recovery_disk),
        ],
        check=True,
    )
    serial = scratch / "serial.log"
    command = (
        [
            "qemu-system-i386",
            "-M",
            "pc",
            "-cpu",
            "qemu32,+nx",
            "-kernel",
            str(ROOT / "BUILD/i386/kernel.elf"),
        ]
        if i386
        else [
            "qemu-system-aarch64",
            "-M",
            "virt,gic-version=3",
            "-global",
            "virtio-mmio.force-legacy=false",
            "-cpu",
            "cortex-a72",
            "-kernel",
            str(ROOT / "BUILD/default/kernel.bin"),
        ]
    )
    command += [
        "-smp",
        "2",
        "-m",
        "512M",
        "-display",
        "none",
        "-monitor",
        "unix:" + str(scratch / "monitor.sock") + ",server=on,wait=off",
        "-serial",
        "file:" + str(serial),
        "-no-reboot",
        "-drive",
        f"if=none,format=raw,file={disk},id=d0",
        "-device",
        (
            "virtio-blk-pci,drive=d0,disable-modern=on"
            if i386
            else "virtio-blk-device,drive=d0"
        ),
        "-append",
        "recovery.volume=1 -no-gpu -no-input -no-sound -no-windowserver -no-desktop -no-crash-ui -no-patchd log.trace=0 log.stats_ms=0",
    ]
    command += [
        "-drive",
        f"if=none,format=raw,file={recovery_disk},id=d1",
        "-device",
        (
            "virtio-blk-pci,drive=d1,disable-modern=on"
            if i386
            else "virtio-blk-device,drive=d1"
        ),
    ]
    if not i386:
        # QEMU allocates MMIO slots downward; NXU discovers them upward.
        # Add the system device last so it is registered as disk0.
        device = command.index("virtio-blk-device,drive=d0")
        del command[device - 1 : device + 1]
        command += ["-device", "virtio-blk-device,drive=d0"]
    with (scratch / "qemu.log").open("w") as log:
        process = subprocess.Popen(command, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 180
            while time.monotonic() < deadline:
                text = serial.read_text(errors="replace") if serial.exists() else ""
                if "nsw-vm: FAIL" in text:
                    raise AssertionError(text[-8000:])
                if "nsw-vm: PASS" in text:
                    break
                if process.poll() is not None:
                    raise RuntimeError((scratch / "qemu.log").read_text())
                time.sleep(1)
            else:
                raise AssertionError("NSW preparation timed out: " + str(serial))
        finally:
            process.terminate()
            process.wait(timeout=10)
    result = subprocess.run(
        ["debugfs", "-R", "cat /Prepared/vm-test/Ready", str(recovery_disk)],
        capture_output=True,
        text=True,
        check=True,
    )
    assert result.stdout == "verified\n", result.stdout
    print(f"PASS: {options.arch} NSW extraction and synced ready marker")


if __name__ == "__main__":
    main()
