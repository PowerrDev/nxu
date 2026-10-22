# Test definition: windowserver-about
TEST_windowserver-about_GROUP := Graphical boot
TEST_windowserver-about_DESC := WindowServer About screen
TEST_windowserver-about_CFLAGS := -DNXU_AQUA_BOOT_TEST
TEST_windowserver-about_FRAMEWORKS := $(TEST_GUI_STACK)
TEST_windowserver-about_DISPLAY := $(TEST_GUI_DISPLAY)
TEST_windowserver-about_GPU := ,xres=$(QEMU_GPU_XRES),yres=$(QEMU_GPU_YRES)
TEST_windowserver-about_RAMFB := $(QEMU_RAMFB_DEVICE)
