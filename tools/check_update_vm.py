#!/usr/bin/env python3
"""Exercise queued offline updates, trial confirmation and rollback in QEMU."""

import argparse
import hashlib
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile
import struct
import time

ROOT = Path(__file__).resolve().parent.parent


def run(command, **kwargs):
    return subprocess.run(command, cwd=ROOT, check=True, **kwargs)


def image(path, root, megabytes):
    with path.open("wb") as stream:
        stream.truncate(megabytes * 1024 * 1024)
    run(
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
            str(path),
        ]
    )


def read(disk, path):
    return run(["debugfs", "-R", "cat " + path, str(disk)], capture_output=True).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--damage-primary-gpt",
        action="store_true",
        help="test discovery from the backup GPT",
    )
    parser.add_argument(
        "--partitioned",
        action="store_true",
        help="use one GPT SSD and automatic recovery discovery",
    )
    parser.add_argument(
        "--no-build", action="store_true", help="reuse binaries for parallel VM cases"
    )
    parser.add_argument("--arch", choices=["arm64", "i386"], default="arm64")
    parser.add_argument(
        "--case",
        choices=[
            "success",
            "core",
            "core-unconfirmed",
            "failure",
            "unconfirmed",
            "interrupted",
            "tamper",
            "recovery",
            "incomplete",
            "unsupported",
            "watchdog",
            "partition-recovery",
            "missing-recovery",
        ],
        default="success",
    )
    args = parser.parse_args()
    if args.case == "partition-recovery" and not args.partitioned:
        parser.error("partition-recovery requires --partitioned")
    i386 = args.arch == "i386"
    scratch = Path(tempfile.mkdtemp(prefix="nxu-install-", dir="/private/tmp"))
    print(f"Update VM artifacts: {scratch}", flush=True)
    user = ROOT / ("BUILD/i386/user" if i386 else "BUILD/userland")
    if not args.no_build:
        run(
            [
                "make",
                "-j8",
                "i386" if i386 else "all",
                *[
                    str((user / name).relative_to(ROOT))
                    for name in ("bootd", "logd", "patchd", "triageOS")
                ],
            ]
        )
    root = scratch / "root"
    core = root / "System/Library/CoreServices"
    jobs = root / "System/Library/BootDaemons"
    core.mkdir(parents=True)
    jobs.mkdir()
    (root / "var/db/patchd").mkdir(parents=True)
    (root / "var/log").mkdir()
    (root / "Users/test").mkdir(parents=True)
    (root / "Users/test/keep.txt").write_text("user data\n")
    (root / "var/keep.txt").write_text("state data\n")
    shutil.copyfile(ROOT / "tools/DiskRoot/hello.txt", root / "hello.txt")
    shutil.copyfile(
        ROOT / "tools/DiskRoot/System/README.txt", root / "System/README.txt"
    )
    (root / "System/update-test.txt").write_text("old system\n")
    for name in ("bootd", "logd", "patchd"):
        for suffix in ("", ".recovery"):
            shutil.copyfile(user / name, core / (name + suffix))
    for name in ("logd", "patchd"):
        shutil.copyfile(
            ROOT / f"frameworks/BootDaemons.framework/Services/com.nxu.{name}.plist",
            jobs / f"com.nxu.{name}.plist",
        )
    (jobs / "com.nxu.install-test.plist").write_text(
        "<plist><dict><key>Label</key><string>com.nxu.install-test</string>"
        "<key>Program</key><string>/disk/System/Library/CoreServices/install-test</string>"
        "<key>RunAtLoad</key><true/><key>KeepAlive</key><false/>"
        "<key>AllowFilesystemWrite</key><true/></dict></plist>\n"
    )
    obj = scratch / "probe.o"
    run(
        [
            "clang",
            "--target=" + args.arch.replace("arm64", "aarch64") + "-none-elf",
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
            "tools/fixtures/update/install_vm.c",
            "-o",
            str(obj),
        ]
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
    run(
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
            str(core / "install-test"),
        ]
    )
    updated = scratch / "updated"
    shutil.copytree(root, updated)
    (updated / "System/update-test.txt").write_text("new system\n")
    (updated / "System/README.txt").write_text("Updated system build\n")
    (updated / "Applications/UpdateTest.app").mkdir(parents=True)
    (updated / "Applications/UpdateTest.app/new.txt").write_text("new application\n")
    # Payload user/state trees must never be copied to the live installation.
    (updated / "Users/test/keep.txt").write_text("REPLACE USER DATA\n")
    (updated / "var/keep.txt").write_text("REPLACE STATE\n")
    if args.case in ("core", "core-unconfirmed"):
        for name in ("bootd", "logd", "patchd"):
            for suffix in ("", ".recovery"):
                program = updated / "System/Library/CoreServices" / (name + suffix)
                # A harmless ELF trailer changes the bytes while retaining real daemon behavior.
                program.write_bytes(program.read_bytes() + b"NXU updated core daemon\n")
    if args.case == "interrupted":
        (updated / "System/interrupted.bin").write_bytes(b"i" * (3 * 1024 * 1024))
    if args.case == "unsupported":
        (updated / "System/escape").symlink_to("/Users/test/keep.txt")
    recovery_payload_root = scratch / "recovery-payload"
    (recovery_payload_root / "System/Recovery").mkdir(parents=True)
    shutil.copyfile(
        user / "triageOS", recovery_payload_root / "System/Recovery/triageOS"
    )
    recovery = scratch / "Recovery"
    prepared = recovery / "Prepared/install-test"
    prepared.mkdir(parents=True)
    image(prepared / "install-test.img", updated, 16)
    image(prepared / "RecoveryRamdisk.img", recovery_payload_root, 8)
    manifest = {"FormatVersion": 1, "Build": "install-test", "Architecture": args.arch}
    for key, name in (
        ("SystemImage", "install-test.img"),
        ("RecoveryRamdisk", "RecoveryRamdisk.img"),
    ):
        manifest[key] = name
        manifest[key + "SHA256"] = hashlib.sha256(
            (prepared / name).read_bytes()
        ).hexdigest()
        manifest[key + "Format"] = "ext4"
    (prepared / "BuildManifest.plist").write_bytes(
        plistlib.dumps(manifest, sort_keys=False)
    )
    (prepared / "Ready").write_text("verified\n")
    if args.partitioned:
        (recovery / "System/Recovery").mkdir(parents=True)
        shutil.copyfile(user / "triageOS", recovery / "System/Recovery/triageOS")
    disk = scratch / "disk.img"
    recovery_disk = scratch / "recovery.img"
    image(disk, root, 64)
    image(recovery_disk, recovery, 96)

    ssd = scratch / "ssd.img"
    partition_starts = []
    if args.partitioned:
        run(
            [
                "python3",
                "tools/make_system_disk.py",
                "--system-image",
                str(disk),
                "--recovery-image",
                str(recovery_disk),
                "--output",
                str(ssd),
            ]
        )
        with ssd.open("rb") as stream:
            stream.seek(2 * 512)
            table = stream.read(128 * 128)
        if args.damage_primary_gpt:
            with ssd.open("r+b") as stream:
                stream.seek(512 + 16)
                stream.write(b"\0\0\0\0")
        partition_starts = [
            struct.unpack_from("<Q", table, index * 128 + 32)[0] * 512
            for index in range(2)
        ]

    def transfer(to_ssd):
        if not args.partitioned:
            return
        with ssd.open("r+b" if to_ssd else "rb") as whole:
            for component, start in zip((disk, recovery_disk), partition_starts):
                size = component.stat().st_size
                whole.seek(start)
                with component.open("rb" if to_ssd else "r+b") as part:
                    for offset in range(0, size, 1024 * 1024):
                        count = min(size - offset, 1024 * 1024)
                        if to_ssd:
                            whole.write(part.read(count))
                        else:
                            part.write(whole.read(count))

    def boot(number, expected, extra=""):
        transfer(True)
        serial = scratch / f"serial-{number}.log"
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
            "none",
            "-serial",
            "file:" + str(serial),
            "-no-reboot",
        ]
        for index, drive in enumerate(
            (ssd,) if args.partitioned else (disk, recovery_disk)
        ):
            command += ["-drive", f"if=none,format=raw,file={drive},id=d{index}"]
        for index in ((0,) if args.partitioned else ((0, 1) if i386 else (1, 0))):
            command += [
                "-device",
                (
                    f"virtio-blk-pci,drive=d{index},disable-modern=on"
                    if i386
                    else f"virtio-blk-device,drive=d{index}"
                ),
            ]
        command += [
            "-append",
            ("" if args.partitioned else "recovery.volume=1 ")
            + "-no-gpu -no-input -no-sound -no-windowserver -no-desktop -no-crash-ui log.trace=0 log.stats_ms=0 "
            + extra,
        ]
        with (scratch / f"qemu-{number}.log").open("w") as log:
            process = subprocess.Popen(command, stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 180
                while time.monotonic() < deadline:
                    text = serial.read_text(errors="replace") if serial.exists() else ""
                    if (
                        "install-vm: FAIL" in text
                        or "patchd: installation request rejected" in text
                        or (
                            "update preflight failed" in text
                            and "preflight failed" not in expected
                        )
                    ):
                        raise AssertionError(text[-8000:])
                    if expected in text:
                        if number == 1 or "restarting to roll back" in expected:
                            process.wait(timeout=20)
                        break
                    if process.poll() is not None:
                        raise AssertionError(f"QEMU exited: {text[-8000:]}")
                    time.sleep(0.5)
                else:
                    raise AssertionError(
                        f"Missing {expected}: {serial}\n{text[-8000:]}"
                    )
            finally:
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=10)
        transfer(False)
        if args.partitioned:
            assert "boot: selected GPT system volume" in text
            if args.case == "missing-recovery" and number == 3:
                assert "recovery: volume mount failed" in text
            else:
                assert "recovery: mounted update volume" in text
        return text

    if args.case == "partition-recovery":
        with disk.open("r+b") as image_file:
            image_file.seek(1080)
            image_file.write(b"\0\0")
        text = boot(0, "triageOS: recovery display unavailable")
        assert "system mount failed; starting the recovery partition" in text
        assert "loading /Volumes/Recovery/System/Recovery/triageOS as PID 1" in text
        print(
            f"PASS: {args.arch} corrupt system boots standalone recovery on the same SSD",
            flush=True,
        )
        return

    boot(1, "patchd: restarting to install the queued system update")
    pending = read(recovery_disk, "/InstallPending").decode().splitlines()
    assert len(pending) == 3, pending
    transaction = "/Transactions/" + pending[1]
    if args.case == "incomplete":
        run(
            ["debugfs", "-w", "-R", "rm /InstallReady", str(recovery_disk)],
            capture_output=True,
        )
        text = boot(2, "patchd: incomplete installation queue discarded")
        assert "patchd: system files were not changed\n" in text
        assert read(recovery_disk, "/InstallPending") == b""
    elif args.case == "unsupported":
        text = boot(2, "patchd: update preflight failed")
        assert "patchd: system files were not changed\n" in text
    elif args.case == "tamper":
        replacement = scratch / "tampered-manifest"
        replacement.write_text("bad manifest\n")
        run(
            [
                "debugfs",
                "-w",
                "-R",
                "rm /Prepared/install-test/BuildManifest.plist",
                str(recovery_disk),
            ],
            capture_output=True,
        )
        run(
            [
                "debugfs",
                "-w",
                "-R",
                f"write {replacement} /Prepared/install-test/BuildManifest.plist",
                str(recovery_disk),
            ],
            capture_output=True,
        )
        text = boot(2, "patchd: update preflight failed")
        assert "patchd: system files were not changed\n" in text
    elif args.case == "interrupted":
        boot(2, "applying patches to /disk/System/interrupted.bin", "-no-patchd")
        assert read(recovery_disk, transaction + "/State") == b"applying\n"
        boot(3, "interrupted or unconfirmed update rolled back", "-no-patchd")
        assert read(disk, "/System/interrupted.bin") == b""
    elif args.case == "failure":
        text = boot(2, "failed installation rolled back", "update.test.fail_after=2")
        assert "applying patches to /disk/System/update-test.txt" in text
    else:
        marker = (
            "updated boot health confirmed"
            if args.case in ("success", "core")
            else "patchd: waiting for the updated system to confirm its boot"
        )
        if args.case == "watchdog":
            marker = "patchd: restarting to roll back"
        text = boot(2, marker, "" if args.case in ("success", "core") else "-no-patchd")
        if args.case not in ("success", "core", "watchdog"):
            assert "patchd: patches applied\n" in text
        if args.case == "watchdog":
            assert "patchd: updated boot was not confirmed\n" in text
            assert "patchd: restarting to roll back\n" in text
        assert "applying patches to /disk/System/update-test.txt" in text
        assert "applying patches to /disk/Applications/UpdateTest.app/new.txt" in text
        if args.case in ("core", "core-unconfirmed"):
            for name in ("bootd", "logd", "patchd"):
                assert (
                    "applying patches to /disk/System/Library/CoreServices/" + name
                    in text
                )
                assert read(disk, "/System/Library/CoreServices/" + name).endswith(
                    b"NXU updated core daemon\n"
                )
        assert read(disk, "/System/update-test.txt") == b"new system\n"
        assert (
            read(disk, "/Applications/UpdateTest.app/new.txt") == b"new application\n"
        )
        if args.case in ("success", "core"):
            assert read(recovery_disk, transaction + "/State") == b"committed\n"
            assert read(recovery_disk, "/InstallPending") == b""
            # A committed build must remain installed after another boot.
            boot(3, "patchd: integrity repair service started")
        elif args.case == "missing-recovery":
            assert read(disk, "/var/db/patchd/update-active")
            with recovery_disk.open("r+b") as image_file:
                image_file.seek(1080)
                image_file.write(b"\0\0")
            boot(3, "refusing a partial system boot", "-no-patchd")
            assert read(disk, "/var/db/patchd/update-active")
            assert read(disk, "/Users/test/keep.txt") == b"user data\n"
            print(
                f"PASS: {args.arch} unavailable recovery refuses an active trial boot",
                flush=True,
            )
            return
        elif args.case == "recovery":
            # Corrupt all backups to force the verified recovery payload path.
            entries = read(recovery_disk, transaction + "/Entries")
            for index in range(len(entries) // 320):
                run(
                    [
                        "debugfs",
                        "-w",
                        "-R",
                        f"rm {transaction}/Backup/{index:08x}",
                        str(recovery_disk),
                    ],
                    capture_output=True,
                )
            text = boot(3, "triageOS: recovery display unavailable", "-no-patchd")
            assert "patchd: starting the verified recovery environment" in text
            assert "loading /UpdateRecovery/System/Recovery/triageOS as PID 1" in text
        else:
            boot(3, "interrupted or unconfirmed update rolled back", "-no-patchd")
    if args.case == "core-unconfirmed":
        for name in ("bootd", "logd", "patchd"):
            assert (
                read(disk, "/System/Library/CoreServices/" + name)
                == (user / name).read_bytes()
            )
    for key in ("SystemImage", "RecoveryRamdisk"):
        payload = read(recovery_disk, "/Prepared/install-test/" + manifest[key])
        assert hashlib.sha256(payload).hexdigest() == manifest[key + "SHA256"]
    assert read(disk, "/Users/test/keep.txt") == b"user data\n"
    assert read(disk, "/var/keep.txt") == b"state data\n"
    if args.case in (
        "failure",
        "unconfirmed",
        "core-unconfirmed",
        "interrupted",
        "tamper",
        "incomplete",
        "unsupported",
        "watchdog",
    ):
        assert read(disk, "/System/update-test.txt") == b"old system\n"
        assert read(disk, "/Applications/UpdateTest.app/new.txt") == b""
    elif args.case in ("success", "core"):
        assert read(disk, "/System/README.txt") == b"Updated system build\n"
        assert read(disk, "/System/update-test.txt") == b"new system\n"
    print(
        f"PASS: {args.arch} {args.case}; preserved user/state files and immutable payloads",
        flush=True,
    )


if __name__ == "__main__":
    main()
