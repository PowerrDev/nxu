# =============================================================================
# Kernel tests
# =============================================================================
#
#   make tests              interactive picker (tools/test_menu.sh)
#   make test TEST=<id>     build and boot a single test directly
#   make test-list          machine-readable registry (id, group, description)
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

TEST_GUI_STACK := UISERVICE=1 WINDOWSERVER=1

# QEMU_DISPLAY is a full `-display <backend>` option; the rule adds the flag itself.
TEST_GUI_DISPLAY := $(patsubst -display %,%,$(QEMU_DISPLAY))

TEST_IDS := \
    ui-about \
    ui-voyager \
    windowserver-about \
    journal-crash \
    ipc-process \
    thread-process \
    socket-process \
    xamethyst-process \
    windowserver-process \
    about-sevos-process


# -- Graphical boot ------------------------------------------------------------

TEST_ui-about_GROUP := Graphical boot
TEST_ui-about_DESC := UIService About screen
TEST_ui-about_CFLAGS := -DNXU_UI_SERVICE_BOOT_TEST
TEST_ui-about_FRAMEWORKS := $(TEST_GUI_STACK)
TEST_ui-about_DISPLAY := $(TEST_GUI_DISPLAY)
TEST_ui-about_GPU := ,xres=$(QEMU_GPU_XRES),yres=$(QEMU_GPU_YRES)
TEST_ui-about_RAMFB := $(QEMU_RAMFB_DEVICE)

TEST_ui-voyager_GROUP := Graphical boot
TEST_ui-voyager_DESC := UIService Voyager app (what make run boots)
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
TEST_journal-crash_DISPLAY := gtk


# -- Userland processes --------------------------------------------------------

TEST_ipc-process_GROUP := Userland processes
TEST_ipc-process_DESC := bootd registry, two processes exchange IPC
TEST_ipc-process_CFLAGS := -DNXU_IPC_PROCESS_TEST

TEST_thread-process_GROUP := Userland processes
TEST_thread-process_DESC := User process spawns and joins its threads
TEST_thread-process_CFLAGS := -DNXU_THREAD_PROCESS_TEST

TEST_socket-process_GROUP := Userland processes
TEST_socket-process_DESC := Socket server and client stream a payload
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


# -- Rules ---------------------------------------------------------------------

.PHONY: tests test test-list

ifneq ($(filter test,$(MAKECMDGOALS)),)
ifeq ($(filter $(TEST),$(TEST_IDS)),)
$(error unknown or missing TEST '$(TEST)' -- pick one of: $(TEST_IDS) -- or run `make tests` for the menu)
endif
endif


tests:

	+@MAKE="$(MAKE)" tools/test_menu.sh


test-list:

	@$(foreach id,$(TEST_IDS),printf '%s\t%s\t%s\n' '$(id)' '$(TEST_$(id)_GROUP)' '$(TEST_$(id)_DESC)';)


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
		-serial stdio \
		-monitor $(or $(TEST_$(TEST)_MONITOR),none)
