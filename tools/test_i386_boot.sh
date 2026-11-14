#!/bin/bash
#
# End-to-end boot tests for the i386 kernel: the whole stack (paging, interrupts,
# scheduler, VFS, virtio-blk, ELF32 loader, ring 3, syscalls) under QEMU, booting
# real userland from the ext4 image `make i386-disk` builds. Behind
# `make test-i386-boot`.
#
# Each case boots a private copy of the disk (bootd and the services write to
# it), so nothing is left behind. A clean run ends with qemu-exit, status 1
# (isa-debug-exit, code 0); a fatal phase failure or panic is status 3.
# Plain bash 3.2 (what macOS ships).

set -u

KERNEL=${1:?usage: test_i386_boot.sh <kernel.elf> <disk.img>}
DISK=${2:?usage: test_i386_boot.sh <kernel.elf> <disk.img>}
TIMEOUT=${I386_TEST_TIMEOUT:-60}
HERE=$(dirname "$0")

STATUS_CLEAN=1

WORK=$(mktemp -d "${TMPDIR:-/tmp}/i386boot.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

failures=0
count=0

# run_case <name> <with disk: yes|no> <boot args> <expected status> <expected output>...
# With SERIAL_SCRIPT set (SERIAL_SCRIPT=<file> run_case ...), the host types
# into the console UART as that script says (tools/serial_expect.pl).
run_case() {
  local name=$1 with_disk=$2 append=$3 want=$4 output status pattern ok=1
  shift 4

  local disk_args=()
  local driver=(perl "$HERE/qemu_watchdog.pl" "$TIMEOUT")

  if [ -n "${SERIAL_SCRIPT:-}" ]; then
    driver=(perl "$HERE/serial_expect.pl" "$TIMEOUT" "$SERIAL_SCRIPT")
  fi

  if [ "$with_disk" = yes ]; then
    cp "$DISK" "$WORK/disk.img"
    disk_args=(-drive "if=none,format=raw,file=$WORK/disk.img,id=d0" -device virtio-blk-pci,drive=d0,disable-modern=on)
  fi

  output=$("${driver[@]}" qemu-system-i386 -cpu qemu32,+nx -M pc \
    -kernel "$KERNEL" -m 1024M \
    -display none -serial stdio -monitor none -no-reboot \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    ${disk_args[@]+"${disk_args[@]}"} \
    -append "$append" </dev/null 2>&1)
  status=$?

  count=$((count + 1))

  if [ "$status" -ne "$want" ]; then
    ok=0
    printf 'FAIL  %-16s qemu exited %s, expected %s\n' "$name" "$status" "$want"
  fi

  for pattern in "$@"; do
    if ! printf '%s\n' "$output" | grep -qF -- "$pattern"; then
      ok=0
      printf 'FAIL  %-16s missing output: %s\n' "$name" "$pattern"
    fi
  done

  if [ "$ok" -eq 1 ]; then
    printf 'ok    %-16s\n' "$name"
  else
    failures=$((failures + 1))
    printf '%s\n' "$output" | tail -40 | sed 's/^/      | /'
  fi
}

run_case kernel no "test=kernel qemu-exit=1" $STATUS_CLEAN \
  "i386_init_kernel: ramfs mounted at /, /Volumes/mainOS mountpoint created" \
  "i386_init_kernel_selftest: NXPC, name tables, ramfs and boot-args ok"

run_case kernel-only no "qemu-exit=1" $STATUS_CLEAN \
  "no block device, staying in kernel-only mode" \
  "i386_init: boot phases complete"

run_case bootd yes "run-seconds=10 qemu-exit=1" $STATUS_CLEAN \
  "userland_spawn_bootd: bootd PID 1 ready for scheduler dispatch" \
  "bootd: userspace service manager started as PID 1" \
  "bootd: com.nxu.logd: started" \
  "bootd: com.nxu.patchd: started" \
  "patchd: integrity repair service started" \
  "run time elapsed, leaving the scheduler"

run_case process-thread yes "process-test=thread qemu-exit=1" $STATUS_CLEAN \
  "thread_process_test: real process spawned its own threads and all ran correctly"

run_case process-ipc yes "process-test=ipc qemu-exit=1" $STATUS_CLEAN \
  "ipctest_b: received \"pong\", exiting" \
  "ipc_process_test: two-process bootstrap register/lookup round trip passed"

run_case process-socket yes "process-test=socket qemu-exit=1" $STATUS_CLEAN \
  "socket_process_test: two-process streaming socket round trip passed"

run_case process-signal yes "process-test=signal qemu-exit=1" $STATUS_CLEAN \
  "sigtest: handlers, masks, stop, continue and kill all worked" \
  "signal_process_test: signal handlers, stop, continue and kill work between spawned processes"

run_case process-spawn yes "process-test=spawn qemu-exit=1" $STATUS_CLEAN \
  "spawntest: arguments, descriptors, groups, sessions and masks were all in place at the start" \
  "spawn_process_test: a spawned process starts with its arguments, descriptors, group and mask in place"

run_case tty yes "process-test=tty qemu-exit=1" $STATUS_CLEAN \
  "tty_init: initialized tty successfully" \
  "userland_run_process_test: tty process test passed"

SERIAL_SCRIPT="$HERE/serial_test.script" run_case serial yes "process-test=serial qemu-exit=1" $STATUS_CLEAN \
  "serial_init: /dev/console on the console UART, interrupt driven" \
  "serial_test_run: line is interrupt driven" \
  "serial_test_run: 6000 bytes, input held back in the UART" \
  "userland_run_process_test: serial process test passed"

# A normal boot, logged into on the serial console (getty and login); the script ends it, status 0.
SERIAL_SCRIPT="$HERE/console_login.script" run_case console-login yes "" 0 \
  "bootd: com.nxu.getty: started" \
  "Login incorrect" \
  "uid 1000  gid 4" \
  "uid 0  gid 4" \
  "serial_expect.pl: script complete"

run_case pty yes "process-test=pty qemu-exit=1" $STATUS_CLEAN \
  "pty_init: initialized pty successfully" \
  "ptytest: a shell on a pseudo-terminal edits, runs, interrupts, stops and continues jobs" \
  "pty_process_test: a shell on a pseudo-terminal edits, runs, interrupts, stops and continues jobs" \
  "userland_run_process_test: pty process test passed"

run_case termios yes "process-test=termios qemu-exit=1" $STATUS_CLEAN \
  "ttytest: termios calls, terminal requests and stty all behaved" \
  "termios_process_test: programs configure their terminal with termios and stty" \
  "userland_run_process_test: termios process test passed"

run_case restart yes "process-test=restart qemu-exit=1" $STATUS_CLEAN \
  "restarttest: interrupted system calls were restarted, or not, as they should be" \
  "restart_process_test: interrupted system calls are restarted, or not, as SA_RESTART and the call say" \
  "userland_run_process_test: restart process test passed"

printf '\n%d case(s), %d failure(s)\n' "$count" "$failures"

[ "$failures" -eq 0 ]
