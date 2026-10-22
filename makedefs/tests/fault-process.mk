# Test definition: fault-process
TEST_fault-process_GROUP := Userland processes
TEST_fault-process_DESC := Faulting user processes are killed, not the kernel
TEST_fault-process_PASS := fault_process_test: passed
TEST_fault-process_CFLAGS := -DNXU_FAULT_PROCESS_TEST
