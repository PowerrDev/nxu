# Test definition: ui-about
TEST_ui-about_GROUP := Graphical boot
TEST_ui-about_DESC := UIService desktop alone (About sevOS from the system menu)
TEST_ui-about_CFLAGS := -DNXU_UI_SERVICE_BOOT_TEST
TEST_ui-about_FRAMEWORKS := $(TEST_GUI_STACK)
TEST_ui-about_DISPLAY := $(TEST_GUI_DISPLAY)
TEST_ui-about_GPU := ,xres=$(QEMU_GPU_XRES),yres=$(QEMU_GPU_YRES)
TEST_ui-about_RAMFB := $(QEMU_RAMFB_DEVICE)
