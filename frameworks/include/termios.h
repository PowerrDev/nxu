/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        frameworks/include/termios.h
 *
 * <termios.h>: terminal configuration, with the POSIX names.
 *
 * The types, flags and control-character slots are the kernel's ABI
 * (kern/syscall/termios_defs.h) under their usual names; `struct termios` is
 * laid out exactly like `struct nxu_termios` and is handed to the kernel as
 * it is. Values follow Darwin, so code written against macOS's <termios.h>
 * means the same thing here, with one difference in the types: every flag
 * word and speed is 32 bits wide (Darwin's are `unsigned long`).
 *
 * The functions behave as POSIX describes, returning 0 for success and -1
 * for failure. NXU has no C library yet and so no `errno`: a caller that
 * needs to know why a call failed makes the request with nxu_ioctl, which
 * returns -NXU_SYS_E_*. A descriptor that is not a terminal fails every
 * request with NXU_SYS_E_NOT_TTY; isatty() is that test.
 *
 * Not here yet: tcgetpgrp, tcsetpgrp, tcgetsid (job control) and
 * tcsendbreak (no driver can send a break).
 */
#ifndef NXU_USER_TERMIOS_H
#define NXU_USER_TERMIOS_H

#include <kern/syscall/termios_defs.h>

#include <stddef.h>
#include <stdint.h>

typedef nxu_tcflag_t tcflag_t;
typedef nxu_cc_t cc_t;
typedef nxu_speed_t speed_t;

#define NCCS NXU_NCCS

struct termios {
	tcflag_t c_iflag; /* input */
	tcflag_t c_oflag; /* output */
	tcflag_t c_cflag; /* the line */
	tcflag_t c_lflag; /* local: echo, canonical input, signals */
	cc_t c_cc[NCCS];  /* control characters */
	speed_t c_ispeed; /* bits per second */
	speed_t c_ospeed;
};

_Static_assert(sizeof(struct termios) == sizeof(struct nxu_termios), "struct termios is struct nxu_termios");
_Static_assert(offsetof(struct termios, c_cc) == offsetof(struct nxu_termios, c_cc), "struct termios layout");
_Static_assert(offsetof(struct termios, c_ospeed) == offsetof(struct nxu_termios, c_ospeed), "struct termios layout");

/* The size of the window behind a terminal (TIOCGWINSZ). */
struct winsize {
	unsigned short ws_row;
	unsigned short ws_col;
	unsigned short ws_xpixel;
	unsigned short ws_ypixel;
};

_Static_assert(sizeof(struct winsize) == sizeof(struct nxu_winsize), "struct winsize is struct nxu_winsize");

/* A c_cc entry that matches no input. */
#define _POSIX_VDISABLE NXU_POSIX_VDISABLE
#define CCEQ(value, c) ((c) == (value) && (value) != _POSIX_VDISABLE)

/* c_cc slots. */
#define VEOF NXU_VEOF
#define VEOL NXU_VEOL
#define VEOL2 NXU_VEOL2
#define VERASE NXU_VERASE
#define VWERASE NXU_VWERASE
#define VKILL NXU_VKILL
#define VREPRINT NXU_VREPRINT
#define VINTR NXU_VINTR
#define VQUIT NXU_VQUIT
#define VSUSP NXU_VSUSP
#define VDSUSP NXU_VDSUSP
#define VSTART NXU_VSTART
#define VSTOP NXU_VSTOP
#define VLNEXT NXU_VLNEXT
#define VDISCARD NXU_VDISCARD
#define VMIN NXU_VMIN
#define VTIME NXU_VTIME
#define VSTATUS NXU_VSTATUS

/* c_iflag. */
#define IGNBRK NXU_IGNBRK
#define BRKINT NXU_BRKINT
#define IGNPAR NXU_IGNPAR
#define PARMRK NXU_PARMRK
#define INPCK NXU_INPCK
#define ISTRIP NXU_ISTRIP
#define INLCR NXU_INLCR
#define IGNCR NXU_IGNCR
#define ICRNL NXU_ICRNL
#define IXON NXU_IXON
#define IXOFF NXU_IXOFF
#define IXANY NXU_IXANY
#define IMAXBEL NXU_IMAXBEL
#define IUTF8 NXU_IUTF8

/* c_oflag. */
#define OPOST NXU_OPOST
#define ONLCR NXU_ONLCR
#define OXTABS NXU_OXTABS
#define ONOEOT NXU_ONOEOT
#define OCRNL NXU_OCRNL
#define ONOCR NXU_ONOCR
#define ONLRET NXU_ONLRET

/* c_cflag. */
#define CSIZE NXU_CSIZE
#define CS5 NXU_CS5
#define CS6 NXU_CS6
#define CS7 NXU_CS7
#define CS8 NXU_CS8
#define CSTOPB NXU_CSTOPB
#define CREAD NXU_CREAD
#define PARENB NXU_PARENB
#define PARODD NXU_PARODD
#define HUPCL NXU_HUPCL
#define CLOCAL NXU_CLOCAL

/* c_lflag. */
#define ECHOKE NXU_ECHOKE
#define ECHOE NXU_ECHOE
#define ECHOK NXU_ECHOK
#define ECHO NXU_ECHO
#define ECHONL NXU_ECHONL
#define ECHOPRT NXU_ECHOPRT
#define ECHOCTL NXU_ECHOCTL
#define ISIG NXU_ISIG
#define ICANON NXU_ICANON
#define ALTWERASE NXU_ALTWERASE
#define IEXTEN NXU_IEXTEN
#define EXTPROC NXU_EXTPROC
#define TOSTOP NXU_TOSTOP
#define FLUSHO NXU_FLUSHO
#define NOKERNINFO NXU_NOKERNINFO
#define PENDIN NXU_PENDIN
#define NOFLSH NXU_NOFLSH

/* tcsetattr: when. TCSASOFT (or'ed in) leaves c_cflag and the speeds as they are. */
#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2
#define TCSASOFT 0x10

/* tcflush: which queue. */
#define TCIFLUSH 1
#define TCOFLUSH 2
#define TCIOFLUSH 3

/* tcflow: what. */
#define TCOOFF 1
#define TCOON 2
#define TCIOFF 3
#define TCION 4

/* Speeds are bits per second. */
#define B0 0U
#define B50 50U
#define B75 75U
#define B110 110U
#define B134 134U
#define B150 150U
#define B200 200U
#define B300 300U
#define B600 600U
#define B1200 1200U
#define B1800 1800U
#define B2400 2400U
#define B4800 4800U
#define B9600 9600U
#define B19200 19200U
#define B38400 38400U
#define B57600 57600U
#define B115200 115200U
#define B230400 230400U

int tcgetattr(int descriptor, struct termios *termios);
int tcsetattr(int descriptor, int when, const struct termios *termios);
int tcdrain(int descriptor);
int tcflow(int descriptor, int action);
int tcflush(int descriptor, int queue);
int tcgetwinsize(int descriptor, struct winsize *size);
int tcsetwinsize(int descriptor, const struct winsize *size);

speed_t cfgetispeed(const struct termios *termios);
speed_t cfgetospeed(const struct termios *termios);
int cfsetispeed(struct termios *termios, speed_t speed);
int cfsetospeed(struct termios *termios, speed_t speed);
int cfsetspeed(struct termios *termios, speed_t speed);
void cfmakeraw(struct termios *termios);

/* 1 when `descriptor` is a terminal, 0 when it is not (POSIX puts this in <unistd.h>). */
int isatty(int descriptor);

#endif
