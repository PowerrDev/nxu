# =============================================================================
# Kernel tests
# =============================================================================
#
#   make run                the normal graphical boot on 4 CPUs, gated on the
#                           kernel's own startup self-tests (make test TEST=desktop)
#   make tests              interactive picker (tools/test_menu.sh)
#   make test TEST=<id>     build and boot a single test directly
#   make test-list          machine-readable registry (id, group, description)
#   make check              build and boot every headless test, then the i386
#                           suites, and print one pass/fail table (tools/check.sh)
#   make check-list         the kernel tests `make check` runs (id, timeout, pass line,
#                           defines, capture, verify, frameworks)
#   make check-i386-list    the i386 suite targets `make check` runs
#
# Every test is a kernel CONFIG of its own (BUILD/<id>/kernel.bin) that is
# compiled with the test's -D switch and booted under QEMU against the
# persistent disk image. To add a test, append its id to TEST_IDS and define
# the TEST_<id>_* variables in makedefs/tests/<id>.mk; nothing else needs to change.
#
#   TEST_<id>_GROUP       menu section
#   TEST_<id>_DESC        one line for the menu (plain text: no quotes, $ or #)
#   TEST_<id>_CFLAGS      kernel defines that select the test
#   TEST_<id>_FRAMEWORKS  sibling framework switches (UISERVICE=1, WINDOWSERVER=1)
#   TEST_<id>_DISPLAY     QEMU -display argument      (default: none)
#   TEST_<id>_GPU         QEMU virtio-gpu-device args (default: bare device)
#   TEST_<id>_RAMFB       QEMU ramfb device           (default: none)
#   TEST_<id>_MONITOR     QEMU -monitor argument      (default: none)
#   TEST_<id>_SMP         virtual CPUs QEMU gives it (-smp; default: 1)
#   TEST_<id>_PASS        serial line that means the test passed; setting it
#                         puts the test in `make check` (headless tests only).
#                         Several lines, all required, are separated by |
#   TEST_<id>_TIMEOUT     seconds `make check` waits for it (default: CHECK_TIMEOUT)
#   TEST_<id>_AUDIODEV    QEMU -audiodev arguments for the sound device (default:
#                         QEMU_AUDIODEV, the host's speakers)
#   TEST_<id>_CAPTURE     set to 1 for a test that records what it plays: `make
#                         check` gives it QEMU's wav audiodev and, once the pass
#                         line has shown, runs TEST_<id>_VERIFY on the recording
#   TEST_<id>_VERIFY      command that checks the recording; @CAPTURE@ stands for
#                         its path. It must exit 0 (make check only; no shell
#                         quotes, tabs or # in it)
#   TEST_<id>_SERIAL1     QEMU arguments for a second serial port (default: none)
#   TEST_<id>_TEP         set to 1 to boot tepOS alongside, joined by the mailbox
#                         (tools/with_tepos.sh; TEP=0 turns it off, TEP=1 on)

TEST_GUI_STACK := UISERVICE=1 WINDOWSERVER=1

# The rule below adds the `-display` flag itself, so hand it just the backend.
TEST_GUI_DISPLAY := $(QEMU_DISPLAY_BACKEND)

TEST_IDS := \
    everything \
    desktop \
    ui-about \
    ui-voyager \
    windowserver-about \
    journal-crash \
    ipc-process \
    thread-process \
    fault-process \
    process-control \
    socket-process \
    network \
    windowserver-process \
    about-sevos-process \
    smp \
    smp-user \
    sound \
    tep-mailbox



# -- Everything at once --------------------------------------------------------

# The `make check` regression row for the normal boot (see `desktop` below,
# what `make run` actually boots): one kernel, one QEMU, everything that can
# share a boot running in it (kern/tests/unified_boot.h). The boot is a normal
# one -- bootd as PID 1 with logd and patchd, the boot chime -- with the
# UIService session (WindowServer + the Voyager app) on the boot thread and a
# kernel thread that runs ipc-process, thread-process, process-control,
# socket-process and sound against the live system, then prints one
# unified_boot_summary block.
#
# Left out, and why (the code decides, not the names):
#   journal-crash        halts the kernel after crashing mid-transaction on purpose
#   fault-process        exists to fault user processes on purpose
#   ui-about             the same session as ui-voyager with a different app;
#                        one session, one app: Voyager is the richer one
#   windowserver-about   ui-about again (both bootstrap WindowServer, then run About)
#   windowserver-process a second display server: the userland WindowServer
#                        claims the one display the UI session already owns
#   about-sevos-process  needs that userland WindowServer to draw into
#
# `make check` runs it headless: the same kernel with -display none (the UI
# session still runs, on the virtio-gpu framebuffer) and every pass line below
# must show. It does not check the audio recording: the boot chime plays with
# underruns here while the UI loop and the tests share the CPU (known, see
# doc/testing.md), so the recording is checked by the sound row instead.
# Four CPUs: the secondaries are up and idle while the whole system runs on the boot CPU.


