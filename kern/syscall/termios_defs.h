/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/syscall/termios_defs.h
 *
 * The terminal ABI shared by the kernel and user space: the configuration
 * (struct nxu_termios), the window size, and the ioctl requests on a
 * terminal. User space reaches it through <termios.h>
 * (frameworks/include/termios.h), which gives it the POSIX names.
 *
 * Every type here is fixed-width and laid out identically on every
 * architecture, so the same request number means the same argument
 * everywhere. Requests are NXU_IOC numbers (kern/syscall/syscall_defs.h);
 * terminal requests are in group 't', the generic descriptor queries in
 * group 'f', with the numbers Darwin uses for the same requests.
 *
 * A request a descriptor does not take fails with NXU_SYS_E_NOT_TTY; a
 * descriptor that is not a terminal at all answers every one of them that way.
 */

#ifndef NXU_KERN_SYSCALL_TERMIOS_DEFS_H
#define NXU_KERN_SYSCALL_TERMIOS_DEFS_H

#include <kern/syscall/syscall_defs.h>

#include <stdint.h>

#define NXU_TTY_IOC_GROUP 't'
#define NXU_FILE_IOC_GROUP 'f'

/* Bytes that can be read now without waiting (uint32_t). */
#define NXU_FIONREAD NXU_IOR(NXU_FILE_IOC_GROUP, 127U, uint32_t)

/* Bytes written but not yet taken by the terminal's driver (uint32_t). */
#define NXU_TIOCOUTQ NXU_IOR(NXU_TTY_IOC_GROUP, 115U, uint32_t)

/*
 * struct nxu_termios
 *
 * A terminal's configuration. The layout is Darwin's with every field fixed
 * at 32 bits (Darwin's are `unsigned long`): four flag words, the control
 * characters, two speeds. 44 bytes, no padding, the same on every
 * architecture. Bit values and control-character slots are Darwin's too, so
 * code written against <termios.h> on macOS means the same thing here.
 */
#define NXU_NCCS 20U

typedef uint32_t nxu_tcflag_t;
typedef uint8_t nxu_cc_t;
typedef uint32_t nxu_speed_t;

struct nxu_termios {
	nxu_tcflag_t c_iflag;
	nxu_tcflag_t c_oflag;
	nxu_tcflag_t c_cflag;
	nxu_tcflag_t c_lflag;
	nxu_cc_t c_cc[NXU_NCCS];
	nxu_speed_t c_ispeed;
	nxu_speed_t c_ospeed;
};

_Static_assert(sizeof(struct nxu_termios) == 44U, "struct nxu_termios is ABI: its layout must not change");

/* A control character set to this is disabled: no input byte matches it. */
#define NXU_POSIX_VDISABLE 0xFFU

/* c_cc slots. */
#define NXU_VEOF 0U
#define NXU_VEOL 1U
#define NXU_VEOL2 2U
#define NXU_VERASE 3U
#define NXU_VWERASE 4U
#define NXU_VKILL 5U
#define NXU_VREPRINT 6U
#define NXU_VINTR 8U
#define NXU_VQUIT 9U
#define NXU_VSUSP 10U
#define NXU_VDSUSP 11U
#define NXU_VSTART 12U
#define NXU_VSTOP 13U
#define NXU_VLNEXT 14U
#define NXU_VDISCARD 15U
#define NXU_VMIN 16U
#define NXU_VTIME 17U
#define NXU_VSTATUS 18U

/* c_iflag: input. */
#define NXU_IGNBRK 0x00000001U
#define NXU_BRKINT 0x00000002U
#define NXU_IGNPAR 0x00000004U
#define NXU_PARMRK 0x00000008U
#define NXU_INPCK 0x00000010U
#define NXU_ISTRIP 0x00000020U
#define NXU_INLCR 0x00000040U
#define NXU_IGNCR 0x00000080U
#define NXU_ICRNL 0x00000100U
#define NXU_IXON 0x00000200U
#define NXU_IXOFF 0x00000400U
#define NXU_IXANY 0x00000800U
#define NXU_IMAXBEL 0x00002000U
#define NXU_IUTF8 0x00004000U

