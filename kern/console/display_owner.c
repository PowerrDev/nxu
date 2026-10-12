#include <kern/console/display_owner.h>
#include <kern/lock.h>
#include <kern/process/signal.h>
#include <kern/syscall/syscall_defs.h>
#include <libk/string.h>
#include <drivers/video/ui_service_bridge.h>

static nxu_spinlock_t g_owner_lock;
static proc_ident_t g_owner;
static uint64_t g_next_epoch = 1;

static bool
same(proc_ident_t a, proc_ident_t b)
{
	return a.pid == b.pid && a.uniqueid == b.uniqueid && a.idversion == b.idversion;
}

bool
display_owner_claim(proc_id_t pid)
{
	proc_t proc = proc_find(pid);
	if (!proc || pid == PROC_PID_KERNEL || (proc->p_flag & PROC_FLAG_EXITING)) return false;
	uint64_t irq = nxu_spin_lock_irqsave(&g_owner_lock);
	bool allowed = g_owner.pid == PROC_PID_KERNEL || same(g_owner, proc->p_ident);
	if (allowed) g_owner = proc->p_ident;
	nxu_spin_unlock_irqrestore(&g_owner_lock, irq);
	return allowed;
}

bool
display_owner_is(proc_id_t pid)
{
	if (pid == PROC_PID_KERNEL) return false;

	proc_t proc = proc_find(pid);
	if (!proc) return false;

	proc_ident_t identity = proc->p_ident;
	proc_rele(proc);

	uint64_t irq = nxu_spin_lock_irqsave(&g_owner_lock);
	bool allowed = same(g_owner, identity);
	nxu_spin_unlock_irqrestore(&g_owner_lock, irq);
	return allowed;
}

void
display_owner_release(proc_t proc)
{
	uint64_t irq = nxu_spin_lock_irqsave(&g_owner_lock);
	if (same(g_owner, proc->p_ident)) memset(&g_owner, 0, sizeof(g_owner));
	proc->p_gui_epoch = proc->p_gui_features = 0;
	proc->p_gui_record_limit = 0;
	proc->p_gui_minor = 0;
	nxu_spin_unlock_irqrestore(&g_owner_lock, irq);
	ui_bridge_host_release(proc);
}

int64_t
display_owner_host(proc_t proc, const nxu_gui_host_hello_t *request, nxu_gui_host_hello_reply_t *reply)
{
	const nxu_gui_host_header_t *h = &request->header;
	memset(reply, 0, sizeof(*reply));
	reply->header = (nxu_gui_host_header_t){sizeof(*reply), NXU_GUI_HOST_MAJOR, NXU_GUI_HOST_MINOR, h->operation, 0, 0,
											h->request_id};
	int32_t status = 0;
	uint64_t irq = nxu_spin_lock_irqsave(&g_owner_lock);
	if ((proc->p_flag & PROC_FLAG_EXITING) || !proc_has_caps(proc, NXU_CAP_DISPLAY))
		status = -NXU_SYS_E_DENIED;
	else if (h->major != NXU_GUI_HOST_MAJOR)
		status = -NXU_SYS_E_NOT_SUPPORTED;
	else if (h->flags || !h->request_id)
		status = -NXU_SYS_E_INVALID_ARGUMENT;
	else if (h->operation == NXU_GUI_HOST_HELLO) {
		uint64_t enabled =
			request->supported_features &
			(NXU_GUI_HOST_FEATURE_LEASE |
			 (h->minor >= 1 && request->max_record_size >= sizeof(nxu_gui_host_desktop_t) ? NXU_GUI_HOST_FEATURE_SESSION
																						  : 0));
		if (h->size != sizeof(*request) || h->session_epoch || request->reserved ||
			request->max_record_size < sizeof(*request) || (request->required_features & ~request->supported_features))
			status = -NXU_SYS_E_INVALID_ARGUMENT;
		else if (request->required_features & ~enabled)
			status = -NXU_SYS_E_NOT_SUPPORTED;
		else if (proc->p_gui_epoch)
			status = -NXU_SYS_E_BUSY;
		else if (g_next_epoch == UINT64_MAX)
			status = -NXU_SYS_E_NO_SPACE;
		else {
			proc->p_gui_epoch = g_next_epoch++;
			proc->p_gui_features = enabled;
			proc->p_gui_minor = h->minor < NXU_GUI_HOST_MINOR ? h->minor : NXU_GUI_HOST_MINOR;
			proc->p_gui_record_limit =
				request->max_record_size < NXU_GUI_HOST_MAX_RECORD ? request->max_record_size : NXU_GUI_HOST_MAX_RECORD;
		}
	} else if (h->operation == NXU_GUI_HOST_DISPLAY_ATTACH) {
		if (h->size != sizeof(*h) || h->minor != proc->p_gui_minor || !h->session_epoch ||
			h->session_epoch != proc->p_gui_epoch)
			status = -NXU_SYS_E_INVALID_ARGUMENT;
		else if (!(proc->p_gui_features & NXU_GUI_HOST_FEATURE_LEASE))
			status = -NXU_SYS_E_DENIED;
		else if (g_owner.pid != PROC_PID_KERNEL && !same(g_owner, proc->p_ident))
			status = -NXU_SYS_E_BUSY;
		else
			g_owner = proc->p_ident;
	} else if (h->operation == NXU_GUI_HOST_RELEASE) {
		if (h->size != sizeof(*h) || h->minor != proc->p_gui_minor || !h->session_epoch ||
			h->session_epoch != proc->p_gui_epoch)
			status = -NXU_SYS_E_INVALID_ARGUMENT;
		else {
			if (same(g_owner, proc->p_ident)) memset(&g_owner, 0, sizeof(g_owner));
			proc->p_gui_epoch = proc->p_gui_features = 0;
			proc->p_gui_record_limit = 0;
			proc->p_gui_minor = 0;
		}
	} else
		status = -NXU_SYS_E_NOT_SUPPORTED;
	if (!status) {
		reply->header.minor = proc->p_gui_minor;
		reply->header.session_epoch = proc->p_gui_epoch;
		reply->enabled_features = proc->p_gui_features;
		reply->max_record_size = proc->p_gui_record_limit;
	}
	reply->status = status;
	nxu_spin_unlock_irqrestore(&g_owner_lock, irq);
	return status;
}

/* Desktop policy is process-local. Retire the old compositor together with
 * its desktop so a replacement cannot inherit orphaned window IDs. */
void
display_owner_restart(void)
{
	uint64_t irq = nxu_spin_lock_irqsave(&g_owner_lock);
	proc_ident_t owner = g_owner;
	nxu_spin_unlock_irqrestore(&g_owner_lock, irq);
	if (owner.pid == PROC_PID_KERNEL) return;

	proc_t compositor = proc_find_ident(&owner);

	if (compositor) {
		(void)signal_send(proc_kernel(), compositor->p_ident.pid, NXU_SIGKILL);
		proc_rele(compositor);
	}
}
