#!/usr/bin/env python3
"""Run real network commands and executable tests through nzh in ARM64 QEMU."""

import argparse
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent


def screenshot_contains(path, word):
    data = path.read_bytes()
    header, pixels = data.split(b"\n255\n", 1)
    lines = [line for line in header.splitlines() if not line.startswith(b"#")]
    assert lines[0] == b"P6"
    width, height = map(int, lines[1].split())
    font = bytes(
        int(value, 16)
        for value in re.findall(
            r"0x([0-9a-fA-F]{2})", (ROOT / "kern/console/font8x16.c").read_text()
        )
    )
    assert len(font) == 4096
    rows = [
        bytes(
            pixels[(y * width + x) * 3 : (y * width + x) * 3 + 3] == b"\xdc\xe4\xee"
            for x in range(width)
        )
        for y in range(height)
    ]
    glyphs = [
        bytes(
            bool(font[ord(character) * 16 + row] & (1 << bit))
            for character in word
            for bit in range(8)
        )
        for row in range(16)
    ]
    anchor = max(range(16), key=lambda row: sum(glyphs[row]))
    for y in range(height - 15):
        x = rows[y + anchor].find(glyphs[anchor])
        while x >= 0:
            if all(
                rows[y + row][x : x + len(glyphs[row])] == glyphs[row]
                for row in range(16)
            ):
                return True
            x = rows[y + anchor].find(glyphs[anchor], x + 1)
    return False


