# Test definition: everything
TEST_everything_GROUP := Everything at once
TEST_everything_DESC := Everything that can run together, in action (the make check regression row; make run boots the desktop test instead)
TEST_everything_CFLAGS := -DNXU_UNIFIED_BOOT_TEST
TEST_everything_FRAMEWORKS := $(TEST_GUI_STACK)
TEST_everything_DISPLAY := $(TEST_GUI_DISPLAY)
TEST_everything_GPU := ,xres=$(QEMU_GPU_XRES),yres=$(QEMU_GPU_YRES)
TEST_everything_RAMFB := $(QEMU_RAMFB_DEVICE)
TEST_everything_PASS := ipc_process_test: passed|thread_process_test: passed|process_control_test: passed|socket_process_test: passed|sound_test: passed|boot_chime_play: playback started|unified_boot_summary: UIService desktop running|dock: showing 2 app|unified_boot_summary: all 5 test(s) passed
TEST_everything_TIMEOUT := 240
TEST_everything_SMP := 4
