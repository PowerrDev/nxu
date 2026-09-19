#ifndef NXU_USER_THREAD_H
#define NXU_USER_THREAD_H

#include <stdint.h>

/*
 * pthread_create-style convenience over nxu_mmap + nxu_thread_create
 * (frameworks/include/nxu/syscall.h): allocates a private stack of
 * stack_size bytes and starts entry(arg) on it as a new thread sharing the
 * caller's address space. Returns the new thread's id (> 0) on success, a
 * negative -NXU_SYS_E_* on failure.
 */
int64_t nxu_thread_spawn(void (*entry)(void *arg), void *arg, uint64_t stack_size);

#endif
