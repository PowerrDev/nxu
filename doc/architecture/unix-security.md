# NXU Unix Security Architecture

Status: **in progress** — Phase 1 (kernel process credentials) landing on branch
`worktree-unix-creds-sudo`. This document is the roadmap for giving NXU a native,
Darwin-inspired multi-user Unix security model, culminating in `sudo`.

## 1. Goal and guiding principle

NXU today has **no user model**. Its whole security model is a per-process
capability bitmask (`p_caps`, `NXU_CAP_*`), default-deny, granted only downward
from PID 1, plus an interactive device-passcode gate (SAP) verified by a separate
security processor (TEP). There are no UIDs, no accounts, no file ownership, no
`setuid`, no password database.

The objective is a coherent, **secure-by-design multi-user Unix model** in which:

- the **kernel** owns and enforces process credentials and permissions;
- **userspace services** manage accounts and perform authentication;
- privileged operations are **explicitly authorized**, with authentication kept
  separate from authorization;
- everything works on **ordinary hardware** with a **software-backed**
  implementation — no custom security processor required.

> **Architectural principle: NXU must be secure by design. TEP is an optional
> experiment, not a security prerequisite.**

The capability model is **not** discarded. Credentials and capabilities coexist:
UID/GID express *identity and ownership*; capabilities express *what a process may
do to the machine*. The bridge (§4) is that `euid == 0` (root) implies the full
capability set. This keeps every existing `NXU_CAP_*` check working while adding
Unix identity on top.

## 2. Reference: Darwin / XNU

