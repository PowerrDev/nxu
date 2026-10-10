#ifndef NXU_TEST_SIGNAL_H
#define NXU_TEST_SIGNAL_H
#include <kern/process/proc.h>
#include <kern/syscall/syscall_defs.h>

typedef enum { SIGNAL_SEND_OK } signal_send_result_t;

signal_send_result_t signal_send(proc_t, proc_id_t, uint32_t);
#endif
