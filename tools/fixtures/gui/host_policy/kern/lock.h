#include <stdint.h>

typedef struct {
	int value;
} nxu_spinlock_t;

static inline uint64_t nxu_spin_lock_irqsave(nxu_spinlock_t *p)
{
	(void)p;
	return 0;
}

static inline void nxu_spin_unlock_irqrestore(nxu_spinlock_t *p, uint64_t v)
{
	(void)p;
	(void)v;
}
