# Test definition: serial
TEST_serial_GROUP := Kernel
TEST_serial_DESC := The serial terminal: /dev/console driven from the host through the console UART (line editing, echo, Ctrl-C, raw reads, a flood held back in the UART)
TEST_serial_PASS := serial_test_run: passed
TEST_serial_CFLAGS := -DNXU_SERIAL_TEST
TEST_serial_TIMEOUT := 60
TEST_serial_SERIAL_SCRIPT := tools/serial_test.script
