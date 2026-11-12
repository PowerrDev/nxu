/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/process/proc_internal.h
 *
 * What kern/process/proc.c shares with the other files of the process
 * subsystem and with nobody else: the process table lock and the lookups that
 * are only meaningful while it is held.
 *
 * The process table lock protects the process lists, the parent/child tree,
 * every process's state and flags, and all process-group and session state
 * (kern/process/pgrp.h). It is a spinlock: nothing that sleeps may be called
 * with it held. p_siglock nests inside it.
 */

#ifndef NXU_KERN_PROC_INTERNAL_H
#define NXU_KERN_PROC_INTERNAL_H

#include <kern/process/proc.h>

void proc_table_lock(void);
void proc_table_unlock(void);

/* The live (not zombie, not dead) process with this pid, or 0. No reference is taken. */
proc_t proc_find_live_locked(proc_id_t pid);

/* Take a reference to proc; false if it is being torn down or the count is saturated. */
bool proc_reference_locked(proc_t proc);

/* True for a process that still runs: not an embryo, not exiting, not a zombie, not dead. */
bool proc_is_live_locked(proc_t proc);

#endif
