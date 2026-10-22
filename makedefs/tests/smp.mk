# Test definition: smp
TEST_smp_GROUP := Kernel
TEST_smp_DESC := SMP: four CPUs, per-CPU run queues, IPIs, affinity, migration, shared memory
TEST_smp_PASS := smp_test: passed
TEST_smp_CFLAGS := -DNXU_SMP_TEST
TEST_smp_SMP := 8
TEST_smp_TIMEOUT := 120
