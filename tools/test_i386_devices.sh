#!/bin/bash
#
# Boots the i386 kernel under QEMU with the legacy PC devices and checks the
# platform and drivers self-tests (PCI enumeration, RTC, virtio-blk over
# legacy VirtIO-PCI, virtio keyboard and mouse, virtio-gpu display). Behind
# `make test-i386-devices`.
#
# Exit status is the isa-debug-exit device's: a clean shutdown (qemu-exit=1)
# exits 1, a failed phase exits 3. A watchdog kills QEMU that never exits
# (exit 124). All disks are scratch files; nothing tracked is touched. The
# checks match this area's own self-test lines only, never other areas' logs.
# Plain bash 3.2 (what macOS ships).

set -u

KERNEL=${1:?usage: test_i386_devices.sh <kernel.elf>}
TIMEOUT=${I386_TEST_TIMEOUT:-30}

STATUS_CLEAN=1
STATUS_FAILED=3

WORK=$(mktemp -d "${TMPDIR:-/tmp}/nxu-i386-devices.XXXXXX") || exit 2
trap 'rm -rf "$WORK"' EXIT

failures=0
count=0

# The QEMU devices under test. virtio-blk has a transitional (legacy, I/O BAR)
# interface; virtio-input and virtio-gpu are modern-only, so those devices are
# reached through the capability transport in memory BARs. -vga none removes
# the PC machine's default legacy VGA card so the screendump-based checks
# below are unambiguously reading virtio-gpu's scanout.
DISPLAY_XRES=640
DISPLAY_YRES=480

DEVICE_FLAGS=(
	-vga none
	-device virtio-blk-pci,drive=d0,disable-modern=on
	-device virtio-keyboard-pci,disable-modern=on
	-device virtio-mouse-pci,disable-modern=on
	-device virtio-gpu-pci,xres=$DISPLAY_XRES,yres=$DISPLAY_YRES
)

SECTORS=16384
MARKER="NXU-DEV0"

# make_disk <path>: an 8 MiB scratch image whose sector 0 starts with MARKER.
make_disk() {
	dd if=/dev/zero of="$1" bs=512 count=$SECTORS 2>/dev/null
	printf '%s' "$MARKER" | dd of="$1" bs=1 conv=notrunc 2>/dev/null
}

# watchdog <seconds> <command...>: run, kill hard on timeout (QEMU does not
# die from a plain alarm signal), exit 124 then.
watchdog() {
	local seconds=$1
	shift
	perl -e '
		my $t = shift @ARGV;
		my $pid = fork();
		die "fork: $!" unless defined $pid;
		if ($pid == 0) { exec @ARGV or die "exec: $!"; }
		$SIG{ALRM} = sub { kill 9, $pid; waitpid($pid, 0); exit 124; };
		alarm $t;
		waitpid($pid, 0);
		exit($? >> 8);
	' "$seconds" "$@"
}

# check <name> <status> <want status> <output> <pattern>...
verify() {
	local name=$1 status=$2 want=$3 output=$4 ok=1 pattern
	shift 4

	count=$((count + 1))

	if [ "$status" -ne "$want" ]; then
		ok=0
		printf 'FAIL  %-14s qemu exited %s, expected %s\n' "$name" "$status" "$want"
	fi

	for pattern in "$@"; do
		if ! printf '%s\n' "$output" | grep -qF -- "$pattern"; then
			ok=0
			printf 'FAIL  %-14s missing output: %s\n' "$name" "$pattern"
		fi
	done

	if [ "$ok" -eq 1 ]; then
		printf 'ok    %-14s\n' "$name"
	else
		failures=$((failures + 1))
		printf '%s\n' "$output" | sed 's/^/      | /'
	fi
}

# boot <disk|-> <boot args> -> $OUTPUT, $STATUS. With "-" no devices are attached.
boot() {
	local disk=$1 append=$2

	if [ "$disk" = "-" ]; then
		OUTPUT=$(watchdog "$TIMEOUT" qemu-system-i386 -M pc \
			-kernel "$KERNEL" -m 512M \
			-display none -serial stdio -monitor none -no-reboot \
			-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
			-append "$append" 2>&1 < /dev/null)
	else
		OUTPUT=$(watchdog "$TIMEOUT" qemu-system-i386 -M pc \
			-kernel "$KERNEL" -m 512M \
			-display none -serial stdio -monitor none -no-reboot \
			-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
			-drive if=none,format=raw,file="$disk",id=d0 \
			"${DEVICE_FLAGS[@]}" \
			-append "$append" 2>&1 < /dev/null)
	fi
	STATUS=$?
}

