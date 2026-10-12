#!/usr/bin/env python3
"""Exercise recovery SAP cancellation, password authentication and directory removal."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from check_gui_service import read_ppm
from tep_test_fixture import EnclaveFixture

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=["i386", "arm64"], required=True)
    parser.add_argument("--unavailable", action="store_true", help="verify refusal without an enclave link")
    args = parser.parse_args()
    os.environ["PATH"] = "/opt/homebrew/opt/e2fsprogs/sbin:/usr/local/opt/e2fsprogs/sbin:" + os.environ["PATH"]
    scratch = Path(tempfile.mkdtemp(prefix="nxu-recovery-sap-" + args.arch + "-"))
    print("Recovery SAP artifacts: " + str(scratch), flush=True)
    i386 = args.arch == "i386"
    build = ROOT / ("BUILD/BUILD-i386-desktop/i386/user" if i386 else "BUILD/userland")

    def run(command):
        subprocess.run(command, cwd=ROOT, check=True)

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
    obj = scratch / "sap.o"
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
            "tools/fixtures/recovery/sap.c",
            "-o",
            str(obj),
        ]
    )
    objects = (
        [
            "frameworks/crt0_i386.o",
            "frameworks/lib/syscall_i386.o",
            "frameworks/lib/thread_i386.o",
            "frameworks/lib/spawn_io_i386.o",
        ]
        if i386
        else ["frameworks/crt0.o", "frameworks/lib/syscall.o", "frameworks/lib/thread.o"]
    )
    objects += [
        "frameworks/lib/string.o",
        "frameworks/Recovery.framework/lib/RecoveryServices.o",
        "frameworks/Recovery.framework/triageOS/shell.o",
        "frameworks/SoftwareUpdate.framework/lib/nsw.o",
        "frameworks/SoftwareUpdate.framework/lib/manifest.o",
        "frameworks/CoreFoundation.framework/lib/plist/plist.o",
        "libk/sha256.o",
    ]
    root = scratch / "root"
    (root / "System/Recovery").mkdir(parents=True)
    run(
        [
            "ld.lld",
            *(["-m", "elf_i386"] if i386 else []),
            "-T",
            "makedefs/user-i386.ld" if i386 else "makedefs/user.ld",
            "-nostdlib",
            "-static",
            *[str(build / path) for path in objects],
            str(obj),
            *([str(build / "libnxurt.a")] if i386 else []),
            "-o",
            str(root / "System/Recovery/triageOS"),
        ]
    )
    tree = root / "System/Library/Frameworks/SAPTree.framework"
    (tree / "nested").mkdir(parents=True)
    (tree / "nested/file.txt").write_text("keep tree")
    (tree / "second.txt").write_text("second child")
    # A long branch verifies that recursive approval does not use the 16 KiB user stack.
    deep = tree
    for _ in range(20):
        deep /= "a"
        deep.mkdir()
    (deep / "leaf.txt").write_text("deep child")
    keep = root / "System/Library/Frameworks/Keep.framework"
    keep.mkdir()
    (keep / "keep.txt").write_text("keep sibling")
    disk = scratch / "disk.img"
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
    pipe = scratch / "tep"
    for suffix in (".in", ".out"):
        os.mkfifo(str(pipe) + suffix)
    password = "sap-password"
    enclave = None if args.unavailable else EnclaveFixture(pipe, password, transport="pipe")
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
    if enclave:
        command += ["-chardev", f"pipe,id=tep,path={pipe}", "-serial", "chardev:tep"]
    else:
        command[command.index("-append") + 1] += " test.sap-unavailable=1"
    with (scratch / "qemu.log").open("w") as log:
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT)
        if enclave:
            enclave.start()
        deadline = time.monotonic() + 180

        def text():
            return serial.read_text(errors="replace") if serial.exists() else ""

        def monitor(line):
            process.stdin.write((line + "\n").encode())
            process.stdin.flush()

        def wait(marker, count=1):
            while time.monotonic() < deadline:
                content = text()
                assert "RECOVERY SAP FAIL" not in content, content[-4000:]
                if content.count(marker) >= count:
                    return
                if process.poll() is not None:
                    raise AssertionError("QEMU exited: " + content[-4000:])
                time.sleep(0.1)
            raise AssertionError("Timeout waiting for " + marker + ": " + content[-4000:])

        def key(code):
            monitor("sendkey " + code)
            time.sleep(0.15)

        def type_text(value):
            for character in value:
                key("minus" if character == "-" else character)

        def capture(name, panel=True):
            path = scratch / (name + ".ppm")
            for _ in range(50):
                monitor("screendump " + str(path))
                time.sleep(0.1)
                try:
                    width, height, pixels = read_ppm(path)
                    offset = (91 * width + 41) * 3
                    color = bytes([239, 239, 241]) if panel else bytes([0, 96, 64])
                    if pixels[offset : offset + 3] == color:
                        return
                except (FileNotFoundError, ValueError, AssertionError):
                    pass
            raise AssertionError("Modal or recovery surface missing from scanout")

        try:
            wait("sap: confirmation requested")
            capture("cancel")
            key("esc")
            wait("sap: confirmation requested", 2)
            key("ret")
            wait("sap: password requested")
            type_text("incorrect")
            key("ret")
            wait("sap: password refused")
            capture("refused")
            key("esc")
            if args.unavailable:
                wait("RECOVERY SAP UNAVAILABLE PASS")
                capture("restored", panel=False)
            else:
                wait("sap: confirmation requested", 3)
                key("ret")
                wait("sap: password requested", 2)
                type_text(password)
                capture("masked")
                key("ret")
                wait("recovery-sap: fresh-approval phase")
                wait("sap: confirmation requested", 4)
                capture("fresh-approval")
                key("esc")
                wait("RECOVERY SAP PASS")
                capture("restored", panel=False)
                assert password not in text(), "password leaked into logs"
                assert enclave.error is None and enclave.denials >= 1 and enclave.accepts >= 1

        finally:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            if enclave:
                enclave.close()
    run(["e2fsck", "-fn", str(disk)])
    result = subprocess.run(
        ["debugfs", "-R", "stat /System/Library/Frameworks/SAPTree.framework", str(disk)],
        capture_output=True,
        text=True,
        check=True,
    )
    assert ("File not found" in result.stderr) != args.unavailable, "tree persistence did not match authorization"
    restored = scratch / "keep.txt"
    run(["debugfs", "-R", f"dump /System/Library/Frameworks/Keep.framework/keep.txt {restored}", str(disk)])
    assert restored.read_text() == "keep sibling"
    print(
        "PASS "
        + args.arch
        + " recovery SAP: "
        + (
            "unavailable authentication refused"
            if args.unavailable
            else "cancel, wrong password, exact tree, fresh approval, exclusive input and scanout"
        ),
        flush=True,
    )


if __name__ == "__main__":
    main()
