# =============================================================================
# Kernel tests
# =============================================================================
#
#   make run                everything that can share one boot, in one QEMU
#                           (make test TEST=everything, see below)
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
# the TEST_<id>_* variables below; nothing else needs to change.
#
#   TEST_<id>_GROUP       menu section
#   TEST_<id>_DESC        one line for the menu (plain text: no quotes, $ or #)
#   TEST_<id>_CFLAGS      kernel defines that select the test
#   TEST_<id>_FRAMEWORKS  sibling framework switches (UISERVICE=1, WINDOWSERVER=1)
#   TEST_<id>_DISPLAY     QEMU -display argument      (default: none)
#   TEST_<id>_GPU         QEMU virtio-gpu-device args (default: bare device)
#   TEST_<id>_RAMFB       QEMU ramfb device           (default: none)
#   TEST_<id>_MONITOR     QEMU -monitor argument      (default: none)
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

TEST_GUI_STACK := UISERVICE=1 WINDOWSERVER=1

# The rule below adds the `-display` flag itself, so hand it just the backend.
TEST_GUI_DISPLAY := $(QEMU_DISPLAY_BACKEND)

TEST_IDS := \
    everything \
    ui-about \
    ui-voyager \
    windowserver-about \
    journal-crash \
    ipc-process \
    thread-process \
    fault-process \
    process-control \
    socket-process \
    xamethyst-process \
    windowserver-process \
    about-sevos-process \
    sound


# -- Everything at once --------------------------------------------------------

# What `make run` boots: one kernel, one QEMU, everything that can share a boot
# running in it (kern/tests/unified_boot.h). The boot is a normal one -- bootd
# as PID 1 with logd and patchd, the boot chime -- with the UIService session
# (WindowServer + the Voyager app) on the boot thread and a kernel thread that
# runs ipc-process, thread-process, process-control, socket-process and sound
# against the live system, then prints one unified_boot_summary block.
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
#   xamethyst-process    XAmethyst claims the display too (it replaces WindowServer)
#
# `make check` runs it headless: the same kernel with -display none (the UI
# session still runs, on the virtio-gpu framebuffer) and every pass line below
# must show. It does not check the audio recording: the boot chime plays with
# underruns here while the UI loop and the tests share the CPU (known, see
# doc/testing.md), so the recording is checked by the sound row instead.
TEST_everything_GROUP := Everything at once
TEST_everything_DESC := Everything that can run together, in action (what make run boots)
TEST_everything_CFLAGS := -DNXU_UNIFIED_BOOT_TEST -DNXU_UI_SERVICE_APP_VOYAGER
TEST_everything_FRAMEWORKS := $(TEST_GUI_STACK)
TEST_everything_DISPLAY := $(TEST_GUI_DISPLAY)
TEST_everything_GPU := ,xres=$(QEMU_GPU_XRES),yres=$(QEMU_GPU_YRES)
TEST_everything_RAMFB := $(QEMU_RAMFB_DEVICE)
TEST_everything_PASS := ipc_process_test: passed|thread_process_test: passed|process_control_test: passed|socket_process_test: passed|sound_test: passed|boot_chime_play: playback started|unified_boot_summary: UIService Voyager.app running|unified_boot_summary: all 5 test(s) passed
TEST_everything_TIMEOUT := 240


# -- Graphical boot ------------------------------------------------------------

TEST_ui-about_GROUP := Graphical boot
TEST_ui-about_DESC := UIService About screen
TEST_ui-about_CFLAGS := -DNXU_UI_SERVICE_BOOT_TEST
TEST_ui-about_FRAMEWORKS := $(TEST_GUI_STACK)
TEST_ui-about_DISPLAY := $(TEST_GUI_DISPLAY)
TEST_ui-about_GPU := ,xres=$(QEMU_GPU_XRES),yres=$(QEMU_GPU_YRES)
TEST_ui-about_RAMFB := $(QEMU_RAMFB_DEVICE)

TEST_ui-voyager_GROUP := Graphical boot
TEST_ui-voyager_DESC := UIService Voyager app on its own
TEST_ui-voyager_CFLAGS := -DNXU_UI_SERVICE_BOOT_TEST -DNXU_UI_SERVICE_APP_VOYAGER
TEST_ui-voyager_FRAMEWORKS := $(TEST_GUI_STACK)
TEST_ui-voyager_DISPLAY := $(TEST_GUI_DISPLAY)
TEST_ui-voyager_GPU := ,xres=$(QEMU_GPU_XRES),yres=$(QEMU_GPU_YRES)
TEST_ui-voyager_RAMFB := $(QEMU_RAMFB_DEVICE)

TEST_windowserver-about_GROUP := Graphical boot
TEST_windowserver-about_DESC := WindowServer About screen
TEST_windowserver-about_CFLAGS := -DNXU_AQUA_BOOT_TEST
TEST_windowserver-about_FRAMEWORKS := $(TEST_GUI_STACK)
TEST_windowserver-about_DISPLAY := $(TEST_GUI_DISPLAY)
TEST_windowserver-about_GPU := ,xres=$(QEMU_GPU_XRES),yres=$(QEMU_GPU_YRES)
TEST_windowserver-about_RAMFB := $(QEMU_RAMFB_DEVICE)


# -- Filesystem ----------------------------------------------------------------

# Crashes mid-transaction on purpose; boot again with `make run` to watch the
# ext4 journal replay.
TEST_journal-crash_GROUP := Filesystem
TEST_journal-crash_DESC := Crash mid-transaction; make run replays it
TEST_journal-crash_CFLAGS := -DNXU_JOURNAL_CRASH_TEST
TEST_journal-crash_DISPLAY := cocoa


