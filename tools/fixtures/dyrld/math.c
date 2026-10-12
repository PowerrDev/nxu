extern int nxu_leaf_value(void);
extern void nxu_leaf_crash(void);
static int result, initializations;

__attribute__((constructor)) static void
initialize(void)
{
	result = nxu_leaf_value() * 6;
	initializations++;
}

int
nxu_math_result(void)
{
	return result;
}

int
nxu_math_initializations(void)
{
	return initializations;
}

__attribute__((noinline)) void
nxu_math_crash(void)
{
	nxu_leaf_crash();
	result++;
}
