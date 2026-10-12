#!/usr/bin/env python3
"""Verify the recovery/SAP cursor handoff through actual QEMU scanout."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
from check_gui_service import read_ppm

ROOT = Path(__file__).resolve().parent.parent
from generate_pointer_cursor import load_arrow

CURSOR_WIDTH, CURSOR_HEIGHT, HOTSPOT_X, HOTSPOT_Y, CURSOR_PIXELS = load_arrow()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=["i386", "arm64"], required=True)
    args = parser.parse_args()
    os.environ["PATH"] = "/opt/homebrew/opt/e2fsprogs/sbin:" + os.environ["PATH"]
    scratch = Path(tempfile.mkdtemp(prefix="nxu-cursor-" + args.arch + "-"))
    print("Cursor artifacts: " + str(scratch), flush=True)
    i386 = args.arch == "i386"
    build = ROOT / ("BUILD/BUILD-i386-desktop/i386/user" if i386 else "BUILD/userland")
    root = scratch / "root"
    (root / "System/Recovery").mkdir(parents=True)
    (root / "System/Library/Frameworks/Cursor.framework").mkdir(parents=True)
    shutil.copyfile(build / "triageOS", root / "System/Recovery/triageOS")
    disk = scratch / "disk.img"
    with disk.open("wb") as file:
        file.truncate(64 * 1024 * 1024)
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
    kind = "pci" if i386 else "device"
    legacy = ",disable-modern=on" if i386 else ""
    serial = scratch / "serial.log"
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
    command += [
        "-smp",
        "2",
        "-m",
        "512M",
        "-display",
        "none",
        "-no-reboot",
        "-drive",
        f"if=none,format=raw,file={disk},id=d0",
        "-device",
        f"virtio-blk-{kind},drive=d0{legacy}",
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
        "recovery=1 log.trace=0 log.stats_ms=0 "
        + ("test=desktop ui.userland=1 virtio-irq=1" if i386 else "-no-sound -no-windowserver -no-crash-ui -no-patchd"),
    ]
    with (scratch / "qemu.log").open("w") as log:
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT)

        def monitor(value):
            process.stdin.write((value + "\n").encode())
            process.stdin.flush()
            time.sleep(0.2)

        def wait(marker):
            deadline = time.monotonic() + 90
            while time.monotonic() < deadline:
                text = serial.read_text(errors="replace") if serial.exists() else ""
                if marker in text:
                    return
                if process.poll() is not None:
                    raise AssertionError(text[-3000:])
                time.sleep(0.1)
            raise AssertionError("Timeout: " + marker + ": " + text[-3000:])

        def capture(name):
            path = scratch / (name + ".ppm")
            monitor("screendump " + str(path))
            for _ in range(30):
                try:
                    w, h, pixels = read_ppm(path)
                    assert (w, h) == (640, 480)
                    return pixels
                except (FileNotFoundError, ValueError):
                    time.sleep(0.1)
            raise AssertionError("Missing scanout")

        def pixel(pixels, x, y):
            offset = (y * 640 + x) * 3
            return pixels[offset : offset + 3]

        def arrow(pixels, x, y, background=None):
            x -= HOTSPOT_X
            y -= HOTSPOT_Y
            checked = 0
            for row in range(CURSOR_HEIGHT):
                for col in range(CURSOR_WIDTH):
                    source = CURSOR_PIXELS[row * CURSOR_WIDTH + col]
                    alpha = source >> 24
                    if alpha == 255 or background is not None:
                        channels = [(source >> 16) & 255, (source >> 8) & 255, source & 255]
                        if background is None:
                            expected = bytes(channels)
                        else:
                            behind = pixel(background, x + col, y + row)
                            expected = bytes(
                                (channel * alpha + value * (255 - alpha) + 127) // 255
                                for channel, value in zip(channels, behind)
                            )
                        assert pixel(pixels, x + col, y + row) == expected, (x, y, row, col)
                        checked += 1
            assert checked > 50, "Missing mainOS arrow pixels"

        try:
            wait("triageOS: StartupOptionsUI is live")
            for key in ["down", "down", "ret"]:
                monitor("sendkey " + key)
            monitor("mouse_move 0 0")
            background = capture("background")
            for _ in range(3):
                monitor("mouse_move -90 -70")
            arrow(capture("recovery"), 50, 30, background)
            for character in "rm -r --force-system /disk/System/Library/Frameworks/Cursor.framework":
                if character == " ":
                    key = "spc"
                elif character == "/":
                    key = "slash"
                elif character == ".":
                    key = "dot"
                elif character == "-":
                    key = "minus"
                else:
                    key = "shift-" + character.lower() if character.isupper() else character
                monitor("sendkey " + key)
            monitor("sendkey ret")
            wait("sap: confirmation requested")
            modal = capture("modal")
            arrow(modal, 320, 240)
            for row in range(CURSOR_HEIGHT):
                for col in range(CURSOR_WIDTH):
                    expected = bytes(
                        (value & 0xFE) >> 1 for value in pixel(background, 50 - HOTSPOT_X + col, 30 - HOTSPOT_Y + row)
                    )
                    assert (
                        pixel(modal, 50 - HOTSPOT_X + col, 30 - HOTSPOT_Y + row) == expected
                    ), "Frozen recovery cursor in SAP backdrop"
            monitor("sendkey esc")
            time.sleep(0.5)
            arrow(capture("restored"), 50, 30, background)
            print(
                "PASS " + args.arch + ": shared arrow, clean SAP backdrop, cursor restored after cancellation",
                flush=True,
            )
        finally:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


if __name__ == "__main__":
    main()
