# Test definition: socket-process
TEST_socket-process_GROUP := Userland processes
TEST_socket-process_DESC := Socket server and client stream a payload
TEST_socket-process_PASS := socket_process_test: passed
TEST_socket-process_CFLAGS := -DNXU_SOCKET_PROCESS_TEST
