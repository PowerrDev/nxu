# Test definition: ui-voyager
TEST_ui-voyager_GROUP := Graphical boot
TEST_ui-voyager_DESC := UIService desktop alone (apps from the Dock)
TEST_ui-voyager_CFLAGS := -DNXU_UI_SERVICE_BOOT_TEST
TEST_ui-voyager_FRAMEWORKS := $(TEST_GUI_STACK)
TEST_ui-voyager_DISPLAY := $(TEST_GUI_DISPLAY)
TEST_ui-voyager_GPU := ,xres=$(QEMU_GPU_XRES),yres=$(QEMU_GPU_YRES)
TEST_ui-voyager_RAMFB := $(QEMU_RAMFB_DEVICE)