/* c_oflag: output. */
#define NXU_OPOST 0x00000001U
#define NXU_ONLCR 0x00000002U
#define NXU_OXTABS 0x00000004U
#define NXU_ONOEOT 0x00000008U
#define NXU_OCRNL 0x00000010U
#define NXU_ONOCR 0x00000020U
#define NXU_ONLRET 0x00000040U

/* c_cflag: the line. */
#define NXU_CSIZE 0x00000300U
#define NXU_CS5 0x00000000U
#define NXU_CS6 0x00000100U
#define NXU_CS7 0x00000200U
#define NXU_CS8 0x00000300U
#define NXU_CSTOPB 0x00000400U
#define NXU_CREAD 0x00000800U
#define NXU_PARENB 0x00001000U
#define NXU_PARODD 0x00002000U
#define NXU_HUPCL 0x00004000U
#define NXU_CLOCAL 0x00008000U

/* c_lflag: local modes. FLUSHO and PENDIN are state the terminal sets. */
#define NXU_ECHOKE 0x00000001U
#define NXU_ECHOE 0x00000002U
#define NXU_ECHOK 0x00000004U
#define NXU_ECHO 0x00000008U
#define NXU_ECHONL 0x00000010U
#define NXU_ECHOPRT 0x00000020U
#define NXU_ECHOCTL 0x00000040U
#define NXU_ISIG 0x00000080U
#define NXU_ICANON 0x00000100U
#define NXU_ALTWERASE 0x00000200U
#define NXU_IEXTEN 0x00000400U
#define NXU_EXTPROC 0x00000800U
#define NXU_TOSTOP 0x00400000U
#define NXU_FLUSHO 0x00800000U
#define NXU_NOKERNINFO 0x02000000U
#define NXU_PENDIN 0x20000000U
#define NXU_NOFLSH 0x80000000U

/* Speeds are bits per second, as on Darwin. */
#define NXU_B0 0U
#define NXU_B1200 1200U
#define NXU_B2400 2400U
#define NXU_B4800 4800U
#define NXU_B9600 9600U
#define NXU_B19200 19200U
#define NXU_B38400 38400U
#define NXU_B57600 57600U
#define NXU_B115200 115200U

/*
 * What a terminal starts with (Darwin's <sys/ttydefaults.h>, plus IUTF8):
 * canonical input with echo and erase by rub-out, the signal characters, CR
 * to NL in and NL to CR-NL out, 8-bit characters. VSTATUS and VDSUSP are
 * disabled: there is no SIGINFO, and a delayed suspend is not implemented.
 */
#define NXU_TTYDEF_IFLAG (NXU_BRKINT | NXU_ICRNL | NXU_IMAXBEL | NXU_IXON | NXU_IXANY | NXU_IUTF8)
#define NXU_TTYDEF_OFLAG (NXU_OPOST | NXU_ONLCR)
#define NXU_TTYDEF_LFLAG (NXU_ECHO | NXU_ICANON | NXU_ISIG | NXU_IEXTEN | NXU_ECHOE | NXU_ECHOKE | NXU_ECHOCTL)
#define NXU_TTYDEF_CFLAG (NXU_CREAD | NXU_CS8 | NXU_HUPCL)
#define NXU_TTYDEF_SPEED NXU_B9600

#define NXU_CTRL(c) ((uint8_t)((c) & 0x1FU))

#define NXU_CEOF NXU_CTRL('d')
#define NXU_CEOL NXU_POSIX_VDISABLE
#define NXU_CERASE 0x7FU
#define NXU_CINTR NXU_CTRL('c')
#define NXU_CSTATUS NXU_POSIX_VDISABLE
#define NXU_CKILL NXU_CTRL('u')
#define NXU_CMIN 1U
#define NXU_CQUIT 0x1CU
#define NXU_CSUSP NXU_CTRL('z')
#define NXU_CTIME 0U
#define NXU_CDSUSP NXU_POSIX_VDISABLE
#define NXU_CSTART NXU_CTRL('q')
#define NXU_CSTOP NXU_CTRL('s')
#define NXU_CLNEXT NXU_CTRL('v')
#define NXU_CDISCARD NXU_CTRL('o')
#define NXU_CWERASE NXU_CTRL('w')
#define NXU_CREPRINT NXU_CTRL('r')
#define NXU_CEOL2 NXU_POSIX_VDISABLE

