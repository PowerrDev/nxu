#!/usr/bin/env python3
"""Boot the networking regression through modern MMIO and legacy/modern PCI."""

import argparse
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
MARKER = "net_test: IPv4/IPv6 loopback and VirtIO gateway echo passed"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=("arm64", "i386"), default="arm64")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--timeout", type=float, default=60)
    options = parser.parse_args()
    scratch = Path(tempfile.mkdtemp(prefix="nxu-network-vm-", dir="/private/tmp"))
    print(f"Networking artifacts: {scratch}", flush=True)

    if options.arch == "arm64":
        kernel = ROOT / "BUILD/network-standalone/kernel.bin"
        build = [
            "make",
            "-j8",
            "CONFIG=network-standalone",
            "UISERVICE=0",
            "WINDOWSERVER=0",
            "all",
        ]
        transports = ("mmio",)
    else:
        kernel = ROOT / "BUILD/BUILD-i386-desktop/i386/kernel.elf"
        build = ["make", "-j8", "BUILD_ROOT=BUILD/BUILD-i386-desktop", "i386"]
        transports = ("legacy-pci", "modern-pci")

    if not options.no_build:
        with (scratch / "build.log").open("w") as output:
            subprocess.run(
                build, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT, check=True
            )

    for transport in transports:
        serial = scratch / (transport + ".serial.log")
        pcap = scratch / (transport + ".pcap")
        if transport == "mmio":
            command = [
                "qemu-system-aarch64",
                "-machine",
                "virt,gic-version=3",
                "-cpu",
                "cortex-a72",
                "-global",
                "virtio-mmio.force-legacy=false",
                "-device",
                "virtio-net-device,netdev=nxunet",
            ]
        else:
            mode = (
                "disable-modern=on"
                if transport == "legacy-pci"
                else "disable-legacy=on"
            )
            command = [
                "qemu-system-i386",
                "-cpu",
                "qemu32,+nx",
                "-device",
                "virtio-net-pci,netdev=nxunet," + mode,
                "-device",
                "isa-debug-exit,iobase=0xf4,iosize=0x04",
            ]

        command += [
            "-m",
            "512M",
            "-smp",
            "2",
            "-kernel",
            str(kernel),
            "-display",
            "none",
            "-netdev",
            "user,id=nxunet,net=10.0.2.0/24,host=10.0.2.2,ipv4=on,ipv6=on",
            "-object",
            f"filter-dump,id=wire,netdev=nxunet,file={pcap}",
            "-serial",
            "file:" + str(serial),
            "-monitor",
            "none",
            "-no-reboot",
            "-append",
            "net.test=1 test=network qemu-exit=1 -no-block -no-gpu -no-input -no-sound log.trace=0 log.stats_ms=0",
        ]
        with (scratch / (transport + ".qemu.log")).open("w") as output:
            process = subprocess.Popen(
                command, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT
            )
            deadline = time.monotonic() + options.timeout
            try:
                while time.monotonic() < deadline:
                    text = serial.read_text(errors="replace") if serial.exists() else ""
                    if MARKER in text:
                        print(
                            f"PASS {options.arch} {transport}: IPv4/IPv6 loopback and gateway ping",
                            flush=True,
                        )
                        break
                    if (
                        ("net_test: case" in text and "failed" in text)
                        or "panic:" in text
                        or "Kernel halted." in text
                        or process.poll() is not None
                    ):
                        raise AssertionError(
                            f"Networking boot failed; see {serial}\n{text[-4000:]}"
                        )
                    time.sleep(0.1)
                else:
                    raise AssertionError(f"Networking timeout; see {serial}")
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