# -- Userland processes --------------------------------------------------------

TEST_ipc-process_GROUP := Userland processes
TEST_ipc-process_DESC := bootd registry, two processes exchange IPC
TEST_ipc-process_PASS := ipc_process_test: passed
TEST_ipc-process_CFLAGS := -DNXU_IPC_PROCESS_TEST

TEST_thread-process_GROUP := Userland processes
TEST_thread-process_DESC := User process spawns and joins its threads
TEST_thread-process_PASS := thread_process_test: passed
TEST_thread-process_CFLAGS := -DNXU_THREAD_PROCESS_TEST

TEST_fault-process_GROUP := Userland processes
TEST_fault-process_DESC := Faulting user processes are killed, not the kernel
TEST_fault-process_PASS := fault_process_test: passed
TEST_fault-process_CFLAGS := -DNXU_FAULT_PROCESS_TEST

TEST_process-control_GROUP := Userland processes
TEST_process-control_DESC := fork, exec, signals, copy-on-write, demand paging
TEST_process-control_PASS := process_control_test: passed
TEST_process-control_CFLAGS := -DNXU_PROCESS_CONTROL_TEST

TEST_socket-process_GROUP := Userland processes
TEST_socket-process_DESC := Socket server and client stream a payload
TEST_socket-process_PASS := socket_process_test: passed
TEST_socket-process_CFLAGS := -DNXU_SOCKET_PROCESS_TEST

TEST_xamethyst-process_GROUP := Userland processes
TEST_xamethyst-process_DESC := XAmethyst X11 handshake and input
TEST_xamethyst-process_CFLAGS := -DNXU_XAMETHYST_PROCESS_TEST

TEST_windowserver-process_GROUP := Userland processes
TEST_windowserver-process_DESC := WindowServer as a process (monitor :45455)
TEST_windowserver-process_CFLAGS := -DNXU_WINDOWSERVER_PROCESS_TEST
TEST_windowserver-process_FRAMEWORKS := $(TEST_GUI_STACK)
TEST_windowserver-process_GPU := ,xres=$(QEMU_GPU_XRES),yres=$(QEMU_GPU_YRES)
TEST_windowserver-process_MONITOR := telnet:127.0.0.1:45455,server,nowait

TEST_about-sevos-process_GROUP := Userland processes
TEST_about-sevos-process_DESC := About sevOS app (monitor :45456)
TEST_about-sevos-process_CFLAGS := -DNXU_ABOUT_SEVOS_PROCESS_TEST
TEST_about-sevos-process_FRAMEWORKS := $(TEST_GUI_STACK)
TEST_about-sevos-process_GPU := ,xres=1280,yres=960
TEST_about-sevos-process_MONITOR := telnet:127.0.0.1:45456,server,nowait


# -- Sound ---------------------------------------------------------------------

# The kernel plays a test tone, the boot chime (through the boot code) and then
# playsound (a user process) into QEMU's audio backend. `make test TEST=sound`
# plays it through the host's speakers; `make check` records it with the wav
# backend and verify_capture.py compares the recording with the tone formula and
# with Boot_Audio.wav sample by sample.
TEST_sound_GROUP := Sound
TEST_sound_DESC := Tone, boot chime and playsound through VirtIO Sound (audible)
TEST_sound_PASS := sound_test: passed
TEST_sound_CFLAGS := -DNXU_SOUND_TEST
TEST_sound_TIMEOUT := 120
TEST_sound_CAPTURE := 1
TEST_sound_VERIFY := python3 tools/audio/verify_capture.py @CAPTURE@ tools/DiskRoot/System/Library/Resources/Audio/Boot_Audio.wav --tone --chimes 2


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
    test-audio-host


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

	@printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' 'default' '$(CHECK_TIMEOUT)' '$(CHECK_DEFAULT_PASS)' '-' '-' '-' '-'
	@$(foreach id,$(CHECK_IDS),printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' '$(id)' '$(or $(TEST_$(id)_TIMEOUT),$(CHECK_TIMEOUT))' '$(TEST_$(id)_PASS)' '$(or $(TEST_$(id)_CFLAGS),-)' '$(if $(TEST_$(id)_CAPTURE),1,-)' '$(or $(TEST_$(id)_VERIFY),-)' '$(or $(TEST_$(id)_FRAMEWORKS),-)';)


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

	qemu-system-aarch64 \
		-machine virt,gic-version=3 \
		-cpu cortex-a72 \
		-smp 1 \
		-m 512M \
		-kernel BUILD/$(TEST)/kernel.bin \
		-append "$(BOOT_ARGS) ui.scale=$(QEMU_UI_SCALE_PERMILLE)" \
		-display $(or $(TEST_$(TEST)_DISPLAY),none) \
		-global virtio-mmio.force-legacy=false \
		-drive if=none,format=raw,file=$(DISK),id=nxudisk \
		$(TEST_$(TEST)_RAMFB) \
		-device virtio-blk-device,drive=nxudisk \
		-device virtio-gpu-device$(TEST_$(TEST)_GPU) \
		-device virtio-keyboard-device \
		-device virtio-mouse-device \
		$(or $(TEST_$(TEST)_AUDIODEV),$(QEMU_AUDIODEV)) \
		$(QEMU_SOUND_DEVICE) \
		-serial stdio \
		-monitor $(or $(TEST_$(TEST)_MONITOR),none)
