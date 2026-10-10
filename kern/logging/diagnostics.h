#ifndef NXU_DIAGNOSTICS_H
#define NXU_DIAGNOSTICS_H
#include <kern/syscall/syscall.h>
void diagnostics_start(void);
void diagnostics_syscall_begin(const syscall_request_t *request);
void diagnostics_syscall_end(const syscall_request_t *request, syscall_result_t result);
#endif