We model the kernel layer on XNU's BSD credential subsystem rather than copying
individual macOS commands. Key references
(https://github.com/apple-oss-distributions/xnu):

| Concern | XNU | NXU equivalent |
|---|---|---|
| Process credential record | `bsd/sys/ucred.h` (`struct ucred`, ref-counted) | `kern/security/ucred.h` (`struct ucred`, embedded in `struct proc` for now) |
| Credential ops / transitions | `bsd/kern/kern_cred.c`, `kern/kern_prot.c` | `kern/security/ucred.c`, syscalls in `kern/syscall/syscall.c` |
| setuid/setgid on exec | `bsd/kern/kern_exec.c` | loader (Phase 4) |
| Authorization framework | Kauth, Authorization Services | `authd` userspace service (Phase 5) |
| Privileged helpers | launchd privileged helpers | privileged services + `sudo` (Phase 7) |

Deviations from XNU, chosen deliberately for a first cut:

- **Embedded, copy-semantics `ucred`** in `struct proc` instead of a ref-counted
  shared `kauth_cred_t`. Simpler; revisit if credential sharing/mutation cost
  matters.
- **Capabilities retained** as the machine-mutation gate, bridged from `euid`.

## 3. Authentication vs. authorization

These stay separate, as in Authorization Services:

- **Authentication** ("who are you") is a userspace responsibility: a service
  verifies a password against a stored hash. Password hashes use **Argon2id**
  with unique per-record salts and tuned parameters. Hashes are **never** stored
  in plaintext and never stored *only* inside TEP.
- **Authorization** ("may you do this") is enforced by the kernel via process
  credentials and permission checks, independent of who authenticated.

Identifying a user does not by itself grant any privileged operation.

## 4. The credential ⇄ capability bridge

- `struct ucred` carries real/effective/saved UID and GID plus supplementary
  groups.
- A process is **privileged** iff its effective UID is `0` (`NXU_UID_ROOT`).
- A privileged process is treated as holding `NXU_CAP_ALL`. Non-root processes
  keep exactly the capabilities granted to them (unchanged from today).
- This is implemented centrally so existing `proc_has_caps()` callers need no
  change: root passes every capability check; non-root behaves as before.

Enforcement bridges land incrementally: identity in Phase 1–2, filesystem
ownership/permissions in Phase 3, setuid-exec in Phase 4.

## 5. TEP: demotion to optional experiment

TEP was an experiment in isolated/hardware-backed execution. It is **no longer a
required component**.

- TEP is **disabled by default**; it initializes only when explicitly enabled by
  an experimental build/config option *and* compatible hardware is present.
- If TEP is absent, fails to init, or is disabled, NXU **operates normally**.
- **Nothing mandatory** — accounts, password storage/auth, process credentials,
  privilege escalation, filesystem permissions, or boot — may depend on TEP.
- A **security-provider abstraction** lets optional hardware-backed crypto/key
  protection plug in without coupling ordinary operations to TEP. The
  **software-backed provider is always available and sufficient**.
- Operations that *explicitly require* hardware-backed guarantees must **report**
  unavailability rather than silently downgrade.
- SAP (the current destructive-op gate) is re-based onto the software auth path
  (authenticate against the account database) so it no longer requires TEP.
- Long term, TEP stays isolated, documented, independently buildable, and may be
  removed entirely if its upkeep outweighs its benefit.

## 6. Phased roadmap

Each phase is a self-contained, compiling, testable increment.

- **Phase 1 — Kernel process credentials.** `struct ucred` in `kern/security/`,
  embedded in `struct proc`. Root (`0/0`) for the kernel task and PID 1;
  inheritance across `proc_create_user` and `proc_fork`; `exec` preserves creds.
  Accessors (`proc_ucred`, `proc_is_root`). The credential⇄capability bridge.
  **← this session.**
- **Phase 2 — Identity syscalls + tools.** `getuid/geteuid/getgid/getegid/
  getgroups` and `setuid/seteuid/setgid/setegid/setgroups` with POSIX transition
  rules enforced by the kernel. Userland wrappers. Native `id`, `whoami`,
  `groups`. **← started this session (read side + wrappers).**
- **Phase 3 — Filesystem ownership & permissions.** Inode UID/GID/mode in the VFS
  (ext4 and btrfs already carry the fields); permission checks on
  open/exec/unlink/mkdir/etc.; `chown`/`chmod` syscalls; `umask`.
- **Phase 4 — setuid/setgid exec.** Loader honors `S_ISUID`/`S_ISGID`, setting
  effective and saved IDs from the file owner (the `kern_exec.c` analog). This is
  the mechanism `sudo` relies on.
- **Phase 5 — Account database & auth service.** `/etc/passwd`, `/etc/group`,
  `/etc/shadow` (Argon2id). `authd` userspace authentication service (separate
  from authorization). Native `login`, `passwd`, `su`.
- **Phase 6 — libc expansion.** Enough POSIX for the above and for `sudo`: stdio,
  `getenv`/`environ`, `termios`/`isatty`, the `str*`/`printf` family, `pwd.h`,
  `grp.h`. Grown from the existing `libnxurt`.
- **Phase 7 — sudo.** Native `sudo` + `/etc/sudoers` (start with a minimal
  parser; optionally port upstream sudo-project/sudo once the substrate and libc
  shim are complete). Uses Phase 4 setuid-exec and the Phase 5 auth service.
- **Phase 8 — TEP demotion.** Security-provider abstraction; re-base SAP/keystore
  onto the software provider; make TEP opt-in and off by default; ensure no
  mandatory dependency remains.

## 7. Phase 1 as-built (this session)

- `kern/security/ucred.h` — `struct ucred`, `NXU_UID_ROOT`/`NXU_GID_WHEEL`/
  `NXU_UID_NOBODY`, `NXU_NGROUPS_MAX`, init/copy/transition helpers.
- `struct proc` gains `struct ucred p_ucred;`.
- Kernel task and PID 1 initialize to root; other processes and `fork` inherit the
  parent's credentials.
- Syscalls `NXU_SYS_GETUID`…`NXU_SYS_SETGROUPS` (89–98) with userland wrappers;
  the read side proves the layer end-to-end.
- The credential⇄capability bridge in `proc_has_caps`.

## 8. login and getty (2026-11-14)

The native `login` of Phase 5, and the `getty` that runs it on the serial
console (`/dev/console`, see `doc/architecture/tty-subsystem.md`, "Phase 7 as
built", for the terminal itself).

- `getty` (`frameworks/BootDaemons.framework/getty.c`, bootd service
  `com.nxu.getty`, KeepAlive, root; `-no-getty` turns it off). It leads a new
  session with the terminal as controlling terminal and descriptors 0-2, and
  for ever: gives the terminal back to root (owner 0:0, mode 0600, default
  termios, typeahead discarded, its own group in the foreground), shows
  "groveOS (console)" (with the first word of `/etc/hostname` if there is one)
  and `login: `, reads a name, runs `login -t <terminal> <name>` and waits.
  With no exec on i386 it stays as the session leader instead of becoming
  login. It ignores ^C, ^\, ^Z and SIGHUP; the foreground group it shares
  with login gets them.
- `login [-t terminal] [name]` (`login.c`, setuid root, 4755). Everyone
  authenticates, root included. A name that does not exist is asked for a
  password and fails like a wrong one; after a second, "Login incorrect", and
  the name is asked again; three failures end it. On success the terminal
  becomes the user's (owner uid, group tty 4, mode 0620), then
  `setgroups(1, gid)`, `setgid`, `setuid`, and the account's shell runs on the
  terminal (`nxu_spawn_here`) while login waits and ends as it does. ^C or ^\
  at a prompt ends login; ^Z is ignored.
- A user's shell has the capabilities the service was given
  (`AllowFilesystemWrite`), like a shell in Terminal.app.

Test: `tools/console_login.script` against a normal boot, typed by
`tools/serial_expect.pl` (i386 `test-i386-boot` case `console-login`, arm64
`make check` row `console-login`): a wrong password refused, `user` logged in
(`whoami`, `/dev/console` 0620 owned by 1000:4), logout and a new prompt, root
the same way, ^C at the password prompt bringing the prompt back.

Added since (2026-11-14):

- **Records** (`nxu/utmp.h`, `frameworks/lib/utmp.c`): BSD's utmp
  (`/var/run/utmp`, one record per terminal, the session on it now) and
  wtmp (`/var/log/wtmp`, every login and logout, appended), records of one
  fixed 144-byte layout, times in UTC. login writes the login while still
  root; getty writes the logout after the session, and at its start clears
  a session the machine's last run left in utmp (`last` then calls it "gone
  - no logout").
- **`last [user]`** and **`who`**, in BSD's format, times in local time
  from timed (UTC without it).
- **Last login and the message of the day**: login prints "Last login:
  <time> on <terminal>" from wtmp (before recording the new login), then
  `/etc/motd`; a `.hushlogin` in the home directory silences both.
- **Revoke at logout**: getty revokes the console after every session
  (`nxu_revoke`, see the TTY design doc, "Phase 8 as built"), so a job the
  user left running keeps no working descriptor on it and cannot read the
  next user's typing; then opens it again for the next login.
- **Home directory**: login changes to it as the user, or says "No home
  directory" and uses `/`. The homes in `/etc/passwd` now carry the system
  volume's prefix (`/Volumes/mainOS/Users/user`, `/Volumes/mainOS/var/root`,
  mode 0700), as the shell field already did; both disks create them and
  give user's to user.

Not done: supplementary groups from `/etc/group` (only the primary group is
set); boot records in wtmp and a `lastlog`; the shell is not started as a
login shell (`-nzh`); nzh's prompt reads `/etc/username`, not the account.