# -- Normal desktop boot: `make run` --------------------------------------------

# What `make run` boots. Exactly `everything`'s boot -- bootd as PID 1 with logd
# and patchd, the boot chime, the UIService session (WindowServer + Voyager) on
# the boot thread, on 4 CPUs so user processes actually spread across them --
# minus the kernel thread that runs the ipc/thread/process-control/socket/sound
# tests against the live system. Those are regression tests, not a boot-health
# check: they add their own CPU load, log noise and (before sound_test_run_shared
# was fixed to match its own doc comment) a second boot chime playback, none of
# which belongs in an everyday interactive boot. What "make sure the kernel is
# in a good state to boot" actually needs is already unconditional on every
# boot, `desktop` included: kernel_do_post()'s self-test suite (kern/kern_init.c),
# which halts the boot (kern_fail) on the first failure, before bootd even starts.
# Keep normal desktop operation free of per-syscall traces and diagnostic dumps.
# BOOT_ARGS appears first, so explicit debug settings still override these defaults.


# -- Graphical boot ------------------------------------------------------------





# -- Filesystem ----------------------------------------------------------------

# Crashes mid-transaction on purpose; boot again with `make run` to watch the
# ext4 journal replay.


# -- Userland processes --------------------------------------------------------










# -- Sound ---------------------------------------------------------------------

# The kernel plays a test tone, the boot chime (through the boot code) and then
# playsound (a user process) into QEMU's audio backend. `make test TEST=sound`
# plays it through the host's speakers; `make check` records it with the wav
# backend and verify_capture.py compares the recording with the tone formula and
# with Boot_Audio.wav sample by sample.




# -- Trusted Enclave -----------------------------------------------------------

# The mailbox to tepOS (drivers/tep) is a serial link to a second QEMU. NXU's
# serial1 listens on a Unix socket, tepOS's serial1 listens on another, and
# the TrustedEnclaveProcessor repository's tools/mailbox_link.py joins them
# like a cable, relinking when either machine restarts. No QEMU connects out:
# QEMU 11.1 aborts a reconnecting socket client whenever a connection attempt
# fails.
#
# tools/with_tepos.sh does all of that around NXU's QEMU: it builds and boots
# tepOS headless (console in BUILD/tepos-run/console.log), starts the relay,
# adds the mailbox serial port and stops tepOS when NXU exits. `make run`
# uses it (TEST_desktop_TEP); TEP=0 boots NXU alone, TEP=1 adds tepOS to any
# `make test` or to `make run-console`. Without the tepOS checkout (TEP_DIR,
# default next to this repository) NXU boots alone and tepOS requests fail
# closed.
#
# tools/test_tep_mailbox.sh (make test-tep-mailbox) runs the tep-mailbox test
# with both machines and the relay, stopping and restarting tepOS. It is not
# a `make check` row because it needs the tepOS repository.
TEP_WRAPPER = $(if $(filter 1,$(or $(TEP),$(TEST_$(TEST)_TEP))),tools/with_tepos.sh)

# `make test TEST=tep-mailbox` alone: start tepOS and the relay yourself.
TEP_NXU_SOCK ?= /tmp/nxu-mailbox.sock
TEP_SERIAL1 := -chardev socket,id=tep,path=$(TEP_NXU_SOCK),server=on,wait=off -serial chardev:tep


.PHONY: test-tep-mailbox

test-tep-mailbox:

	+@MAKE="$(MAKE)" tools/test_tep_mailbox.sh


