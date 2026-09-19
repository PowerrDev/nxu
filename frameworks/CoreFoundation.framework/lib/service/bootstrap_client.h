#ifndef NXU_COREFOUNDATION_BOOTSTRAP_CLIENT_H
#define NXU_COREFOUNDATION_BOOTSTRAP_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

/*
 * register: hand bootd a send right to service_port_name under `label`.
 * One-way -- there is no synchronous confirmation, matching the wire
 * protocol in nxu/bootstrap_protocol.h. A later bootstrap_client_lookup
 * for the same label succeeding is the proof registration landed.
 */
bool bootstrap_client_register(const char *label, uint32_t service_port_name);

/*
 * lookup: ask bootd for the port registered under `label`, retrying with
 * nxu_yield() between attempts up to a bounded number of tries (bootd may
 * not have started, or the target service may not have registered yet).
 * On success, *out_port_name names a fresh local reference to the service
 * port, owned by the caller.
 */
bool bootstrap_client_lookup(const char *label, uint32_t *out_port_name);

#endif