/*
 * struct nxu_winsize
 *
 * The size of the window behind a terminal, in characters and, if known,
 * pixels. The kernel only stores it; whoever draws the window (a terminal
 * emulator, through the master side of a pseudo-terminal) sets it, and
 * programs that lay out text read it. A change is reported to the
 * terminal's foreground process group with SIGWINCH.
 */
struct nxu_winsize {
	uint16_t ws_row;
	uint16_t ws_col;
	uint16_t ws_xpixel;
	uint16_t ws_ypixel;
};

_Static_assert(sizeof(struct nxu_winsize) == 8U, "struct nxu_winsize is ABI: its layout must not change");

/*
 * Terminal requests. Numbers and argument types are Darwin's.
 *
 * TIOCGETA / TIOCSETA
 *     Read and set the configuration. TIOCSETAW first waits until output
 *     has drained; TIOCSETAF waits, then discards unread input. The driver may
 *     refuse a configuration its line cannot do (NXU_SYS_E_INVALID_ARGUMENT),
 *     and the terminal is then unchanged.
 *
 * TIOCFLUSH
 *     Discard input (NXU_FREAD), output not yet taken by the driver
 *     (NXU_FWRITE), or both (either both bits or 0).
 *
 * TIOCDRAIN
 *     Wait until the driver has taken all output. Interruptible; returns at
 *     once on a hung-up terminal, whose output will never go anywhere.
 *
 * TIOCSTOP / TIOCSTART
 *     Stop and restart output, as VSTOP and VSTART do from the keyboard (and
 *     interchangeably with them).
 *
 * TIOCGWINSZ / TIOCSWINSZ
 *     Read and set the window size.
 */
#define NXU_TIOCGETA NXU_IOR(NXU_TTY_IOC_GROUP, 19U, struct nxu_termios)
#define NXU_TIOCSETA NXU_IOW(NXU_TTY_IOC_GROUP, 20U, struct nxu_termios)
#define NXU_TIOCSETAW NXU_IOW(NXU_TTY_IOC_GROUP, 21U, struct nxu_termios)
#define NXU_TIOCSETAF NXU_IOW(NXU_TTY_IOC_GROUP, 22U, struct nxu_termios)
#define NXU_TIOCFLUSH NXU_IOW(NXU_TTY_IOC_GROUP, 16U, uint32_t)
#define NXU_TIOCDRAIN NXU_IO(NXU_TTY_IOC_GROUP, 94U)
#define NXU_TIOCSTOP NXU_IO(NXU_TTY_IOC_GROUP, 111U)
#define NXU_TIOCSTART NXU_IO(NXU_TTY_IOC_GROUP, 110U)
#define NXU_TIOCGWINSZ NXU_IOR(NXU_TTY_IOC_GROUP, 104U, struct nxu_winsize)
#define NXU_TIOCSWINSZ NXU_IOW(NXU_TTY_IOC_GROUP, 103U, struct nxu_winsize)

/*
 * Job control (Darwin's numbers; the argument is an int32_t process group or
 * session ID). TIOCGPGRP, TIOCSPGRP, TIOCGSID and TIOCNOTTY apply only to the
 * caller's controlling terminal (NXU_SYS_E_NOT_TTY otherwise).
 *
 * TIOCSCTTY
 *     Make this terminal the caller's controlling terminal. The caller leads
 *     its session, the session has none, and no other session has this one
 *     (NXU_SYS_E_DENIED otherwise).
 *
 * TIOCNOTTY
 *     Give the controlling terminal up. For the session leader this ends the
 *     session's hold on it, and its foreground group is sent SIGHUP and
 *     SIGCONT; for anyone else it does nothing.
 *
 * TIOCGPGRP / TIOCSPGRP
 *     The foreground process group. A new one must be a group of the caller's
 *     session (NXU_SYS_E_DENIED otherwise). With no foreground group,
 *     TIOCGPGRP reports an ID no group has.
 *
 * TIOCGSID
 *     The session the terminal controls.
 */
