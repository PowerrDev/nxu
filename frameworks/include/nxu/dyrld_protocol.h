#ifndef NXU_DYRLD_PROTOCOL_H
#define NXU_DYRLD_PROTOCOL_H
#include <kern/syscall/dyrld_defs.h>
#define NXU_DYRLD_SERVICE "com.nxu.dyrld"
#define NXU_DYRLD_PROTOCOL_VERSION 1U

typedef struct {
	uint32_t version, status;
	char path[NXU_DYRLD_PATH];
	uint8_t identity[32];
} nxu_dyrld_measurement_t;
#endif
