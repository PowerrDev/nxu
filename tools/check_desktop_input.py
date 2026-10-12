#!/usr/bin/env python3
"""Check the production desktop's input delivery and idle waits on the host."""

from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="nxu-desktop-input-") as scratch:
    work = Path(scratch)
    binary = work / "check"
    subprocess.run(
        [
            "clang",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-ffunction-sections",
            "-fdata-sections",
            "-I" + str(ROOT),
            *[
                "-I" + str(ROOT / "tools/DiskRoot/System/Library/Frameworks" / (name + ".framework/Headers"))
                for name in ("CoreGraphics", "DesktopService")
            ],
            "-I" + str(ROOT / "frameworks/include"),
            str(ROOT / "tools/fixtures/gui/desktop_input.c"),
            *(["-Wl,-dead_strip"] if sys.platform == "darwin" else ["-Wl,--gc-sections"]),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
