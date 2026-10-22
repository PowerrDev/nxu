# Test definition: process-control
TEST_process-control_GROUP := Userland processes
TEST_process-control_DESC := fork, exec, signals, copy-on-write, demand paging
TEST_process-control_PASS := process_control_test: passed
TEST_process-control_CFLAGS := -DNXU_PROCESS_CONTROL_TEST
