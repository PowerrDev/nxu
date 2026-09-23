#!/usr/bin/env python3
#
# tools/console_switch.py -- one terminal for NXU's and tepOS's consoles.
#
# tools/with_tepos.sh starts it when run from a terminal. Both QEMUs expose
# their console (serial0) as a listening Unix socket; this connects to both,
# logs everything each machine prints, and shows one of them:
#
#   s        show NXU's console
#   t        show tepOS's console
#   Ctrl-C   quit (stops both machines)
#
# Switching clears the screen and replays the recent part of that console.
# Any other key goes to the machine being shown. When NXU's QEMU exits, so
# does this.

import argparse
import os
import select
import shutil
import socket
import sys
import termios
import time
import tty

HISTORY_BYTES = 256 * 1024
NAMES = {"nxu": "NXU", "tepos": "tepOS"}


def connect(path, timeout):
    deadline = time.monotonic() + timeout
    while True:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            s.connect(path)
            return s
        except OSError:
            s.close()
            if time.monotonic() > deadline:
                raise SystemExit("console_switch: no console socket at %s" % path)
            time.sleep(0.1)


class Console:
    def __init__(self, name, path, log_path, timeout):
        self.name = name
        self.sock = connect(path, timeout)
        self.log = open(log_path, "wb")
        self.history = bytearray()
        self.open = True

    def feed(self, data):
        self.log.write(data)
        self.log.flush()
        self.history += data
        if len(self.history) > HISTORY_BYTES:
            del self.history[:len(self.history) - HISTORY_BYTES]


def out(data):
    os.write(sys.stdout.fileno(), data)


def show(console, closed_note=False):
    """Clear the screen, print the banner and the tail that fits."""
    cols, rows = shutil.get_terminal_size((80, 24))
    other = "t: tepOS" if console.name == "nxu" else "s: NXU"
    banner = " %s console  |  %s  |  Ctrl-C: quit " % (NAMES[console.name], other)
    out(b"\x1b[2J\x1b[H\x1b[7m" + banner[:cols].ljust(cols).encode() + b"\x1b[0m\r\n")
    lines = bytes(console.history).split(b"\n")
    for line in lines[-(rows - 2):-1] if len(lines) > 1 else []:
        out(line.rstrip(b"\r") + b"\r\n")
    if lines and lines[-1]:
        out(lines[-1])
    if not console.open or closed_note:
        out(b"\r\n[%s console closed]\r\n" % NAMES[console.name].encode())


def main():
    ap = argparse.ArgumentParser(description="NXU/tepOS console switcher")
    ap.add_argument("--nxu", required=True, help="NXU console socket")
    ap.add_argument("--tepos", required=True, help="tepOS console socket")
    ap.add_argument("--nxu-log", required=True)
    ap.add_argument("--tepos-log", required=True)
    ap.add_argument("--timeout", type=float, default=30.0)
    args = ap.parse_args()

    consoles = {
        "nxu": Console("nxu", args.nxu, args.nxu_log, args.timeout),
        "tepos": Console("tepos", args.tepos, args.tepos_log, args.timeout),
    }
    active = "nxu"
    stdin = sys.stdin.fileno()
    saved = termios.tcgetattr(stdin)
    tty.setcbreak(stdin)
    try:
        show(consoles[active])
        while True:
            watched = [stdin] + [c.sock for c in consoles.values() if c.open]
            readable, _, _ = select.select(watched, [], [])
            for fd in readable:
                if fd == stdin:
                    key = os.read(stdin, 64)
                    for byte in key:
                        ch = bytes([byte])
                        if ch in (b"s", b"t"):
                            wanted = "nxu" if ch == b"s" else "tepos"
                            if wanted != active:
                                active = wanted
                                show(consoles[active])
                        elif consoles[active].open:
                            consoles[active].sock.sendall(ch)
                    continue
                console = next(c for c in consoles.values() if c.sock is fd)
                try:
                    data = console.sock.recv(4096)
                except OSError:
                    data = b""
                if not data:
                    console.open = False
                    if console.name == "nxu":
                        return 0
                    if active == console.name:
                        out(b"\r\n[tepOS console closed]\r\n")
                    continue
                console.feed(data)
                if console.name == active:
                    out(data)
    except KeyboardInterrupt:
        return 130
    finally:
        termios.tcsetattr(stdin, termios.TCSADRAIN, saved)
        out(b"\r\n")


if __name__ == "__main__":
    sys.exit(main())
