# Test definition: tty
TEST_tty_GROUP := Kernel
TEST_tty_DESC := Terminals: the core, line discipline and termios requests in the kernel, then ttytest and stty from user space
TEST_tty_PASS := termios_process_test: passed
TEST_tty_CFLAGS := -DNXU_TTY_TEST
TEST_tty_TIMEOUT := 90
