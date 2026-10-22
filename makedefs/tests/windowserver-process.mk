# Test definition: windowserver-process
TEST_windowserver-process_GROUP := Userland processes
TEST_windowserver-process_DESC := WindowServer as a process (monitor :45455)
TEST_windowserver-process_CFLAGS := -DNXU_WINDOWSERVER_PROCESS_TEST
TEST_windowserver-process_FRAMEWORKS := $(TEST_GUI_STACK)
TEST_windowserver-process_GPU := ,xres=$(QEMU_GPU_XRES),yres=$(QEMU_GPU_YRES)
TEST_windowserver-process_MONITOR := telnet:127.0.0.1:45455,server,nowait
