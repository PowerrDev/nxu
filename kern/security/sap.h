#ifndef NXU_SECURE_ANCHOR_H
#define NXU_SECURE_ANCHOR_H
#include <kern/process/proc.h>
#include <vfs/vfs.h>
/* Only single regular-file deletion is supported. No recursive erase or
 * transferable privilege grant. SAP never accepts a password from an app. */
bool sap_canonical_path(const char *path, char canonical[VFS_PATH_MAX]);
bool sap_protected_path(const char *path);
vfs_status_t sap_delete(proc_t caller, const char *path);
void sap_cancel(proc_t caller);
#endif
