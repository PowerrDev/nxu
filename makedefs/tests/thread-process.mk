# Test definition: thread-process
TEST_thread-process_GROUP := Userland processes
TEST_thread-process_DESC := User process spawns and joins its threads
TEST_thread-process_PASS := thread_process_test: passed
TEST_thread-process_CFLAGS := -DNXU_THREAD_PROCESS_TEST
