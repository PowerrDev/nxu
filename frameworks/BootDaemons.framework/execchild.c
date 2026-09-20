#include <nxu/syscall.h>

#include <stdint.h>

/*
 * Target of the exec test in proctest.c. It runs only after exec replaced
 * the forked child: it checks that argc and argv arrived intact and exits
 * with a status only a correctly executed image can produce.
 */

#define EXECCHILD_OK_STATUS 31

int
main(int argc, char **argv)
{
	if (argc != 3) return 1;
	if (argv == 0 || argv[0] == 0 || argv[1] == 0 || argv[2] == 0 || argv[3] != 0) return 2;

	if (argv[1][0] != 'a' || argv[1][1] != 'l' || argv[1][2] != 'p' || argv[1][3] != 'h' || argv[1][4] != 'a' || argv[1][5] != '\0') return 3;
	if (argv[2][0] != 'b' || argv[2][1] != 'e' || argv[2][2] != 't' || argv[2][3] != 'a' || argv[2][4] != '\0') return 4;

	/* A fresh image has a fresh stack and a zero-filled bss. */
	static volatile uint64_t zeroed;
	if (zeroed != 0ULL) return 5;

	return EXECCHILD_OK_STATUS;
}
