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
