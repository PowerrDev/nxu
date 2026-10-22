# Test definition: desktop
TEST_desktop_GROUP := Everything at once
TEST_desktop_DESC := The normal desktop boot (what make run boots)
TEST_desktop_CFLAGS := -DNXU_DESKTOP_BOOT
TEST_desktop_FRAMEWORKS := UISERVICE=0 WINDOWSERVER=0
TEST_desktop_BOOT_ARGS := ui.userland=1 log.trace=0 log.stats_ms=0
TEST_desktop_DISPLAY := $(TEST_GUI_DISPLAY)
TEST_desktop_GPU := ,xres=$(QEMU_GPU_XRES),yres=$(QEMU_GPU_YRES)
TEST_desktop_RAMFB := $(QEMU_RAMFB_DEVICE)
TEST_desktop_SMP := 12
TEST_desktop_TEP := 1