include $(sort $(wildcard makedefs/tests/*.mk))


# -- make check ----------------------------------------------------------------

# Seconds `make check` waits for a test's pass line before calling it a failure.
CHECK_TIMEOUT ?= 150

# The plain boot has no test define, so it is not in TEST_IDS; check-list adds it.
CHECK_DEFAULT_PASS := kern_init: root userspace services active

CHECK_IDS := $(foreach id,$(TEST_IDS),$(if $(TEST_$(id)_PASS),$(id)))

# The i386 suites `make check` runs, one make target each. A new i386 area adds
# its test-i386-<area> target to this list, or `make check` will not run it.
CHECK_I386_TARGETS := \
    test-i386 \
    test-i386-interrupts \
    test-i386-vm \
    test-i386-threads \
    test-i386-boot \
    test-i386-fs \
    test-i386-devices \
    test-i386-userland \
    test-i386-btrfs \
    test-i386-sound

# The host tests `make check` runs first, one make target each.
CHECK_HOST_TARGETS := \
    test-audio-host \
    test-network-host


# -- Rules ---------------------------------------------------------------------

.PHONY: tests test test-list check check-list check-i386-list check-host-list

ifneq ($(filter test,$(MAKECMDGOALS)),)
ifeq ($(filter $(TEST),$(TEST_IDS)),)
$(error unknown or missing TEST '$(TEST)' -- pick one of: $(TEST_IDS) -- or run `make tests` for the menu)
endif
endif


tests:

	+@MAKE="$(MAKE)" tools/test_menu.sh


test-list:

	@$(foreach id,$(TEST_IDS),printf '%s\t%s\t%s\n' '$(id)' '$(TEST_$(id)_GROUP)' '$(TEST_$(id)_DESC)';)


check-list:

	@printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' 'default' '$(CHECK_TIMEOUT)' '$(CHECK_DEFAULT_PASS)' '-' '-' '-' '-' '4'
	@$(foreach id,$(CHECK_IDS),printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' '$(id)' '$(or $(TEST_$(id)_TIMEOUT),$(CHECK_TIMEOUT))' '$(TEST_$(id)_PASS)' '$(or $(TEST_$(id)_CFLAGS),-)' '$(if $(TEST_$(id)_CAPTURE),1,-)' '$(or $(TEST_$(id)_VERIFY),-)' '$(or $(TEST_$(id)_FRAMEWORKS),-)' '$(or $(TEST_$(id)_SMP),1)';)


check-i386-list:

	@printf '%s\n' $(CHECK_I386_TARGETS)


check-host-list:

	@printf '%s\n' $(CHECK_HOST_TARGETS)


check:

	+@MAKE="$(MAKE)" CHECK_ONLY="$(CHECK_ONLY)" CHECK_I386="$(CHECK_I386)" tools/check.sh


test: $(DISK) $(DISK_FORMAT_STAMP)

	@echo "NXU: building test $(TEST) ($(TEST_$(TEST)_DESC)) with $(BUILD_JOBS) host job(s)"

	$(MAKE) -j$(BUILD_JOBS) \
		BUILD_ROOT=BUILD CONFIG=$(TEST) \
		$(TEST_$(TEST)_FRAMEWORKS) \
		EXTRA_CFLAGS="$(EXTRA_CFLAGS) $(TEST_$(TEST)_CFLAGS)" \
		all

	$(TEP_WRAPPER) qemu-system-aarch64 \
		-machine virt,gic-version=3 \
		-cpu cortex-a72 \
		-smp $(or $(TEST_$(TEST)_SMP),1) \
		-m 8G \
		-kernel BUILD/$(TEST)/kernel.bin \
		-append "$(BOOT_ARGS) $(TEST_$(TEST)_BOOT_ARGS) ui.scale=$(QEMU_UI_SCALE_PERMILLE)" \
		-display $(or $(TEST_$(TEST)_DISPLAY),none) \
		-global virtio-mmio.force-legacy=false \
		-drive if=none,format=raw,file=$(DISK),id=nxudisk \
		$(TEST_$(TEST)_RAMFB) \
		-device virtio-blk-device,drive=nxudisk \
		-device virtio-gpu-device$(TEST_$(TEST)_GPU) \
		-device virtio-keyboard-device \
		-device virtio-mouse-device \
		$(QEMU_NETWORK) \
		$(or $(TEST_$(TEST)_AUDIODEV),$(QEMU_AUDIODEV)) \
		$(QEMU_SOUND_DEVICE) \
		-serial stdio \
		$(TEST_$(TEST)_SERIAL1) \
		-monitor $(or $(TEST_$(TEST)_MONITOR),none)

.PHONY: test-network-host test-network-vm test-network-session

test-network-host:
	$(Q)python3 tools/check_network.py

test-network-vm:
	$(Q)python3 tools/check_network_vm.py

test-network-session: test-network-vm
	$(Q)python3 tools/check_network_session.py
