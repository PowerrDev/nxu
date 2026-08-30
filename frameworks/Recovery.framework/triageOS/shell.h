#ifndef RECOVERY_USER_TRIAGEOS_SHELL_H
#define RECOVERY_USER_TRIAGEOS_SHELL_H

#include <stdint.h>

#define TRIAGE_SHELL_CWD_MAX 256U

typedef void (*triage_shell_write_fn)(void *context, const char *text);

typedef enum {
	TRIAGE_SHELL_OK = 0,
	TRIAGE_SHELL_CLEAR,
	TRIAGE_SHELL_EXIT
} triage_shell_result_t;

typedef struct {
	char cwd[TRIAGE_SHELL_CWD_MAX];
	triage_shell_write_fn write;
	void *write_context;
} triage_shell_t;

void triage_shell_init(triage_shell_t *shell, triage_shell_write_fn write, void *context);
triage_shell_result_t triage_shell_execute(triage_shell_t *shell, const char *command);

#endif
