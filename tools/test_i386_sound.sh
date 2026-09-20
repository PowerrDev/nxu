#!/bin/bash
#
# The i386 sound suite, behind `make test-i386-sound`: the VirtIO Sound driver
# over VirtIO-PCI on the i386 kernel, against QEMU's virtio-sound-pci device
# recording with the wav audio backend.
#
#   sound-irq     sound-test=1 with the device's interrupt bound to its PIC line:
#                 the kernel plays a tone, the boot chime and playsound (a user
#                 process) and the host checks the recording sample by sample
#                 (tools/audio/verify_capture.py); interrupts must have arrived
#   sound-poll    the same with no interrupt bound: the writer polls the rings
#   no-device     no sound device on the machine: the boot logs it and goes on
#   no-device-test  sound-test=1 with no sound device fails cleanly (no hang)
#
# Each case boots a private copy of the disk. A clean run ends with qemu-exit,
# status 1 (isa-debug-exit, code 0); a failed phase is status 3. Plain bash 3.2.

set -u

KERNEL=${1:?usage: test_i386_sound.sh <kernel.elf> <disk.img>}
DISK=${2:?usage: test_i386_sound.sh <kernel.elf> <disk.img>}
TIMEOUT=${I386_SOUND_TEST_TIMEOUT:-120}
HERE=$(dirname "$0")
CHIME=$HERE/DiskRoot/System/Library/Resources/Audio/Boot_Audio.wav

STATUS_CLEAN=1
STATUS_FAILED=3

WORK=$(mktemp -d "${TMPDIR:-/tmp}/i386sound.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

command -v python3 > /dev/null 2>&1 || { echo "test_i386_sound: python3 not found"; exit 2; }

failures=0
count=0

# run_case <name> <sound device: yes|no> <boot args> <expected status> <expected output>...
# Leaves the serial output in $OUTPUT and the recording in $WORK/<name>.wav.
run_case() {
	local name=$1 with_sound=$2 append=$3 want=$4 status pattern ok=1
	shift 4

	local sound_args=()

	if [ "$with_sound" = yes ]; then
		sound_args=(-audiodev wav,id=snd0,path="$WORK/$name.wav" -device virtio-sound-pci,audiodev=snd0)
	fi

	cp "$DISK" "$WORK/$name.img"

	OUTPUT=$(perl "$HERE/qemu_watchdog.pl" "$TIMEOUT" qemu-system-i386 -M pc \
		-kernel "$KERNEL" -m 512M \
		-display none -serial stdio -monitor none -no-reboot \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
		-drive if=none,format=raw,file="$WORK/$name.img",id=d0 \
		-device virtio-blk-pci,drive=d0,disable-modern=on \
		${sound_args[@]+"${sound_args[@]}"} \
		-append "$append" < /dev/null 2>&1)
	status=$?

	count=$((count + 1))

	if [ "$status" -ne "$want" ]; then
		ok=0
		printf 'FAIL  %-16s qemu exited %s, expected %s\n' "$name" "$status" "$want"
	fi

	for pattern in "$@"; do
		if ! printf '%s\n' "$OUTPUT" | grep -qF -- "$pattern"; then
			ok=0
			printf 'FAIL  %-16s missing output: %s\n' "$name" "$pattern"
		fi
	done

	if [ "$ok" -eq 1 ]; then
		printf 'ok    %-16s\n' "$name"
	else
		failures=$((failures + 1))
		printf '%s\n' "$OUTPUT" | tail -40 | sed 's/^/      | /'
	fi
}

# verify <name>: the recording of the case just run.
verify() {
	local name=$1

	count=$((count + 1))

	if python3 "$HERE/audio/verify_capture.py" "$WORK/$name.wav" "$CHIME" --tone --chimes 2 > "$WORK/$name.verify" 2>&1; then
		printf 'ok    %-16s recording verified: %s\n' "$name" "$(tail -1 "$WORK/$name.verify")"
	else
		failures=$((failures + 1))
		printf 'FAIL  %-16s the recording did not verify\n' "$name"
		sed 's/^/      | /' "$WORK/$name.verify"
	fi
}

# interrupts <name> <min>: the close summary counts the interrupts taken.
interrupts() {
	local name=$1 minimum=$2 taken

	count=$((count + 1))
	taken=$(printf '%s\n' "$OUTPUT" | sed -n 's/.*stream 0: \([0-9][0-9]*\) interrupt(s) taken since attach.*/\1/p' | tail -1)

	if [ -n "$taken" ] && [ "$taken" -ge "$minimum" ]; then
		printf 'ok    %-16s %s interrupt(s) taken\n' "$name" "$taken"
	else
		failures=$((failures + 1))
		printf 'FAIL  %-16s %s interrupt(s) taken, expected at least %s\n' "$name" "${taken:-no count}" "$minimum"
	fi
}

PASS_LINES=(
	"virtio_snd_attach: VirtIO Sound device found"
	"virtio_snd_dev_register: registered /dev/audio0"
	"sound_test_thread: the tone played"
	"sound_test_thread: the boot chime played"
	"sound_test_run_playsound: without NXU_CAP_AUDIO: playsound exited with status 10 as expected"
	"sound_test_run_playsound: with NXU_CAP_AUDIO: playsound exited with status 0 as expected"
	"i386_init_userland: sound-test passed"
)

run_case sound-irq yes "sound-test=1 qemu-exit=1 virtio-irq=1" $STATUS_CLEAN "${PASS_LINES[@]}" \
	"virtio_snd_bring_up: interrupt handler attached: completions arrive by interrupt"
verify sound-irq
interrupts sound-irq 100

run_case sound-poll yes "sound-test=1 qemu-exit=1" $STATUS_CLEAN "${PASS_LINES[@]}" \
	"virtio_snd_bring_up: interrupt delivery is off on this transport: completions are polled"
verify sound-poll

run_case no-device no "run-seconds=4 qemu-exit=1" $STATUS_CLEAN \
	"virtio_snd_probe: no VirtIO Sound device" \
	"i386_init: boot phases complete"

run_case no-device-test no "sound-test=1 qemu-exit=1" $STATUS_FAILED \
	"virtio_snd_probe: no VirtIO Sound device" \
	"i386_init_userland: sound-test FAILED"

echo
printf '%s case(s), %s failure(s)\n' "$count" "$failures"

[ "$failures" -eq 0 ]
