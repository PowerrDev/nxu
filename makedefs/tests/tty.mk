# Test definition: tty
TEST_tty_GROUP := Kernel
TEST_tty_DESC := Terminals: the core, line discipline and termios requests, driven through /dev in the kernel
TEST_tty_PASS := tty_test_run: passed
TEST_tty_CFLAGS := -DNXU_TTY_TEST
TEST_tty_TIMEOUT := 90
