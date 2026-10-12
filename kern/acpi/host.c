/* NXU's boot-time uACPI host. Runtime SCI/GPE delivery is not enabled here. */

#include <uacpi/kernel_api.h>
#include <kern/i386/pmap.h>
#include <kern/i386/vm_param.h>
#include <kern/memory/heap.h>
#include <kern/machine/timer.h>
#include <kern/console/console.h>
#include <kern/lock.h>
#include <libk/string.h>

uint64_t nxu_acpi_rsdp;

uacpi_status
uacpi_kernel_get_rsdp(uacpi_phys_addr *out)
{
	*out = nxu_acpi_rsdp;
	return *out ? UACPI_STATUS_OK : UACPI_STATUS_NOT_FOUND;
}

void *
uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len)
{
	if (!len || addr + len < addr || addr + len > 0x100000000ULL) {
		return UACPI_MAP_FAILED;
	}

	if (addr + len <= VM_DIRECT_MAP_SIZE) {
		return (void *)(uintptr_t)(addr + VMM_HIGHER_HALF_BASE);
	}

	void *mapped = 0;

	return pmap_map_mmio(addr, len, &mapped) ? mapped : UACPI_MAP_FAILED;
}

void
uacpi_kernel_unmap(void *addr, uacpi_size len)
{
	if (pmap_mmio_contains(addr)) {
		(void)pmap_unmap_mmio(addr, len);
	}
}

void
uacpi_kernel_log(uacpi_log_level level, const char *message)
{
	(void)level;
	while (*message) {
		char line[256];
		unsigned n = 0;

		while (*message && *message != '\n' && n < sizeof(line) - 1) {
			line[n++] = *message++;
		}

		if (*message == '\n') {
			message++;
		}

		line[n] = 0;
		if (n) {
			kprintf("NxuACPIPlatform: %s\n", line);
		}
	}
}

void *
uacpi_kernel_alloc(uacpi_size n)
{
	return kmalloc(n);
}

void *
uacpi_kernel_alloc_zeroed(uacpi_size n)
{
	return kcalloc(1, n);
}

void
uacpi_kernel_free(void *p)
{
	if (p) {
		(void)kfree(p);
	}
}

uacpi_u64
uacpi_kernel_get_nanoseconds_since_boot(void)
{
	return timer_get_microseconds() * 1000ULL;
}

void
uacpi_kernel_stall(uacpi_u8 us)
{
	uint64_t end = timer_get_microseconds() + us;

	while (timer_get_microseconds() < end) {
		cpu_relax();
	}
}

void
uacpi_kernel_sleep(uacpi_u64 ms)
{
	uint64_t end = timer_get_microseconds() + ms * 1000ULL;

	while (timer_get_microseconds() < end) {
		cpu_relax();
	}
}

/* This interpreter is used on the bootstrap thread, before AP dispatch. */
uacpi_thread_id
uacpi_kernel_get_thread_id(void)
{
	return (void *)(uintptr_t)1;
}

uacpi_interrupt_state
uacpi_kernel_disable_interrupts(void)
{
	return ml_irq_save();
}

void
uacpi_kernel_restore_interrupts(uacpi_interrupt_state f)
{
	ml_irq_restore(f);
}

uacpi_handle
uacpi_kernel_create_mutex(void)
{
	return kcalloc(1, sizeof(nxu_spinlock_t));
}

void
uacpi_kernel_free_mutex(uacpi_handle h)
{
	uacpi_kernel_free(h);
}

uacpi_status
uacpi_kernel_acquire_mutex(uacpi_handle h, uacpi_u16 timeout)
{
	uint64_t end = timer_get_microseconds() + (uint64_t)timeout * 1000;

	do {
		if (nxu_spin_trylock(h)) {
			return UACPI_STATUS_OK;
		}

		cpu_relax();
	} while (timeout == 0xffff || timer_get_microseconds() < end);
	return UACPI_STATUS_TIMEOUT;
}

void
uacpi_kernel_release_mutex(uacpi_handle h)
{
	nxu_spin_unlock(h);
}

