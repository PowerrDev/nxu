#!/usr/bin/env python3
"""Boot NxuBoot through UEFI, verify ACPI/AML discovery and recovery scanout."""

import argparse
import os
import re
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import time
from make_uefi_image import build_image
from check_gui_service import read_ppm

ROOT = Path(__file__).resolve().parent.parent


def package(payload):
    if len(payload) + 1 < 64:
        return bytes([len(payload) + 1]) + payload
    length = len(payload) + 2
    assert length < 4096
    return bytes([0x40 | (length & 15), length >> 4]) + payload


def device(name, present):
    # Both identification and status are methods, requiring real AML execution.
    hid = b"\x14" + package(b"_HID\x00\xa4\x0dNXU0001\x00")
    sta = b"\x14" + package(b"_STA\x00\xa4" + (b"\x72\x0a\x0e\x01\x00" if present else b"\x00"))
    return b"\x5b\x82" + package(name + hid + sta + b"\x08_UID\x0a\x07")


def sdt(signature, payload):
    header = bytearray(
        struct.pack("<4sIBB6s8sIII", signature, 36 + len(payload), 2, 0, b"NXUTST", b"DISCOVER", 1, 0x4E585554, 1)
    )
    table = header + payload
    table[9] = (-sum(table)) & 255
    return table


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpu", default="qemu64,phys-bits=32")
    parser.add_argument("--accel", default="tcg", choices=["tcg", "hvf"])
    parser.add_argument("--modes", nargs="+", default=["uefi-gop", "uefi-virtio", "bios", "no-acpi", "bad-elf"])
    parser.add_argument("--build-root", type=Path, default=ROOT / "BUILD/BUILD-i386-desktop")
    parser.add_argument("--firmware", type=Path, default=Path("/usr/local/share/qemu/edk2-x86_64-code.fd"))
    parser.add_argument("--vars", type=Path, default=Path("/usr/local/share/qemu/edk2-i386-vars.fd"))
    args = parser.parse_args()
    build = args.build_root.resolve()
    os.environ["PATH"] = "/opt/homebrew/opt/e2fsprogs/sbin:/usr/local/opt/e2fsprogs/sbin:" + os.environ["PATH"]
    scratch = Path(tempfile.mkdtemp(prefix="nxu-firmware-"))
    print("Firmware artifacts: " + str(scratch), flush=True)
    splash = [
        int(pixel, 16) for pixel in re.findall(r"0x([0-9A-Fa-f]{8})U", (ROOT / "kern/boot/splash_asset.c").read_text())
    ]
    kernel = (build / "i386/kernel.elf").read_bytes()
    loader = (build / "nxuboot/BOOTX64.EFI").read_bytes()
    root = scratch / "root"
    (root / "System/Recovery").mkdir(parents=True)
    shutil.copyfile(build / "i386/user/triageOS", root / "System/Recovery/triageOS")
    disk = scratch / "disk.img"
    with disk.open("wb") as f:
        f.truncate(64 * 1024 * 1024)
    subprocess.run(
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
        ],
        check=True,
    )
    fake = device(b"GOST", True)
    buffer = b"\x08BLOB\x11" + package(b"\x0a" + bytes([len(fake)]) + fake)
    aml = b"\x10" + package(b"\\_SB_" + device(b"NXU0", True) + device(b"OFF0", False) + buffer)
    ssdt = scratch / "ssdt.aml"
    ssdt.write_bytes(sdt(b"SSDT", aml))
    unknown = scratch / "unknown.aml"
    unknown.write_bytes(sdt(b"NXUT", b"firmware inventory test"))
    for mode in args.modes:
        serial = scratch / (mode + ".log")
        variables = scratch / (mode + "-vars.fd")
        boot = scratch / (mode + ".img")
        boot.write_bytes(
            build_image(
                loader,
                b"invalid elf" if mode == "bad-elf" else kernel,
                "recovery=1 virtio-irq=1 log.trace=0 log.stats_ms=0",
            )
        )
        command = [
            "qemu-system-x86_64",
            "-accel",
            args.accel,
            "-machine",
            "q35" if mode == "uefi-gop" else ("pc,acpi=off" if mode == "no-acpi" else "pc"),
            "-m",
            "512M",
            "-smp",
            "2",
            "-display",
            "none",
            "-no-reboot",
            "-monitor",
            "stdio",
            "-serial",
            "file:" + str(serial),
        ]
        if mode.startswith("uefi") or mode == "bad-elf":
            shutil.copyfile(args.vars, variables)
            command += [
                "-cpu",
                args.cpu,
                "-global",
                ("q35-pcihost" if mode == "uefi-gop" else "i440FX-pcihost") + ".pci-hole64-size=0",
                "-fw_cfg",
                "name=opt/ovmf/X-PciMmio64Mb,string=0",
                "-drive",
                f"if=pflash,format=raw,readonly=on,file={args.firmware}",
                "-drive",
                f"if=pflash,format=raw,file={variables}",
                "-drive",
                f"if=none,format=raw,file={boot},id=boot",
                "-device",
                "qemu-xhci",
                "-device",
                "usb-storage,drive=boot",
            ]
        else:
            command += [
                "-kernel",
                str(build / "i386/kernel.elf"),
                "-append",
                "recovery=1 virtio-irq=1 log.trace=0 log.stats_ms=0",
            ]
        if mode != "bad-elf":
            command += [
                "-drive",
                f"if=none,format=raw,file={disk},id=d0",
                "-device",
                "virtio-blk-pci,drive=d0,disable-modern=on",
                "-device",
                "virtio-keyboard-pci,disable-modern=on",
                "-device",
                "virtio-mouse-pci,disable-modern=on",
            ]
        if mode not in ["uefi-gop", "bad-elf"]:
            command += ["-vga", "none", "-device", "virtio-gpu-pci,xres=640,yres=480"]
        if mode not in ["no-acpi", "bad-elf"]:
            command += ["-acpitable", "file=" + str(ssdt), "-acpitable", "file=" + str(unknown)]
        with (scratch / (mode + "-qemu.log")).open("w") as log:
            process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT)
            try:
                marker = (
                    "NxuBoot: invalid kernel or load address unavailable"
                    if mode == "bad-elf"
                    else "triageOS: StartupOptionsUI is live"
                )
                deadline = time.monotonic() + 90
                splash_checked = False
                while time.monotonic() < deadline:
                    text = serial.read_text(errors="replace") if serial.exists() else ""
                    if mode != "bad-elf" and not splash_checked and "boot_splash_show: shown" in text:
                        shot = scratch / (mode + "-splash.ppm")
                        process.stdin.write(("screendump " + str(shot) + "\n").encode())
                        process.stdin.flush()
                        for _ in range(50):
                            try:
                                sw, sh, sp = read_ppm(shot)
                            except (FileNotFoundError, ValueError, AssertionError):
                                time.sleep(0.01)
                                continue
                            else:
                                left, top = (sw - 128) // 2, (sh - 128) // 2
                                for index, pixel in enumerate(splash):
                                    if pixel >> 24 != 255:
                                        continue
                                    expected = bytes(((pixel >> 16) & 255, (pixel >> 8) & 255, pixel & 255))
                                    offset = ((top + index // 128) * sw + left + index % 128) * 3
                                    assert sp[offset : offset + 3] == expected, "Boot logo missing from scanout"
                                splash_checked = True
                                break
                        assert splash_checked, "Splash screenshot unavailable"
                    if marker in text:
                        break
                    if process.poll() is not None:
                        raise AssertionError("QEMU exited: " + text[-3000:])
                    time.sleep(0.1)
                else:
                    raise AssertionError(mode + " timeout: " + text[-3000:])
                if mode == "bad-elf":
                    assert "i386_init:" not in text
                else:
                    assert splash_checked, "Boot splash was not shown"
                    assert "boot_splash_finish: progress complete" in text
                    assert "IORegistry:" not in text
                    if mode == "no-acpi":
                        assert "NxuACPIPlatform: firmware has no discoverable RSDP" in text
                    else:
                        for expected in [
                            "table SSDT",
                            "table NXUT",
                            "device \\_SB_.NXU0 HID=NXU0001 UID=7 status=present STA=0xf",
                            "device \\_SB_.OFF0 HID=NXU0001 UID=7 status=absent STA=0x0",
                            "discovery complete",
                            "2 CPU(s)",
                        ]:
                            assert expected in text, (mode, expected, text[-6000:])
                        assert "device \\_SB_.GOST" not in text, "Buffer bytes misidentified as an AML device"
                    if mode.startswith("uefi"):
                        assert "NxuBoot: entering i386 kernel" in text and "root table XSDT" in text
                    if mode == "uefi-gop":
                        assert "DriverKitDisplayFamily: firmware framebuffer" in text
                    screenshot = scratch / (mode + ".ppm")
                    process.stdin.write(("screendump " + str(screenshot) + "\n").encode())
                    process.stdin.flush()
                    for _ in range(50):
                        try:
                            width, height, pixels = read_ppm(screenshot)
                            assert len(set(pixels)) > 20
                            break
                        except (FileNotFoundError, ValueError, AssertionError):
                            time.sleep(0.1)
                    else:
                        raise AssertionError("Recovery scanout missing")
                    before = pixels
                    for key in ["down", "down", "ret"]:
                        process.stdin.write(("sendkey " + key + "\n").encode())
                        process.stdin.flush()
                        time.sleep(0.25)
                    terminal = scratch / (mode + "-terminal.ppm")
                    process.stdin.write(("screendump " + str(terminal) + "\n").encode())
                    process.stdin.flush()
                    for _ in range(50):
                        try:
                            _, _, after = read_ppm(terminal)
                            assert (
                                sum(a != b for a, b in zip(before, after)) > width * height // 4
                            ), "Keyboard did not open recovery terminal"
                            break
                        except (FileNotFoundError, ValueError, AssertionError):
                            time.sleep(0.1)
                    else:
                        raise AssertionError("Recovery keyboard input unavailable")
                print("PASS " + mode, flush=True)
            finally:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    subprocess.run(["e2fsck", "-fn", str(disk)], check=True)


if __name__ == "__main__":
    main()
