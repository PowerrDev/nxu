#ifndef NXU_SECURE_ANCHOR_H
#define NXU_SECURE_ANCHOR_H

#include <kern/process/proc.h>
#include <vfs/vfs.h>

/*
 * Approval is for one exact file, or one directory tree owned by the recovery
 * PID 1 request. No transferable privilege grant or app-supplied password.
 */
bool sap_canonical_path(const char *path, char canonical[VFS_PATH_MAX]);
bool sap_protected_path(const char *path);
vfs_status_t sap_delete(proc_t caller, const char *path);
vfs_status_t sap_remove_tree(proc_t caller, const char *path);
void sap_cancel(proc_t caller);

#endif /* NXU_SECURE_ANCHOR_H */