uacpi_handle
uacpi_kernel_create_spinlock(void)
{
	return uacpi_kernel_create_mutex();
}

void
uacpi_kernel_free_spinlock(uacpi_handle h)
{
	uacpi_kernel_free(h);
}

uacpi_cpu_flags
uacpi_kernel_lock_spinlock(uacpi_handle h)
{
	return nxu_spin_lock_irqsave(h);
}

void
uacpi_kernel_unlock_spinlock(uacpi_handle h, uacpi_cpu_flags f)
{
	nxu_spin_unlock_irqrestore(h, f);
}

uacpi_handle
uacpi_kernel_create_event(void)
{
	return kcalloc(1, sizeof(uint32_t));
}

void
uacpi_kernel_free_event(uacpi_handle h)
{
	uacpi_kernel_free(h);
}

void
uacpi_kernel_signal_event(uacpi_handle h)
{
	__atomic_add_fetch((uint32_t *)h, 1, __ATOMIC_RELEASE);
}

void
uacpi_kernel_reset_event(uacpi_handle h)
{
	__atomic_store_n((uint32_t *)h, 0, __ATOMIC_RELEASE);
}

uacpi_bool
uacpi_kernel_wait_for_event(uacpi_handle h, uacpi_u16 timeout)
{
	uint64_t end = timer_get_microseconds() + (uint64_t)timeout * 1000;

	do {
		uint32_t n = __atomic_load_n((uint32_t *)h, __ATOMIC_ACQUIRE);

		if (n && __atomic_compare_exchange_n((uint32_t *)h, &n, n - 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			return 1;
		}

		cpu_relax();
	} while (timeout == 0xffff || timer_get_microseconds() < end);
	return 0;
}

/* SCI and deferred work are deliberately unavailable during boot discovery. */
uacpi_status
uacpi_kernel_install_interrupt_handler(uacpi_u32 irq, uacpi_interrupt_handler fn, uacpi_handle ctx, uacpi_handle *out)
{
	(void)irq;
	(void)fn;
	(void)ctx;
	(void)out;
	return UACPI_STATUS_UNIMPLEMENTED;
}

uacpi_status
uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler fn, uacpi_handle h)
{
	(void)fn;
	(void)h;
	return UACPI_STATUS_UNIMPLEMENTED;
}

uacpi_status
uacpi_kernel_schedule_work(uacpi_work_type t, uacpi_work_handler fn, uacpi_handle ctx)
{
	(void)t;
	(void)fn;
	(void)ctx;
	return UACPI_STATUS_UNIMPLEMENTED;
}

uacpi_status
uacpi_kernel_wait_for_work_completion(void)
{
	return UACPI_STATUS_OK;
}

uacpi_status
uacpi_kernel_handle_firmware_request(uacpi_firmware_request *r)
{
	(void)r;
	return UACPI_STATUS_UNIMPLEMENTED;
}

typedef struct {
	uint32_t base;
	uint32_t length;
} io_range;

uacpi_status
uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size length, uacpi_handle *out)
{
	if (!length || base >= 65536 || length > 65536 - base) {
		return UACPI_STATUS_INVALID_ARGUMENT;
	}

	io_range *r = kmalloc(sizeof(*r));
	if (!r) {
		return UACPI_STATUS_OUT_OF_MEMORY;
	}

	*r = (io_range){base, length};
	*out = r;
	return UACPI_STATUS_OK;
}

void
uacpi_kernel_io_unmap(uacpi_handle h)
{
	uacpi_kernel_free(h);
}