# sector_hash <disk> <first sector> <count>
sector_hash() {
	dd if="$1" bs=512 skip="$2" count="$3" 2>/dev/null | shasum -a 256 | cut -d' ' -f1
}

# Expected content of the 16 scratch sectors the drivers self-test writes:
# byte i = (i*131 + (i/512)*17 + 0xA5) & 0xFF.
expected_pattern() {
	perl -e 'binmode STDOUT; for my $i (0 .. 16*512-1) { print chr(($i*131 + int($i/512)*17 + 0xA5) & 0xFF); }' > "$1"
}

SCRATCH_FIRST=$((SECTORS - 16))

# ---- platform: PCI enumeration and the RTC -----------------------------------

DISK=$WORK/platform.img
make_disk "$DISK"

boot "$DISK" "test=platform qemu-exit=1 expect-virtio=4 expect-time=$(date -u +%s)"
verify platform "$STATUS" $STATUS_CLEAN "$OUTPUT" \
	"i386_init_platform_selftest: 4 VirtIO function(s)" \
	"i386_init_platform_selftest: PCI enumeration and RTC ok" \
	"platform self-test passed"

# ---- drivers: block round trip, split across several requests ----------------

DISK=$WORK/drivers.img
make_disk "$DISK"
before_marker=$(sector_hash "$DISK" 0 1)
before_scratch=$(sector_hash "$DISK" $SCRATCH_FIRST 16)

boot "$DISK" "test=drivers qemu-exit=1 expect-input=2 expect-display=1"
verify drivers "$STATUS" $STATUS_CLEAN "$OUTPUT" \
	"VirtIOBlockFamily: matched virtio-blk" \
	"block device disk0, 16384 sectors of 512 bytes, read-write, flush supported" \
	"sector 0 begins 4E 58 55 2D 44 45 56 30" \
	"wrote, flushed and read back 16 sectors at $SCRATCH_FIRST" \
	"2 input device(s), keyboard present, mouse present" \
	"VirtIOGPUFamily: scanout 0 online at ${DISPLAY_XRES}x${DISPLAY_YRES}" \
	"painting test pattern, ${DISPLAY_XRES}x${DISPLAY_YRES}" \
	"test pattern presented" \
	"block round trip, input and display ok" \
	"drivers self-test passed"

# The self-test restores what it overwrote: the image must be as it was.
count=$((count + 1))
if [ "$(sector_hash "$DISK" 0 1)" = "$before_marker" ] && [ "$(sector_hash "$DISK" $SCRATCH_FIRST 16)" = "$before_scratch" ]; then
	printf 'ok    %-14s\n' "restored"
else
	failures=$((failures + 1))
	printf 'FAIL  %-14s scratch sectors were not restored\n' "restored"
fi

# ---- drivers: the write really reaches the backing file ----------------------

DISK=$WORK/keep.img
make_disk "$DISK"
expected_pattern "$WORK/pattern.bin"

boot "$DISK" "test=drivers qemu-exit=1 block-keep=1"
verify write-persist "$STATUS" $STATUS_CLEAN "$OUTPUT" \
	"block-keep set, scratch sectors left as written" \
	"drivers self-test passed"

count=$((count + 1))
dd if="$DISK" bs=512 skip=$SCRATCH_FIRST count=16 2>/dev/null > "$WORK/ondisk.bin"
if cmp -s "$WORK/pattern.bin" "$WORK/ondisk.bin"; then
	printf 'ok    %-14s\n' "host-verify"
else
	failures=$((failures + 1))
	printf 'FAIL  %-14s pattern on the host image differs from what the guest wrote\n' "host-verify"
fi

# ---- drivers: polling versus the irq_register path ---------------------------

DISK=$WORK/irq.img
make_disk "$DISK"

