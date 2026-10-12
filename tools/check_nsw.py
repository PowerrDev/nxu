#!/usr/bin/env python3
"""Exercise the real NSW reader with ISO packages and injected I/O failures."""

import hashlib
from pathlib import Path
import struct
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def main():
    scratch = Path(tempfile.mkdtemp(prefix="nxu-nsw-check-", dir="/private/tmp"))
    print(f"NSW test artifacts: {scratch}", flush=True)
    common = [
        "clang",
        "-O1",
        "-g",
        "-fsanitize=address,undefined",
        "-D_FORTIFY_SOURCE=0",
    ]
    objects = []
    for source, includes in [
        ("tools/fixtures/update/nsw_host.c", ["-I.", "-Iframeworks/include"]),
        (
            "frameworks/SoftwareUpdate.framework/lib/nsw.c",
            ["-I.", "-Iframeworks/include"],
        ),
        ("frameworks/CoreFoundation.framework/lib/plist/plist.c", ["-I."]),
        ("frameworks/SoftwareUpdate.framework/lib/manifest.c", ["-I."]),
        ("libk/sha256.c", ["-Ilibk"]),
    ]:
        obj = scratch / (Path(source).stem + ".o")
        subprocess.run(
            common + includes + ["-c", source, "-o", str(obj)], cwd=ROOT, check=True
        )
        objects.append(str(obj))
    executable = scratch / "nsw-host"
    subprocess.run(common + objects + ["-o", str(executable)], check=True)
    images = []
    for name in ("system", "recovery"):
        image = scratch / (name + ".img")
        with image.open("wb") as stream:
            stream.truncate(8 * 1024 * 1024)
        subprocess.run(["mkfs.ext4", "-q", "-F", str(image)], check=True)
        images.append(image)
    package = scratch / "build-test.nsw"
    subprocess.run(
        [
            "python3",
            "tools/make_nsw.py",
            "--build",
            "test-1",
            "--arch",
            "arm64",
            "--system-image",
            str(images[0]),
            "--recovery-ramdisk",
            str(images[1]),
            "--output",
            str(package),
        ],
        cwd=ROOT,
        check=True,
    )
    data = package.read_bytes()

    def check(name, contents, expected=False, failure=None):
        path = scratch / (name + ".nsw")
        path.write_bytes(contents)
        destination = scratch / name
        result = subprocess.run(
            [str(executable), str(path), str(destination)]
            + ([failure] if failure else [])
        )
        assert result.returncode == (0 if expected else 1), (name, result.returncode)
        ready = destination / "test-1/Ready"
        assert ready.exists() == expected, name
        assert not (destination / ".prepare-lock").exists(), name
        return destination

    destination = check("valid", data, True)
    for name, source in zip(("test-1.img", "RecoveryRamdisk.img"), images):
        assert (
            hashlib.sha256((destination / "test-1" / name).read_bytes()).digest()
            == hashlib.sha256(source.read_bytes()).digest()
        )
    assert (destination / "test-1/Ready").read_text() == "verified\n"
    assert (
        subprocess.run([str(executable), str(package), str(destination)]).returncode
        == 1
    )
    assert (destination / "test-1/Ready").read_text() == "verified\n"

    # The installation queue binds the exact manifest, refuses duplicate work,
    # and only commits a trial transaction. Host paths stay inside the scratch tree.
    recovery = scratch / "queue-volume"
    (recovery / "Prepared").mkdir(parents=True)
    shutil.copytree(destination / "test-1", recovery / "Prepared/test-1")

    def queue(mode="schedule", expected=0, build="test-1"):
        result = subprocess.run([str(executable), build, str(recovery), mode])
        assert result.returncode == expected, (mode, result.returncode)

    queue(build="../test-1", expected=1)
    queue(mode="schedule-sync", expected=1)
    assert not (recovery / "InstallReady").exists()
    assert not (recovery / "InstallPending").exists()
    queue()
    pending = (recovery / "InstallPending").read_text().splitlines()
    assert pending[0] == "test-1" and len(pending) == 3
    assert (
        pending[2]
        == hashlib.sha256(
            (recovery / "Prepared/test-1/BuildManifest.plist").read_bytes()
        ).hexdigest()
    )
    assert (recovery / "InstallReady").read_text() == "queued\n"
    queue(expected=1)
    state = recovery / "Transactions" / pending[1] / "State"
    state.write_text("applying\n")
    queue(mode="confirm", expected=1)
    state.write_text("trial\n")
    active = recovery / "SystemVolume/var/db/patchd/update-active"
    active.parent.mkdir(parents=True)
    active.write_text("active\n")
    queue(mode="confirm")
    assert not active.exists()
    assert state.read_text() == "committed\n"
    assert not (recovery / "InstallPending").exists()
    assert not (recovery / "InstallReady").exists()
    (recovery / "Prepared/.prepare-lock").mkdir()
    queue(expected=1)
    (recovery / "Prepared/.prepare-lock").rmdir()
    staged = recovery / "Prepared/test-1/test-1.img"
    with staged.open("r+b") as stream:
        stream.seek(4096)
        stream.write(b"tampered payload")
    queue(expected=1)
    assert not (recovery / "InstallReady").exists()

    def replace(old, new):
        assert len(old) == len(new) and data.count(old) == 1
        return data.replace(old, new)

    check(
        "wrong-architecture",
        replace(b"<string>arm64</string>", b"<string>i386</string> "),
    )
    digest = hashlib.sha256(images[0].read_bytes()).hexdigest().encode()
    check("bad-system-hash", replace(digest, b"0" * 64))
    check(
        "path-traversal",
        replace(b"<string>test-1.img</string>", b"<string>../1xx.img</string>"),
    )
    check(
        "duplicate-key", replace(b"<key>Architecture</key>", b"<key>SystemImage</key> ")
    )
    check("truncated", data[:40000])
    check("failed-write", data, failure="write")
    check("failed-sync", data, failure="sync")
    check("corrupt-staged-data", data, failure="corrupt")
    partial = scratch / "partial/test-1"
    partial.mkdir(parents=True)
    (partial / "Ready").write_text("ver")
    check("partial", data, True)

    locked = scratch / "locked"
    (locked / ".prepare-lock").mkdir(parents=True)
    assert subprocess.run([str(executable), str(package), str(locked)]).returncode == 1
    assert not (locked / "test-1/Ready").exists()

    # Alter an extent's data while keeping its manifest and filesystem magic.
    encoded = "RecoveryRamdisk.img".encode("utf-16-be")
    record = data.index(encoded) - 33
    block = struct.unpack_from("<I", data, record + 2)[0]
    damaged = bytearray(data)
    damaged[block * 2048 + 4096] ^= 1
    check("damaged-recovery", damaged)

    # A directory record shorter than its fixed header must be rejected.
    svd = next(
        offset for offset in range(16 * 2048, 80 * 2048, 2048) if data[offset] == 2
    )
    root_block = struct.unpack_from("<I", data, svd + 158)[0]
    malformed = bytearray(data)
    malformed[root_block * 2048] = 1
    check("malformed-directory", malformed)
    print(
        "PASS: NSW extraction, hashes, architecture, paths, malformed ISO/XML, short I/O, failure publication, install queue and trial confirmation"
    )


if __name__ == "__main__":
    main()
