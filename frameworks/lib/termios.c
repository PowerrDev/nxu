/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        frameworks/lib/termios.c
 *
 * The <termios.h> functions (frameworks/include/termios.h).
 *
 * Each is one terminal request (kern/syscall/termios_defs.h) or a few,
 * made with nxu_ioctl; the kernel does the work and the checking. What
 * happens here is only the translation from POSIX's names and argument
 * conventions to the kernel's, and the two pieces POSIX defines in terms of
 * other calls: TCSASOFT, and tcflow's TCIOFF and TCION, which send the stop
 * and start characters down the line. The pseudo-terminal calls are here too,
 * being requests on a terminal's other end.
 */
#include <termios.h>

#include <nxu/syscall.h>

#include <stdbool.h>
#include <stdint.h>

/* One request; 0 or -1, as POSIX wants. */
static int
termios_request(int descriptor, uint32_t command, void *argument)
{
	if (descriptor < 0) return -1;

	return nxu_ioctl((uint64_t)descriptor, command, argument) == 0 ? 0 : -1;
}

int
tcgetattr(int descriptor, struct termios *termios)
{
	if (termios == 0) return -1;

	return termios_request(descriptor, NXU_TIOCGETA, termios);
}

int
tcsetattr(int descriptor, int when, const struct termios *termios)
{
	if (termios == 0) return -1;

	struct termios wanted = *termios;

	/* TCSASOFT: the line itself (c_cflag, the speeds) stays as it is. */
	if ((when & TCSASOFT) != 0) {
		struct termios current;

		if (tcgetattr(descriptor, &current) != 0) return -1;

		wanted.c_cflag = current.c_cflag;
		wanted.c_ispeed = current.c_ispeed;
		wanted.c_ospeed = current.c_ospeed;
		when &= ~TCSASOFT;
	}

	switch (when) {
	case TCSANOW:
		return termios_request(descriptor, NXU_TIOCSETA, &wanted);
	case TCSADRAIN:
		return termios_request(descriptor, NXU_TIOCSETAW, &wanted);
	case TCSAFLUSH:
		return termios_request(descriptor, NXU_TIOCSETAF, &wanted);
	default:
		return -1;
	}
}

int
tcdrain(int descriptor)
{
	return termios_request(descriptor, NXU_TIOCDRAIN, 0);
}

int
tcflush(int descriptor, int queue)
{
	uint32_t which;

	switch (queue) {
	case TCIFLUSH:
		which = NXU_FREAD;
		break;
	case TCOFLUSH:
		which = NXU_FWRITE;
		break;
	case TCIOFLUSH:
		which = NXU_FREAD | NXU_FWRITE;
		break;
	default:
		return -1;
	}

	return termios_request(descriptor, NXU_TIOCFLUSH, &which);
}

/* TCIOFF and TCION: send the terminal's stop or start character, if it has one. */
static int
termios_send_control(int descriptor, uint32_t slot)
{
	struct termios termios;

	if (tcgetattr(descriptor, &termios) != 0) return -1;

	cc_t c = termios.c_cc[slot];

	if (c == _POSIX_VDISABLE) return 0;

	return nxu_write((uint64_t)descriptor, &c, 1U) == 1 ? 0 : -1;
}

int
tcflow(int descriptor, int action)
{
	switch (action) {
	case TCOOFF:
		return termios_request(descriptor, NXU_TIOCSTOP, 0);
	case TCOON:
		return termios_request(descriptor, NXU_TIOCSTART, 0);
	case TCIOFF:
		return termios_send_control(descriptor, VSTOP);
	case TCION:
		return termios_send_control(descriptor, VSTART);
	default:
		return -1;
	}
}

int
tcgetwinsize(int descriptor, struct winsize *size)
{
	if (size == 0) return -1;

	return termios_request(descriptor, NXU_TIOCGWINSZ, size);
}

int
tcsetwinsize(int descriptor, const struct winsize *size)
{
	if (size == 0) return -1;

	struct winsize copy = *size;

	return termios_request(descriptor, NXU_TIOCSWINSZ, &copy);
}

speed_t
cfgetispeed(const struct termios *termios)
{
	return termios->c_ispeed;
}

speed_t
cfgetospeed(const struct termios *termios)
{
	return termios->c_ospeed;
}

int
cfsetispeed(struct termios *termios, speed_t speed)
{
	termios->c_ispeed = speed;
	return 0;
}

int
cfsetospeed(struct termios *termios, speed_t speed)
{
	termios->c_ospeed = speed;
	return 0;
}

int
cfsetspeed(struct termios *termios, speed_t speed)
{
	termios->c_ispeed = speed;
	termios->c_ospeed = speed;
	return 0;
}

/* Darwin's cfmakeraw: bytes pass unchanged both ways, a read returns as soon as one is there. */
void
cfmakeraw(struct termios *termios)
{
	termios->c_iflag &= ~(IMAXBEL | IXOFF | INPCK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON | IGNPAR);
	termios->c_iflag |= IGNBRK;
	termios->c_oflag &= ~OPOST;
	termios->c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHONL | ICANON | ISIG | IEXTEN | NOFLSH | TOSTOP | PENDIN);
	termios->c_cflag &= ~(CSIZE | PARENB);
	termios->c_cflag |= CS8 | CREAD;
	termios->c_cc[VMIN] = 1;
	termios->c_cc[VTIME] = 0;
}

int
tcgetpgrp(int descriptor)
{
	int32_t pgid = -1;

	return termios_request(descriptor, NXU_TIOCGPGRP, &pgid) == 0 ? (int)pgid : -1;
}

int
tcsetpgrp(int descriptor, int pgid)
{
	int32_t value = pgid;

	return termios_request(descriptor, NXU_TIOCSPGRP, &value);
}

int
tcgetsid(int descriptor)
{
	int32_t sid = -1;

	return termios_request(descriptor, NXU_TIOCGSID, &sid) == 0 ? (int)sid : -1;
}

int
isatty(int descriptor)
{
	struct termios termios;

	return tcgetattr(descriptor, &termios) == 0 ? 1 : 0;
}

int
posix_openpt(int flags)
{
	int64_t master = nxu_open("/dev/ptmx", (uint64_t)(uint32_t)flags);

	return master < 0 ? -1 : (int)master;
}

int
grantpt(int master)
{
	return termios_request(master, NXU_TIOCPTYGRANT, 0);
}

int
unlockpt(int master)
{
	return termios_request(master, NXU_TIOCPTYUNLK, 0);
}

int
ptsname_r(int master, char *buffer, size_t size)
{
	nxu_ptyname_t name;

	if (buffer == 0 || termios_request(master, NXU_TIOCPTYGNAME, &name) != 0) return -1;

	size_t length = 0;

	while (length < sizeof(name.name) && name.name[length] != '\0')
		length++;

	if (length == sizeof(name.name) || length >= size) return -1;

	for (size_t index = 0; index <= length; index++)
		buffer[index] = name.name[index];

	return 0;
}
