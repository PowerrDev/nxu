#ifndef LEAF_VALUE
#define LEAF_VALUE 7
#endif
int nxu_leaf_value(void)
{
	return LEAF_VALUE;
}

__attribute__((noinline)) void nxu_leaf_crash(void)
{
	*(volatile unsigned long *)0x12345000UL = 1;
}
