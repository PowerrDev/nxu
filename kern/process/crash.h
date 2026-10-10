#ifndef NXU_PROCESS_CRASH_H
#define NXU_PROCESS_CRASH_H
#include <kern/process/proc.h>
#include <kern/process/thread.h>
/* Capture is bounded, allocation-free, and independent of daemon liveness. */
void crash_begin(proc_t proc, uint64_t status);
void crash_save_current(const void *frame);
void crash_collect_thread(task_t task, thread_t thread);
void crash_incomplete(task_t task);
void crash_finish(proc_t proc, bool complete);
void crash_release(proc_t proc);
int64_t crash_notify(const nxu_crash_notification_t *notification);
int64_t crash_ui_next(nxu_crash_notification_t *notification);
int64_t crash_next(proc_t consumer, nxu_crash_report_t *report);
int64_t crash_ack(proc_t consumer, uint64_t incident);
#endif