boot "$DISK" "test=drivers qemu-exit=1 expect-input=2 expect-display=1 virtio-irq=1"
verify virtio-irq "$STATUS" $STATUS_CLEAN "$OUTPUT" \
	"VirtIO interrupts enabled" \
	"block round trip, input and display ok"

# ---- negative: a missing block device fails the phase ------------------------

boot - "test=drivers qemu-exit=1"
verify no-block "$STATUS" $STATUS_FAILED "$OUTPUT" \
	"i386_init_drivers_selftest: FAIL 0 block device(s), expected 1"

boot - "test=drivers qemu-exit=1 expect-block=0"
verify no-devices "$STATUS" $STATUS_CLEAN "$OUTPUT" \
	"0 input device(s)" \
	"drivers self-test passed"

# ---- input: events injected through the QEMU monitor reach the drivers -------

monitor_send() {
	# The monitor answers on the same socket; give it a moment before closing.
	(printf '%s\n' "$1"; sleep 0.5) | nc -U "$MONITOR" > /dev/null 2>&1
}

# input_case <name> <extra boot args>
input_case() {
	local name=$1 extra=$2 waited=0 attempt

	DISK=$WORK/$name.img
	make_disk "$DISK"
	SERIAL=$WORK/$name.serial
	MONITOR=$WORK/$name.sock
	: > "$SERIAL"

	watchdog "$TIMEOUT" qemu-system-i386 -M pc \
		-kernel "$KERNEL" -m 512M \
		-display none -serial file:"$SERIAL" -monitor unix:"$MONITOR",server,nowait -no-reboot \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
		-drive if=none,format=raw,file="$DISK",id=d0 \
		"${DEVICE_FLAGS[@]}" \
		-append "test=drivers qemu-exit=1 expect-input=2 input-wait=20 $extra" < /dev/null > /dev/null 2>&1 &
	QEMU_PID=$!

	# Wait for the guest to say it is waiting, then inject a key press and mouse motion.
	while ! grep -qF "waiting for input events" "$SERIAL" 2> /dev/null; do
		sleep 0.2
		waited=$((waited + 1))
		if [ "$waited" -gt 100 ] || ! kill -0 $QEMU_PID 2> /dev/null; then break; fi
	done

	# Keep injecting until the guest reports (it needs both classes of event).
	for attempt in 1 2 3 4 5 6 7 8; do
		if grep -qF "received" "$SERIAL" 2> /dev/null; then break; fi
		monitor_send "sendkey a"
		monitor_send "mouse_move 5 5"
		sleep 0.3
	done

	wait $QEMU_PID
	STATUS=$?
	OUTPUT=$(cat "$SERIAL")
}

input_case input-poll ""
verify input-poll "$STATUS" $STATUS_CLEAN "$OUTPUT" \
	"waiting for input events" \
	"keyboard and" \
	"mouse event(s)" \
	"drivers self-test passed"

# With virtio-irq=1 the events arrive by interrupt when the interrupts area is
# linked in (irq_register chains, the PIC line is unmasked), and by polling
# when it is not; either way the events must arrive.
input_case input-irq "virtio-irq=1"
verify input-irq "$STATUS" $STATUS_CLEAN "$OUTPUT" \
	"VirtIO interrupts enabled" \
	"mouse event(s)" \
	"drivers self-test passed"

if printf '%s\n' "$OUTPUT" | grep -qF "input delivered by interrupt"; then
	echo "      (input-irq: events were delivered by PIC interrupt)"
else
	echo "      (input-irq: no interrupt path linked, events were polled)"
fi

# ---- display: virtio-gpu's scanout reaches the host, not just the driver -----
#
# Everything above only checks what the guest's own log says. This boots
# without qemu-exit=1 (so QEMU stays up once the test pattern is painted and
# presented), takes a monitor screendump once the guest says it presented the
# pattern, and checks the actual host-visible pixels: proof the VirtIO-GPU
# command stream (RESOURCE_CREATE_2D, ATTACH_BACKING, SET_SCANOUT, TRANSFER,
# FLUSH) really reached QEMU's scanout, not just that the driver's own
# round trips said "ok".

