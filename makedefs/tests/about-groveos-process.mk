# Test definition: about-groveos-process
TEST_about-groveos-process_GROUP := Userland processes
TEST_about-groveos-process_DESC := About groveOS app (monitor :45456)
TEST_about-groveos-process_CFLAGS := -DNXU_ABOUT_GROVEOS_PROCESS_TEST
TEST_about-groveos-process_FRAMEWORKS := $(TEST_GUI_STACK)
TEST_about-groveos-process_GPU := ,xres=1280,yres=960
TEST_about-groveos-process_MONITOR := telnet:127.0.0.1:45456,server,nowait
