#!/usr/bin/env python3
"""Check retained recovery-terminal output and scroll bounds."""

from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix="nxu-recovery-terminal-") as scratch:
    binary = Path(scratch) / "check"
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
            "-I" + str(ROOT / "frameworks/include"),
            "-I" + str(ROOT / "frameworks/Recovery.framework/include"),
            str(ROOT / "tools/fixtures/gui/recovery_terminal.c"),
            str(ROOT / "frameworks/Recovery.framework/triageOS/ui.c"),
            *(["-Wl,-dead_strip"] if sys.platform == "darwin" else ["-Wl,--gc-sections"]),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
