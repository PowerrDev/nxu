#ifndef NXU_I386_STARTUP_H
#define NXU_I386_STARTUP_H

#include <stdbool.h>

bool i386_startup_tests_enabled(void);
bool i386_startup_core_test(void);
bool i386_startup_storage_test(void);
#endif
