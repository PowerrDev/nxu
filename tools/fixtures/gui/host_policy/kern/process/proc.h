#ifndef NXU_TEST_PROC_H
#define NXU_TEST_PROC_H

#include <stdint.h>
#include <stdbool.h>
typedef uint32_t proc_id_t;

typedef struct {
	uint32_t pid;
	uint64_t uniqueid;
	uint32_t idversion;
} proc_ident_t;

typedef struct proc {
	proc_ident_t p_ident;
	uint32_t p_flag, caps;
	uint64_t p_gui_epoch, p_gui_features;
	uint32_t p_gui_record_limit;
	uint16_t p_gui_minor;
} *proc_t;

#define PROC_PID_KERNEL 0
#define PROC_FLAG_EXITING 1
proc_t proc_find(proc_id_t);
bool proc_has_caps(proc_t, uint32_t);
proc_t proc_kernel(void);
proc_t proc_find_ident(const proc_ident_t *);
void proc_rele(proc_t);

#endif
