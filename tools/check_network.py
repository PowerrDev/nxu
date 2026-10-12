#!/usr/bin/env python3
"""Run the real networking protocols against a deterministic peer with sanitizers."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
SOURCES = [
    *sorted((ROOT / "kern/net").glob("*.c")),
    *sorted((ROOT / "kern/netinet").glob("*.c")),
    *sorted((ROOT / "kern/netinet6").glob("*.c")),
    ROOT / "frameworks/Terminal.framework/netutil.c",
    ROOT / "frameworks/Terminal.framework/shell_parse.c",
    ROOT / "frameworks/lib/string.c",
    ROOT / "tools/fixtures/net/protocol_test.c",
]

with tempfile.TemporaryDirectory(prefix="nxu-network-") as scratch:
    work = Path(scratch)
    for header in (
        "kern/lock.h",
        "kern/console/console.h",
        "kern/machine/timer.h",
        "kern/process/proc.h",
        "kern/process/thread.h",
        "kern/sched_prism/sched.h",
    ):
        target = work / header
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text("#include <tools/fixtures/net/host.h>\n")

    binary = work / "protocol-test"
    subprocess.run(
        [
            "clang",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-g",
            "-fsanitize=address,undefined",
            "-I" + str(work),
            "-I" + str(ROOT),
            "-I" + str(ROOT / "frameworks/include"),
            *map(str, SOURCES),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