def test_terminal(process, serial, scratch):
    def monitor(command):
        process.stdin.write((command + "\n").encode())
        process.stdin.flush()

    deadline = time.monotonic() + 180
    while time.monotonic() < deadline:
        text = serial.read_text(errors="replace")
        if "terminal-ui: launched" in text and "desktop: dynamic host ready" in text:
            break
        time.sleep(0.1)
    else:
        raise AssertionError("Terminal launch timeout")
    time.sleep(3)
    for character in "echo gui-terminal-ok":
        monitor(
            "sendkey "
            + (
                "spc"
                if character == " "
                else "minus" if character == "-" else character
            )
        )
        time.sleep(0.15)
    monitor("sendkey ret")
    time.sleep(2)
    screenshot = scratch / "terminal.ppm"
    monitor("screendump " + str(screenshot))
    time.sleep(1)
    assert screenshot.exists(), "Terminal screenshot missing"
    assert screenshot_contains(
        screenshot, "gui-terminal-ok"
    ), "Shell output missing from Terminal pixels"
    print(f"Terminal screenshot: {screenshot}", flush=True)
    for character in "exit":
        monitor("sendkey " + character)
        time.sleep(0.15)
    monitor("sendkey ret")
    while time.monotonic() < deadline:
        text = serial.read_text(errors="replace")
        if "terminal-ui: clean exit passed" in text:
            print("PASS graphical Terminal: keyboard input and clean shell exit")
            return
        if "terminal-ui: FAILED" in text:
            raise AssertionError(text[-4000:])
        time.sleep(0.1)
    raise AssertionError("Terminal exit timeout")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--terminal",
        action="store_true",
        help="also exercise graphical Terminal input and output",
    )
    parser.add_argument(
        "--no-build",
        action="store_true",
        help="reuse existing kernel and user binaries",
    )
    options = parser.parse_args()
    scratch = Path(tempfile.mkdtemp(prefix="nxu-shell-session-", dir="/private/tmp"))
    print(f"Shell session artifacts: {scratch}", flush=True)
    targets = [
        "bootd",
        "nzh",
        "ping",
        "ifconfig",
        "terminaltest",
        "ipctest_a",
        "ipctest_b",
        "sockettest_server",
        "sockettest_client",
    ]
    if options.terminal:
        targets += [
            "terminal",
            "windowserver_service",
            "desktop_service",
            "dyrld",
            "dyrld-runtime",
        ]
    with (scratch / "build.log").open("w") as log:

        def run(command):
            subprocess.run(
                command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True
            )

        if not options.no_build:
            run(
                ["make", "-j8", "all"]
                if options.terminal
                else [
                    "make",
                    "-j8",
                    "CONFIG=network-standalone",
                    "UISERVICE=0",
                    "WINDOWSERVER=0",
                    "all",
                ]
            )
        if not options.no_build:
            run(
                [
                    "make",
                    "-j8",
                    "USER_CC=clang",
                    *["BUILD/userland/" + name for name in targets],
                ]
            )
        run(
            [
                "clang",
                "--target=aarch64-none-elf",
                "-ffreestanding",
                "-fno-stack-protector",
                "-fno-pic",
                "-mgeneral-regs-only",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Iframeworks/include",
                "-I.",
                "-c",
                "tools/fixtures/net/session_test.c",
                "-o",
                str(scratch / "fixture.o"),
            ]
        )
        objects = [
            "frameworks/crt0",
            "frameworks/lib/syscall",
            "frameworks/lib/string",
            "frameworks/lib/thread",
            "frameworks/CoreFoundation.framework/lib/plist/plist",
            "frameworks/CoreFoundation.framework/lib/service/service_config",
            "frameworks/CoreFoundation.framework/lib/service/bootstrap_client",
            "frameworks/CoreFoundation.framework/sbin/bootd/job",
            "frameworks/CoreFoundation.framework/sbin/bootd/manager",
            "frameworks/CoreFoundation.framework/sbin/bootd/registry",
        ]
        run(
            [
                "ld.lld",
                "-T",
                "makedefs/user.ld",
                "-nostdlib",
                "-static",
                *["BUILD/userland/" + name + ".o" for name in objects],
                str(scratch / "fixture.o"),
                "-o",
                str(scratch / "bootd"),
            ]
        )
        diskroot = scratch / "DiskRoot"
        services = diskroot / "System/Library/CoreServices"
        services.mkdir(parents=True)
        (diskroot / "System/Library/BootDaemons").mkdir()
        shutil.copyfile(ROOT / "tools/DiskRoot/hello.txt", diskroot / "hello.txt")
        shutil.copyfile(
            ROOT / "tools/DiskRoot/System/README.txt", diskroot / "System/README.txt"
        )
        for name in targets:
            shutil.copyfile(ROOT / "BUILD/userland" / name, services / name)
        shutil.copyfile(scratch / "bootd", services / "bootd")
        shutil.copyfile(scratch / "bootd", services / "bootd.recovery")
        if options.terminal:
            shutil.copytree(
                ROOT / "tools/DiskRoot/System/Library/Fonts",
                diskroot / "System/Library/Fonts",
            )
            for name in [
                "Runtime",
                "CoreFoundation",
                "WindowServer",
                "CoreGraphics",
                "DesktopService",
            ]:
                directory = (
                    diskroot / "System/Library/Frameworks" / (name + ".framework")
                )
                directory.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(
                    ROOT / "BUILD/userland" / (name + ".so"), directory / (name + ".so")
                )
            shutil.copyfile(
                ROOT
                / "tools/DiskRoot/System/Library/BootDaemons/com.nxu.windowserver.plist",
                diskroot / "System/Library/BootDaemons/com.nxu.windowserver.plist",
            )
        if options.terminal:
            shutil.copyfile(
                ROOT / "tools/DiskRoot/System/Library/BootDaemons/com.nxu.dyrld.plist",
                diskroot / "System/Library/BootDaemons/com.nxu.dyrld.plist",
            )
        disk = scratch / "disk.img"
        with disk.open("wb") as output:
            output.truncate(128 * 1024 * 1024)
        mkfs = shutil.which("mkfs.ext4") or "/usr/local/opt/e2fsprogs/sbin/mkfs.ext4"
        run(
            [
                mkfs,
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
                str(diskroot),
                str(disk),
            ]
        )
    serial = scratch / "serial.log"
    command = [
        "qemu-system-aarch64",
        "-machine",
        "virt,gic-version=3",
        "-cpu",
        "cortex-a72",
        "-smp",
        "2",
        "-m",
        "512M",
        "-kernel",
        str(
            ROOT
            / (
                "BUILD/default/kernel.bin"
                if options.terminal
                else "BUILD/network-standalone/kernel.bin"
            )
        ),
        "-display",
        "none",
        "-global",
        "virtio-mmio.force-legacy=false",
        "-drive",
        f"if=none,format=raw,file={disk},id=nxudisk",
        "-device",
        "virtio-blk-device,drive=nxudisk",
        "-netdev",
        "user,id=nxunet,ipv4=on,ipv6=on",
        "-device",
        "virtio-net-device,netdev=nxunet",
        "-serial",
        f"file:{serial}",
        "-monitor",
        "none",
        "-no-reboot",
        "-append",
        "-no-gpu -no-input -no-sound -no-windowserver -no-desktop -no-crash-ui -no-patchd log.trace=0 log.stats_ms=0",
    ]
    if options.terminal:
        command[command.index("-monitor") + 1] = "stdio"
        command[-1] = (
            "-no-sound -no-windowserver -no-desktop -no-crash-ui -no-patchd terminal.test=1 ui.scale=1000 dyrld.aslr=0 log.trace=0 log.stats_ms=0"
        )
        command += [
            "-device",
            "virtio-gpu-device,xres=1024,yres=768",
            "-device",
            "virtio-keyboard-device",
            "-device",
            "virtio-mouse-device",
        ]
    with (scratch / "qemu.log").open("w") as log:
        process = subprocess.Popen(
            command,
            cwd=ROOT,
            stdin=subprocess.PIPE,
            stdout=log,
            stderr=subprocess.STDOUT,
        )
        try:
            deadline = time.monotonic() + 120
            while time.monotonic() < deadline and process.poll() is None:
                text = serial.read_text(errors="replace") if serial.exists() else ""
                if "shell-session: networking and executable tests passed" in text:
                    print(
                        "PASS ARM64 shell: IPv4/IPv6 ping, IPC, local sockets, fork/exec and redirected output"
                    )
                    if options.terminal:
                        test_terminal(process, serial, scratch)
                    return
                if "shell-session: FAILED" in text or "panic:" in text:
                    raise AssertionError(text[-6000:])
                time.sleep(0.1)
            raise AssertionError(f"Shell session timeout; see {serial}")
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


if __name__ == "__main__":
    main()