#define IO_ACCESS(bits, suffix)                                                                                        \
	uacpi_status uacpi_kernel_io_read##bits(uacpi_handle h, uacpi_size off, uacpi_u##bits *v)                          \
	{                                                                                                                  \
		io_range *r = h;                                                                                               \
		if (off > r->length || bits / 8 > r->length - off)                                                             \
			return UACPI_STATUS_INVALID_ARGUMENT;                                                                      \
		uint16_t port = r->base + off;                                                                                 \
		__asm__ volatile("in" suffix " %1,%0" : "=a"(*v) : "Nd"(port) : "memory");                                     \
		return UACPI_STATUS_OK;                                                                                        \
	}                                                                                                                  \
	uacpi_status uacpi_kernel_io_write##bits(uacpi_handle h, uacpi_size off, uacpi_u##bits v)                          \
	{                                                                                                                  \
		io_range *r = h;                                                                                               \
		if (off > r->length || bits / 8 > r->length - off)                                                             \
			return UACPI_STATUS_INVALID_ARGUMENT;                                                                      \
		uint16_t port = r->base + off;                                                                                 \
		__asm__ volatile("out" suffix " %0,%1" ::"a"(v), "Nd"(port) : "memory");                                       \
		return UACPI_STATUS_OK;                                                                                        \
	}

IO_ACCESS(8, "b") IO_ACCESS(16, "w") IO_ACCESS(32, "l") static nxu_spinlock_t pci_lock;

uacpi_status
uacpi_kernel_pci_device_open(uacpi_pci_address a, uacpi_handle *out)
{
	if (a.segment || a.device > 31 || a.function > 7) {
		return UACPI_STATUS_NOT_FOUND;
	}

	uint32_t *p = kmalloc(sizeof(*p));

	if (!p) {
		return UACPI_STATUS_OUT_OF_MEMORY;
	}

	*p = 0x80000000U | ((uint32_t)a.bus << 16) | ((uint32_t)a.device << 11) | ((uint32_t)a.function << 8);
	*out = p;
	return UACPI_STATUS_OK;
}

void
uacpi_kernel_pci_device_close(uacpi_handle h)
{
	uacpi_kernel_free(h);
}

#define PCI_ACCESS(bits, suffix)                                                                                       \
	uacpi_status uacpi_kernel_pci_read##bits(uacpi_handle h, uacpi_size off, uacpi_u##bits *v)                         \
	{                                                                                                                  \
		if (off > 256 - bits / 8 || (off & (bits / 8 - 1)))                                                            \
			return UACPI_STATUS_INVALID_ARGUMENT;                                                                      \
		uint64_t flags = nxu_spin_lock_irqsave(&pci_lock);                                                             \
		uint32_t address = *(uint32_t *)h | (off & ~3U);                                                               \
		uint16_t port = 0xcf8;                                                                                         \
		__asm__ volatile("outl %0,%1" ::"a"(address), "Nd"(port) : "memory");                                          \
		port = 0xcfc + (off & 3);                                                                                      \
		__asm__ volatile("in" suffix " %1,%0" : "=a"(*v) : "Nd"(port) : "memory");                                     \
		nxu_spin_unlock_irqrestore(&pci_lock, flags);                                                                  \
		return UACPI_STATUS_OK;                                                                                        \
	}                                                                                                                  \
	uacpi_status uacpi_kernel_pci_write##bits(uacpi_handle h, uacpi_size off, uacpi_u##bits v)                         \
	{                                                                                                                  \
		if (off > 256 - bits / 8 || (off & (bits / 8 - 1)))                                                            \
			return UACPI_STATUS_INVALID_ARGUMENT;                                                                      \
		uint64_t flags = nxu_spin_lock_irqsave(&pci_lock);                                                             \
		uint32_t address = *(uint32_t *)h | (off & ~3U);                                                               \
		uint16_t port = 0xcf8;                                                                                         \
		__asm__ volatile("outl %0,%1" ::"a"(address), "Nd"(port) : "memory");                                          \
		port = 0xcfc + (off & 3);                                                                                      \
		__asm__ volatile("out" suffix " %0,%1" ::"a"(v), "Nd"(port) : "memory");                                       \
		nxu_spin_unlock_irqrestore(&pci_lock, flags);                                                                  \
		return UACPI_STATUS_OK;                                                                                        \
	}

PCI_ACCESS(8, "b") PCI_ACCESS(16, "w") PCI_ACCESS(32, "l")
