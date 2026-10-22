# Test definition: about-sevos-process
TEST_about-sevos-process_GROUP := Userland processes
TEST_about-sevos-process_DESC := About sevOS app (monitor :45456)
TEST_about-sevos-process_CFLAGS := -DNXU_ABOUT_SEVOS_PROCESS_TEST
TEST_about-sevos-process_FRAMEWORKS := $(TEST_GUI_STACK)
TEST_about-sevos-process_GPU := ,xres=1280,yres=960
TEST_about-sevos-process_MONITOR := telnet:127.0.0.1:45456,server,nowait
