#ifndef NXU_KERN_CONSOLE_IOREGISTRY_H
#define NXU_KERN_CONSOLE_IOREGISTRY_H

#include <stdint.h>

/*
 * Minimal IOKit-style device tree, populated only from real attach events
 * (VirtIO device match points, subsystem bring-up) elsewhere in the kernel
 * -- never fabricated. ioreg_dump() prints it as an indented tree, the way
 * the real `ioreg` command lists the IORegistry.
 */

typedef uint32_t ioreg_id_t;

#define IOREG_ROOT 0U
#define IOREG_INVALID 0xFFFFFFFFU

/* Adds a node under `parent`, returning its id (or IOREG_INVALID if full). */
ioreg_id_t ioreg_add(ioreg_id_t parent, const char *name, const char *class_name);

/* Print the whole tree, root first, each child indented under its parent. */
void ioreg_dump(void);

/*
 * Lazily created, memoized well-known nodes shared across every driver that
 * wants to register a child under one of them, without the caller needing
 * to plumb an id through from wherever the family first attached.
 */
ioreg_id_t ioreg_family_platform(void);
ioreg_id_t ioreg_family_hid(void);
ioreg_id_t ioreg_family_storage(void);
ioreg_id_t ioreg_family_graphics(void);

#endif
