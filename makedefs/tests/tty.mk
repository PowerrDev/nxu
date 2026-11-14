# Test definition: tty
TEST_tty_GROUP := Kernel
TEST_tty_DESC := Terminals: the core, line discipline, termios requests and pseudo-terminals in the kernel, then ttytest, stty and restarttest from user space
TEST_tty_PASS := restart_process_test: passed
TEST_tty_CFLAGS := -DNXU_TTY_TEST
TEST_tty_TIMEOUT := 90