display_case() {
	DISK=$WORK/display.img
	make_disk "$DISK"
	SERIAL=$WORK/display.serial
	MONITOR=$WORK/display.sock
	DUMP=$WORK/display.ppm
	: > "$SERIAL"

	qemu-system-i386 -M pc \
		-kernel "$KERNEL" -m 512M \
		-display none -serial file:"$SERIAL" -monitor unix:"$MONITOR",server,nowait -no-reboot \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
		-drive if=none,format=raw,file="$DISK",id=d0 \
		"${DEVICE_FLAGS[@]}" \
		-append "test=drivers expect-display=1" < /dev/null > /dev/null 2>&1 &
	QEMU_PID=$!

	local waited=0
	while ! grep -qF "test pattern presented" "$SERIAL" 2>/dev/null; do
		sleep 0.2
		waited=$((waited + 1))
		if [ "$waited" -gt 100 ] || ! kill -0 $QEMU_PID 2>/dev/null; then break; fi
	done

	sleep 0.3
	(printf 'screendump %s\n' "$DUMP"; sleep 0.5) | nc -U "$MONITOR" > /dev/null 2>&1
	sleep 0.3

	kill $QEMU_PID 2>/dev/null
	wait $QEMU_PID 2>/dev/null
	DISPLAY_SERIAL=$(cat "$SERIAL")
}

display_case
count=$((count + 1))

if ! printf '%s\n' "$DISPLAY_SERIAL" | grep -qF "test pattern presented"; then
	failures=$((failures + 1))
	printf 'FAIL  %-14s guest never reported presenting the pattern\n' "gpu-scanout"
	printf '%s\n' "$DISPLAY_SERIAL" | sed 's/^/      | /'
elif [ ! -s "$DUMP" ]; then
	failures=$((failures + 1))
	printf 'FAIL  %-14s screendump produced no file\n' "gpu-scanout"
else
	RESULT=$(python3 - "$DUMP" "$DISPLAY_XRES" "$DISPLAY_YRES" << 'PYEOF'
import sys

path, xres, yres = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])

with open(path, "rb") as f:
    data = f.read()

# Minimal binary PPM (P6) header parser: magic, three whitespace-separated
# integers (width, height, maxval), one whitespace byte, then raw pixels.
assert data[:2] == b"P6"
pos = 2
values = []
while len(values) < 3:
    while data[pos:pos + 1] in b" \t\r\n":
        pos += 1
    if data[pos:pos + 1] == b"#":
        while data[pos:pos + 1] not in b"\r\n":
            pos += 1
        continue
    start = pos
    while data[pos:pos + 1] not in b" \t\r\n":
        pos += 1
    values.append(int(data[start:pos]))
width, height, _maxval = values
pos += 1


def pixel(x, y):
    offset = pos + (y * width + x) * 3
    return data[offset], data[offset + 1], data[offset + 2]


def close(got, want, tolerance=24):
    return all(abs(g - w) <= tolerance for g, w in zip(got, want))


if width < xres or height < yres:
    print("FAIL size %dx%d smaller than expected %dx%d" % (width, height, xres, yres))
    sys.exit(1)

quarter_x, quarter_y = xres // 4, yres // 4
checks = [
    ("top-left/red", quarter_x, quarter_y, (255, 0, 0)),
    ("top-right/green", xres - quarter_x, quarter_y, (0, 255, 0)),
    ("bottom-left/blue", quarter_x, yres - quarter_y, (0, 0, 255)),
    ("bottom-right/white", xres - quarter_x, yres - quarter_y, (255, 255, 255)),
]

failed = [name for name, x, y, want in checks if not close(pixel(x, y), want)]
if failed:
    print("FAIL quadrant(s) wrong: %s" % ", ".join(failed))
    sys.exit(1)

print("ok")
PYEOF
	)

	if [ "$RESULT" = "ok" ]; then
		printf 'ok    %-14s\n' "gpu-scanout"
	else
		failures=$((failures + 1))
		printf 'FAIL  %-14s %s\n' "gpu-scanout" "$RESULT"
	fi
fi

echo
printf '%s case(s), %s failure(s)\n' "$count" "$failures"

[ "$failures" -eq 0 ]
