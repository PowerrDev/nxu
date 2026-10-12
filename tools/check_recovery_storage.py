#!/usr/bin/env python3
"""Boot real recovery UI and run its shell/storage code on disposable disks."""

import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import time
import uuid
import zlib

ROOT = Path(__file__).resolve().parent.parent


def run(command):
    subprocess.run(command, cwd=ROOT, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=["i386", "arm64"], required=True)
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    os.environ["PATH"] = "/opt/homebrew/opt/e2fsprogs/sbin:/usr/local/opt/e2fsprogs/sbin:" + os.environ["PATH"]
    scratch = Path(tempfile.mkdtemp(prefix="nxu-recovery-" + args.arch + "-"))
    print("Recovery artifacts: " + str(scratch), flush=True)
    i386 = args.arch == "i386"
    build = ROOT / ("BUILD/BUILD-i386-desktop/i386/user" if i386 else "BUILD/userland")
    if i386:
        run(
            [
                "make",
                "-j8",
                "i386",
                "i386-userland",
                "BUILD_ROOT=BUILD/BUILD-i386-desktop",
                "UISERVICE=0",
                "WINDOWSERVER=0",
            ]
        )
    else:
        run(["make", "-j8", "USER_CC=clang", "BUILD/default/kernel.bin", "BUILD/userland/triageOS"])
    obj = scratch / "storage.o"
    run(
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
            "-Iframeworks/Recovery.framework/include",
            "-c",
            "tools/fixtures/recovery/storage.c",
            "-o",
            str(obj),
        ]
    )
    common = (
        [
            "frameworks/crt0_i386.o",
            "frameworks/lib/syscall_i386.o",
            "frameworks/lib/thread_i386.o",
            "frameworks/lib/spawn_io_i386.o",
        ]
        if i386
        else ["frameworks/crt0.o", "frameworks/lib/syscall.o", "frameworks/lib/thread.o"]
    )
    common += [
        "frameworks/lib/string.o",
        "frameworks/Recovery.framework/lib/RecoveryServices.o",
        "frameworks/Recovery.framework/triageOS/shell.o",
        "frameworks/SoftwareUpdate.framework/lib/nsw.o",
        "frameworks/SoftwareUpdate.framework/lib/manifest.o",
        "frameworks/CoreFoundation.framework/lib/plist/plist.o",
        "libk/sha256.o",
    ]
    probe = scratch / "storage-probe"
    run(
        [
            "ld.lld",
            *(["-m", "elf_i386"] if i386 else []),
            "-T",
            "makedefs/user-i386.ld" if i386 else "makedefs/user.ld",
            "-nostdlib",
            "-static",
            *[str(build / path) for path in common],
            str(obj),
            *([str(build / "libnxurt.a")] if i386 else []),
            "-o",
            str(probe),
        ]
    )
    # A single unused MBR partition, never mounted and backed by a private file.
    extra = scratch / "partition.img"
    with extra.open("wb") as file:
        file.truncate(16 * 1024 * 1024)
        sector = bytearray(512)
        sector[446 + 4] = 0x83
        struct.pack_into("<II", sector, 446 + 8, 2048, 4096)
        sector[510:512] = b"\x55\xaa"
        file.seek(0)
        file.write(sector)
    readonly = scratch / "readonly.img"
    shutil.copyfile(extra, readonly)
    gpt = scratch / "gpt.img"
    sectors = 32768
    entries = bytearray(128 * 128)
    for index, start in enumerate((2048, 8192)):
        offset = index * 128
        entries[offset : offset + 16] = uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4").bytes_le
        entries[offset + 16 : offset + 32] = uuid.UUID(int=index + 1).bytes_le
        struct.pack_into("<QQ", entries, offset + 32, start, start + 4095)

    def header(current, backup, table):
        data = bytearray(512)
        struct.pack_into(
            "<8sIIIIQQQQ16sQIII",
            data,
            0,
            b"EFI PART",
            0x10000,
            92,
            0,
            0,
            current,
            backup,
            34,
            sectors - 34,
            uuid.UUID(int=3).bytes_le,
            table,
            128,
            128,
            zlib.crc32(entries),
        )
        struct.pack_into("<I", data, 16, zlib.crc32(data[:92]))
        return data

    with gpt.open("wb") as file:
        file.truncate(sectors * 512)
        protective = bytearray(512)
        protective[450] = 0xEE
        struct.pack_into("<II", protective, 454, 1, sectors - 1)
        protective[510:] = b"\x55\xaa"
        file.write(protective)
        for lba, data in (
            (1, header(1, sectors - 1, 2)),
            (2, entries),
            (sectors - 33, entries),
            (sectors - 1, header(sectors - 1, 1, sectors - 33)),
        ):
            file.seek(lba * 512)
            file.write(data)
    for mode in ("storage", "ui", *(["chord"] if i386 else [])):
        root = scratch / (mode + "-root")
        (root / "System/Recovery").mkdir(parents=True)
        shutil.copyfile(probe if mode == "storage" else build / "triageOS", root / "System/Recovery/triageOS")
        disk = scratch / (mode + ".img")
        with disk.open("wb") as file:
            file.truncate(64 * 1024 * 1024)
        run(
            [
                "mkfs.ext4",
                "-F",
                "-q",
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
            ]
        )
        serial = scratch / (mode + ".log")
        command = (
            [
                "qemu-system-i386",
                "-M",
                "pc",
                "-kernel",
                str(ROOT / "BUILD/BUILD-i386-desktop/i386/kernel.elf"),
                "-vga",
                "none",
            ]
            if i386
            else [
                "qemu-system-aarch64",
                "-M",
                "virt,gic-version=3",
                "-cpu",
                "cortex-a72",
                "-kernel",
                str(ROOT / "BUILD/default/kernel.bin"),
                "-global",
                "virtio-mmio.force-legacy=false",
            ]
        )
        kind = "pci" if i386 else "device"
        legacy = ",disable-modern=on" if i386 else ""
        # ARM virtio-mmio slots are discovered in reverse command-line order.
        devices = [(disk, "d0"), (extra, "d1"), (gpt, "d2"), (readonly, "d3")]
        for image, name in (devices if i386 else devices[::-1]):
            read_only = ",readonly=on" if name == "d3" else ""
            command += [
                "-drive",
                f"if=none,format=raw,file={image},id={name}{read_only}",
                "-device",
                f"virtio-blk-{kind},drive={name}{legacy}",
            ]
        command += [
            "-smp",
            "2",
            "-m",
            "512M",
            "-display",
            "none",
            "-no-reboot",
            "-device",
            f"virtio-gpu-{kind},xres=640,yres=480",
            "-device",
            f"virtio-keyboard-{kind}{legacy}",
            "-device",
            f"virtio-mouse-{kind}{legacy}",
            "-serial",
            f"file:{serial}",
            "-monitor",
            "stdio",
            "-append",
            (
                "test=desktop ui.userland=1 virtio-irq=1 "
                if i386
                else "-no-sound -no-windowserver -no-crash-ui -no-patchd "
            )
            + ("recovery=1 " if mode != "chord" else "")
            + "log.trace=0 log.stats_ms=0",
        ]
        with (scratch / (mode + "-qemu.log")).open("w") as log:
            process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + args.timeout
                sent_chord = False
                marker = "RECOVERY STORAGE PASS" if mode == "storage" else "triageOS: StartupOptionsUI is live"
                while time.monotonic() < deadline:
                    text = serial.read_text(errors="replace") if serial.exists() else ""
                    if "RECOVERY STORAGE FAIL" in text or "KERNEL PANIC" in text:
                        raise RuntimeError(text[-4000:])
                    if mode == "chord" and not sent_chord and "hold Shift+R" in text:
                        process.stdin.write(b"sendkey shift-r 1000\n")
                        process.stdin.flush()
                        sent_chord = True
                    if marker in text:
                        break
                    if process.poll() is not None:
                        raise RuntimeError("QEMU exited: " + text[-4000:])
                    time.sleep(0.1)
                else:
                    raise RuntimeError("timeout: " + text[-4000:])
                if mode != "storage":
                    screenshot = scratch / (mode + ".ppm")
                    process.stdin.write(("screendump " + str(screenshot) + "\n").encode())
                    process.stdin.flush()
                    for _ in range(50):
                        if screenshot.exists():
                            break
                        time.sleep(0.1)
                    from check_gui_service import read_ppm

                    w, h, pixels = read_ppm(screenshot)
                    assert (w, h) == (640, 480) and len(set(pixels)) > 20, "recovery scanout missing"
                    if mode == "chord":
                        assert "startup recovery options requested" in text
                print(args.arch + " recovery " + mode + " PASS", flush=True)
            finally:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        if mode == "storage":
            run(["e2fsck", "-fn", str(disk)])
            assert extra.read_bytes()[446:462] == bytes(16), "partition entry still on disk"
            assert readonly.read_bytes()[446:462] != bytes(16), "read-only partition changed"
            raw = gpt.read_bytes()
            for lba, table in ((1, 2), (sectors - 1, sectors - 33)):
                data = bytearray(raw[lba * 512 : (lba + 1) * 512])
                checksum = struct.unpack_from("<I", data, 16)[0]
                struct.pack_into("<I", data, 16, 0)
                assert zlib.crc32(data[:92]) == checksum, "GPT header checksum"
                table_bytes = raw[table * 512 : table * 512 + 128 * 128]
                assert table_bytes == bytes(128 * 128), "GPT entries still present"
                assert zlib.crc32(table_bytes) == struct.unpack_from("<I", data, 88)[0], "GPT table checksum"


if __name__ == "__main__":
    main()