#define NXU_TIOCSCTTY NXU_IO(NXU_TTY_IOC_GROUP, 97U)
#define NXU_TIOCNOTTY NXU_IO(NXU_TTY_IOC_GROUP, 113U)
#define NXU_TIOCGPGRP NXU_IOR(NXU_TTY_IOC_GROUP, 119U, int32_t)
#define NXU_TIOCSPGRP NXU_IOW(NXU_TTY_IOC_GROUP, 118U, int32_t)
#define NXU_TIOCGSID NXU_IOR(NXU_TTY_IOC_GROUP, 99U, int32_t)

/*
 * Pseudo-terminal master requests (Darwin's numbers), on a descriptor opened
 * from /dev/ptmx. They are what posix_openpt's companions are made of.
 *
 * TIOCPTYGNAME
 *     The path of the slave ("/dev/ttys000"), NUL-terminated (ptsname).
 *
 * TIOCPTYGRANT
 *     Give the slave to the caller's real user, group tty, mode 0620
 *     (grantpt).
 *
 * TIOCPTYUNLK
 *     Allow the slave to be opened (unlockpt). Until then opening it fails
 *     with NXU_SYS_E_DENIED.
 *
 * The master also takes TIOCGETA, TIOCSETA, TIOCSETAW, TIOCSETAF, TIOCFLUSH,
 * TIOCGWINSZ and TIOCSWINSZ, which act on the slave's terminal, and
 * FIONREAD, which counts output the slave has written and the master not yet
 * read.
 */
#define NXU_PTY_NAME_MAX 128U

typedef struct {
	char name[NXU_PTY_NAME_MAX];
} nxu_ptyname_t;

#define NXU_TIOCPTYGNAME NXU_IOR(NXU_TTY_IOC_GROUP, 83U, nxu_ptyname_t)
#define NXU_TIOCPTYGRANT NXU_IO(NXU_TTY_IOC_GROUP, 84U)
#define NXU_TIOCPTYUNLK NXU_IO(NXU_TTY_IOC_GROUP, 82U)

/* TIOCFLUSH: which queues. */
#define NXU_FREAD 0x1U
#define NXU_FWRITE 0x2U

/*
 * The ABI, checked wherever this header is compiled: both kernels and both
 * userlands, arm64 and i386, so a layout or a request number that came out
 * differently on one of them fails its build. Field offsets, sizes, and the
 * requests that encode an argument's size.
 */
_Static_assert(__builtin_offsetof(struct nxu_termios, c_iflag) == 0U, "termios ABI: c_iflag");
_Static_assert(__builtin_offsetof(struct nxu_termios, c_oflag) == 4U, "termios ABI: c_oflag");
_Static_assert(__builtin_offsetof(struct nxu_termios, c_cflag) == 8U, "termios ABI: c_cflag");
_Static_assert(__builtin_offsetof(struct nxu_termios, c_lflag) == 12U, "termios ABI: c_lflag");
_Static_assert(__builtin_offsetof(struct nxu_termios, c_cc) == 16U, "termios ABI: c_cc");
_Static_assert(__builtin_offsetof(struct nxu_termios, c_ispeed) == 36U, "termios ABI: c_ispeed");
_Static_assert(__builtin_offsetof(struct nxu_termios, c_ospeed) == 40U, "termios ABI: c_ospeed");
_Static_assert(__builtin_offsetof(struct nxu_winsize, ws_col) == 2U, "winsize ABI: ws_col");
_Static_assert(__builtin_offsetof(struct nxu_winsize, ws_ypixel) == 6U, "winsize ABI: ws_ypixel");
_Static_assert(sizeof(nxu_ptyname_t) == 128U, "TIOCPTYGNAME ABI: the name buffer");
_Static_assert(NXU_TIOCGETA == 0x802c7413U && NXU_TIOCSETA == 0x402c7414U && NXU_TIOCSETAW == 0x402c7415U &&
				   NXU_TIOCSETAF == 0x402c7416U,
			   "termios ABI: the TIOCGETA family's request numbers");
_Static_assert(NXU_TIOCGWINSZ == 0x80087468U && NXU_TIOCSWINSZ == 0x40087467U, "winsize ABI: request numbers");
_Static_assert(NXU_TIOCPTYGNAME == 0x80807453U && NXU_FIONREAD == 0x8004667fU, "terminal ABI: request numbers");

#endif
