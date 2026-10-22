# Test definition: ipc-process
TEST_ipc-process_GROUP := Userland processes
TEST_ipc-process_DESC := bootd registry, two processes exchange IPC
TEST_ipc-process_PASS := ipc_process_test: passed
TEST_ipc-process_CFLAGS := -DNXU_IPC_PROCESS_TEST
