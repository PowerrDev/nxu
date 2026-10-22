# Test definition: smp-user
TEST_smp-user_GROUP := Kernel
TEST_smp-user_DESC := SMP userland: EL0 processes and threads on four CPUs, concurrently
TEST_smp-user_PASS := smp_user_test: passed
TEST_smp-user_CFLAGS := -DNXU_SMP_USER_TEST
TEST_smp-user_SMP := 4
TEST_smp-user_TIMEOUT := 300
