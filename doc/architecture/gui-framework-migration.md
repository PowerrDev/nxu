# GUI framework migration: boundary and host ABI

Status: dynamic WindowServer, CoreGraphics and DesktopService builds, 2026-10-07.
Host ABI 1.1 adds copied desktop session records to syscall 75. CoreGraphics
contains drawing and compositor client helpers; DesktopService contains desktop
and app-session policy. Final ELF audits pass without kernel bridge imports.
The normal desktop boot retains the kernel renderer while integration gates remain.

## Ownership boundary

| Component | Owns | Must not own |
| --- | --- | --- |
| Kernel | GPU/input drivers, VM, SHM rights, process identity, bounded message transport, display lease, session epoch, trusted input exclusion, SAP and login authentication | Desktop layout, ordinary window management, application drawing |
| WindowServer service | Composition, window surfaces, damage, cursor, ordering and presentation through its display lease | Raw device mappings, authentication decisions, unrestricted process termination |
| Desktop service | Menu bar, Dock/session integration, application launch requests, activation and desktop policy | Scanout ownership, credentials, kernel pointers |
| CoreGraphics.framework | Drawing, fonts, images, geometry and client surface helpers | Login, desktop/session policy or a daemon event loop |
| Runtime.framework | User allocator, syscall adapters, abort/panic termination, memory primitives | Kernel heap references or an infinite panic spin |
| CoreFoundation.framework | Plists, service discovery and other process-local foundation utilities | Display/input ownership |

CoreGraphics is extracted from UIService by responsibility. A global name
replacement would mislabel desktop and login code. Keep compatibility aliases
until consumers move. Framework C callbacks may remain inside a single process;
no callback, context pointer or borrowed kernel framebuffer crosses IPC.

The end state has exactly one ordinary scanout owner: WindowServer. Desktop
surfaces reach it through IPC. The first WindowServer shared-library milestone
may keep the existing desktop in the kernel, but cannot start a second service
that claims the same display at the same time. Select one compositor path per
boot while migrating.

Login and SAP stay trusted. Moving the ordinary desktop must not remove their
exclusive input/presentation protection. Before removing the kernel UIService
archive, extract a small trusted login renderer or provide a separately designed
trusted authentication agent. The current login depends on UIServiceLogin's
Rust renderer; simply unlinking the archive would break login. SAP's secure
pointer path also references the current UIServiceContext and must be replaced
by a kernel-owned input cursor state. Ordinary desktop IPC never carries a
password or an operation that sets the enclave passcode.

## Audited callback replacements

| Current interface | Process-boundary replacement |
| --- | --- |
| UIServiceGetSurface / borrowed pixels | Opaque surface handle and process-local SHM mapping; no scanout mapping |
| UIServicePresent / WSDisplay.present | Owner-checked PRESENT of validated damage and surface sequence |
| UIServicePollEvent / recovery input | Waitable normalized input queue scoped to display lease; no raw password events |
| UIServiceGetTime | Existing uptime and wall-clock syscall adapters |
| UIServiceListDirectory | Existing open/readdir/stat adapter; copied bounded records |
| get_activity | Existing statistics syscall adapter; copied bounded snapshots |
| ui_bridge_session_begin | Authenticated SESSION_BEGIN after trusted login; kernel supplies epoch |
| ui_bridge_accept/info/pid/alive | ACCEPT/DESCRIBE copied connection records with generation and process identity |
| ui_bridge_state/take_frame | FRAME_ACQUIRE/RELEASE of immutable submitted snapshot via SHM handle |
| ui_bridge_post/release | EVENT_POST/CONNECTION_CLOSE with ownership checks |
| ui_bridge_take_launch/activation | Bounded LAUNCH_TAKE/ACTIVATION_TAKE queues; bootd launches and applies capability policy |
| ui_bridge_terminate | Graceful close followed by kernel-controlled timeout policy; no arbitrary PID kill request |
| UIService's direct WS_* calls | Desktop-to-WindowServer IPC adapter; local WS_* C ABI remains inside compositor process |
| kmalloc/kfree | Process-local Runtime allocator preserving Rust Layout alignment and allocation/free pairing |

Implementation sources audited: platform/arm64/services/ui_service.c,
drivers/video/ui_service_login.c, drivers/video/ui_service_bridge.[ch],
frameworks/BootDaemons.framework/windowserver_service.c, sibling
UIService.framework/include/UIService.h and crates/ui-service-nxu/src/{bridge,
windowserver,lib}.rs, WindowServer.framework/include/WindowServer/{WSPrivate,
WSDIsplay}.h and crates/windowserver-nxu/src/runtime.rs.

## Versioned host protocol v1.0

`frameworks/include/nxu/gui_host_protocol.h` declares initial control envelopes,
HELLO request/reply and surface descriptors. It does not replace the callback
ABI v5 or enable a new service. Syscall 75 (`nxu_gui_host`) implements HELLO,
DISPLAY_ATTACH and RELEASE. Remaining operation
payloads must be specified alongside their dispatcher implementation; the enum
alone does not authorize a caller. Do not send these records through an
existing interface that does not advertise this protocol.

The transport will be a narrowly scoped kernel host endpoint with waitable
requests, bounded control records and separately transferred SHM rights.
Kernel authentication uses the caller's proc_ident (PID plus unique identity),
not any client-supplied role, PID or feature mask. Only bootd-authorized owners
may attach the display or begin a session. Feature bits advertise implemented
operations; they do not grant permissions. The current endpoint advertises only
the LEASE feature. Surface, input and session operations remain reserved. While
the legacy kernel desktop/login renderer is running, the endpoint returns BUSY.
It participates in the SAP untrusted-operation gate.

HELLO needs a 56-byte buffer; ATTACH/RELEASE carry a 32-byte request and need
48 bytes of capacity for the reply. Successful RELEASE clears the epoch. Exit
and successful exec revoke ownership and negotiation state; fork does not
inherit them. A failed reply copy revokes the caller's state. Ownership uses
PID, unique ID and identity version, preventing PID reuse from inheriting it.

Negotiation rules:

1. HELLO is first, with epoch zero, flags/reserved zero, a nonzero request ID,
   and the exact v1.0 request size. Major mismatch fails before any side effect.
2. Select the minimum minor version; enable the intersection of supported
   features. Reject when any required feature is absent from that intersection
   or is absent from the caller's own supported set.
3. Cap control record bytes to the smaller advertised limit and 4096. Reject
   limits too small for the fixed v1 records. Reply status is zero or a negative
   NXU_SYS_E_* code; rejection returns no handle and does not acquire a lease.
4. Kernel generates a nonzero epoch and returns it on successful ownership
   establishment. Every later request checks that epoch, identity, operation,
   negotiated size and reserved bits. Reply echoes the request ID. HELLO alone
   does not create an unlocked login session.
5. v1.0 sizes are exact. Future minor extensions require negotiated payload
   layouts; receivers never silently accept unknown trailing bytes. An
   incompatible layout or semantics increments the major version.

All wire fields have fixed-width integer types and explicit padding. Records
are little endian. C bool, size_t, Rust native enum layout, raw pointers and
function pointers are prohibited. C layout assertions cover ARM64 and i386;
Rust mirrors need repr(C) and matching size/offset assertions before dispatch.
The migration targets ARM64 first; layout compatibility is not i386 runtime
support.

## Surfaces, input and lifecycle invariants

Surface handles are generation tagged and scoped to one owner and epoch.
Check format, nonzero dimensions, stride, offset, length and every multiplication
before mapping or copying. The advertised size must fit the transferred SHM
object and VM_SHM_MAX_BYTES; negotiate a per-session memory budget. A surface
mapping is process local and separate from the control descriptor.

Current SHM objects are limited to 16 MiB. A 2732x1536 XRGB8888 surface needs
16,785,408 bytes, exceeding that limit even before additional buffers. The
standalone WindowServer wrapper also caps output at 1920x1080. Resolve display
size negotiation and either bounded larger SHM objects or tiled transfers
before matching the normal desktop's display modes; do not silently inherit
these prototype limits in the new host adapter.

The first implementation preserves the current copied-frame model. An app
cannot mutate a frame the compositor is reading: copy into a sealed snapshot,
or implement double buffering with an acknowledged ownership handoff before
attempting zero-copy. Damage must fit the referenced surface. A connection
close cannot recycle storage until all acquired frames are released.

During secure entry the kernel revokes ordinary presentation/input access and
drains in-flight operations before drawing the trusted overlay. Never enqueue
password events in ordinary queues, including raw recovery queues. Surface
updates may wait or be rejected; secure input is never replayed on resume.
An untrusted process cannot clear this state or restore its own display lease.

On owner death: revoke handles and pending requests, release the display lease,
increment epoch, discard stale input, and show trusted fallback output. Bootd
restarts the service with bounded backoff. Old connections receive closure and
reconnect; app processes survive a compositor restart where possible. Reusing
a PID must not inherit rights. Pending operations need deadlines or owner-death
wakeups. Shutdown ordering drains frames before freeing backing memory.

## Linker audit and prerequisites

The audited existing ARM64 archives are UIService.framework/build/libUIService.a
and WindowServer.framework/BUILD/libWindowServerService.a. Both use freestanding
Rust and the aarch64-unknown-none-softfloat target. A shared-link probe using
SysV hash, 4-KiB pages and reachable entry points UIServiceRunDesktop and
WS_Initialize failed for both. ld.lld reports R_AARCH64_ABS64 against local
symbols in read-only ui_render/ui_window and windowserver sections, requiring
PIC. A whole-archive probe also fails in compiler_builtins. Static object
relocation counts include debug records and are not dynamic-loader requirements.

Required work before shipping shared builds:

Repeat the artifact audit with `python3 tools/audit_gui_frameworks.py`. It saves
archive hashes, object relocation counts, reachable-entry shared-link commands
and linker diagnostics in a private temporary directory. Exit 1 currently
means the existing archives are blocked. A successful probe still needs final
ELF, imports, ABI and runtime validation; it is not a deployable framework.

Implementation prerequisites:

- Compile reachable Rust crates and required runtime objects with PIC in an
  isolated target directory. Rebuild core/alloc/compiler_builtins if their
  reachable objects still prevent a PIC link; do not assume changing one crate
  converts prebuilt runtime libraries. Do not permit text relocations.
- UIService currently embeds windowserver_nxu. Remove that embedding when the
  compositor becomes a separate process; replace direct WS_* calls with the
  IPC adapter. Avoid duplicate global allocators and duplicated compositor
  state in desktop and WindowServer libraries.
- Provide a user allocator with Rust Layout alignment support. Current
  WindowServer allocator calls kmalloc/kfree; bridge frame buffers do too.
  Runtime now supplies alignment-aware allocation and reclamation adapters.
  The existing standalone wrapper already provides user-space compatibility
  symbols with a 16-MiB bump arena and a no-op free; these names are not proof
  of a kernel import. Replace that arena before repeated resize/window churn,
  and preserve or explicitly reject alignments beyond the current 16-byte limit.
- Replace both infinite-loop panic handlers with process termination so crashd
  can capture the failure and bootd can restart the service. Retain frame
  pointers and SHA-matched crash symbol sidecars.
- Export only a reviewed C ABI through an export map, give the library its
  framework-relative soname, and measure DT_NEEDED against a declared allowlist.
  Native Rust ABI is not a compatibility boundary.
- Inspect the final linked ELF, not just archive relocations: ET_DYN/AArch64,
  SysV hash, RELA, <=16 program headers, <=32 MiB image span, 4-KiB page layout,
  alignment <=64 KiB and nonoverlapping PT_LOAD pages. No TLS, IFUNC, symbol
  versions, DT_SYMBOLIC, REL/RELR or unsupported dynamic relocations.
- The current runtime supports RELATIVE, ABS64, GLOB_DAT and JUMP_SLOT only.
  Determine additional requirements from the PIC result. No TLS was seen in
  the audited archive relocation listings; that is not proof a new build will
  remain TLS-free. Prefer TLS-free adapters initially.
- Add a final-protection phase and PT_GNU_RELRO enforcement before production
  migration. This gate is now implemented for ARM64: FINALIZE seals kernel-
  recorded PT_GNU_RELRO pages after eager binding and before constructors.
  Keep executable pages nonwritable during all mapping/binding phases.
- Keep softfloat and prevent SIMD use until the scheduler reliably saves the
  relevant CPU state. Exercise ASLR with tepOS available and unavailable.
- Version the service/library set and define activation/rollback before calling
  multi-library updates atomic. NXU currently has no transactional activation.

## Ordered migration gates

1. Specify and implement ownership, epoch negotiation and dispatcher validation;
   test malformed sizes, unknown major/features, forged handles and PID reuse.
2. Provide user allocator/panic adapters and reproducible PIC builds. Final ELF
   inspection and absence of kernel imports must pass before boot tests.
3. Add relocation finalization/RELRO; verify text is never writable and writes
   to finalized RELRO fault. Add only relocations actually required by artifacts.
4. Load WindowServer dynamically in its existing service. Test composition,
   cursor, resize/damage, crash reports, lease reclamation and repeated restarts.
5. Implement desktop-side bridge IPC, immutable frame ownership, bounded queues
   and owner-death wakeups. Keep application-facing NXU_UI_* behavior compatible.
6. Separate trusted login/SAP rendering and cursor state, then launch the desktop
   in userspace. Verify wrong password, lockout/unavailable enclave, cancellation,
   secure-input isolation during restart, Dock launches and application reconnection.
7. Remove the kernel UIService archive only after a desktop boot passes without
   UIService or ordinary WS_* implementation exports in the kernel symbol table.
8. Extract CoreGraphics exports, migrate consumers, then rename relevant source,
   bundle and service references with compatibility aliases and ABI tests.

Required end-to-end matrix: normal desktop, headless/recovery, missing or
incompatible framework, dependency replacement without executable rebuild,
compositor/desktop crash under active input and submitted frames, low memory,
queue saturation, PID reuse, service restart during SAP, and matching/stale
framework crash symbols. Existing process-control, SMP, crashd and SAP checks
remain regression gates. None of those future migration gates is claimed to
pass merely because this document and the draft header exist.

## Preparation implementation and verification

Runtime now exports `nxu_alloc`, `nxu_free`, `nxu_realloc`, compatibility
`kmalloc`/`kfree`, and `nxu_runtime_abort`. Allocation uses one anonymous user
mapping per allocation, supports power-of-two alignments through 64 KiB and
reclaims the mapping on free. This is a correctness adapter; allocation-heavy
GUI workloads may need an arena allocator before deployment. Abort restores
and unblocks SIGABRT, then signals the current process for crashd capture.
The isolated userland build replaces the sibling Rust panic handlers with this
adapter and replaces the WindowServer allocator with the alignment-aware Rust
adapter. The sibling sources and kernel-linked builds remain unchanged.

ARM64 and i386 kernel builds pass. Runtime.so has no undefined imports and
uses only loader-supported GLOB_DAT/JUMP_SLOT dynamic relocations.

Run `python3 tools/check_gui_host.py` for native ownership/epoch policy and
allocator checks with ASan/UBSan. It uses mocked process lookup and IRQ locks;
it does not validate ARM64 scheduling, syscall copyin or boot behavior.
`python3 tools/check_dyrld.py --cached-gui --user-cc clang` checks actual Runtime
allocation and GUI negotiation through ARM64 syscalls on private disks alongside
the existing dependency/update/ASLR regressions. The display service is disabled
in this fixture so it cannot contend with the negotiation test.

Run `python3 tools/build_gui_pic.py --offline` for isolated nightly PIC builds
including core, alloc and compiler_builtins, followed by reachable ELF link
audits. Build logs and commands are retained in the output directory. This
does not install or replace frameworks. Stable crate-only PIC builds succeed
but shared linking fails on prebuilt Rust core/alloc relocation objects. The ordinary build-std path resolves an unused `std` dependency on the uncached
`object` package. Use `--freestanding-sysroot` to build only core/alloc and
Rust-only compiler_builtins offline from installed nightly rust-src. This copies
the sources, removes the disabled optional C compiler dependency, builds an
isolated sysroot and never changes the installed toolchain. Compiler-rt C support
is explicitly excluded. This path now passes shared linking for both frameworks.
Final ELF checks and shared runtime probes pass. Deployment still requires the
service, IPC and trusted-renderer migration gates.

Current ARM64 fixture results: both non-ASLR boots passed the Runtime allocator,
HELLO/ATTACH/stale-epoch/RELEASE syscall checks, CoreFoundation parser and
framework replacement checks (results 42 then 84 without changing the app).
The subsequent ASLR/crash phases could not launch because this sandbox denies
the tepOS UNIX mailbox socket bind. They have not been revalidated for this
change. Artifact logs: `/private/tmp/nxu-dyrld-c6gcom6k/`.

## Isolated userland PIC build

```sh
make USER_CC=clang BUILD/userland/Runtime.so
python3 tools/build_gui_pic.py --offline --freestanding-sysroot --output /private/tmp/nxu-gui-pic
python3 tools/check_gui_runtime.py --build /private/tmp/nxu-gui-pic
```

The build copies sibling sources and injects the adapters under
`frameworks/Runtime.framework/rust`. It retains commands, toolchain version,
build logs and the SysV ELF link audit. Explicit anonymous export maps expose
only the existing C ABI; Rust symbols stay local. Both libraries declare
`Runtime.framework/Runtime.so`. WindowServer links with `--no-undefined`; its
clock and allocator/panic imports resolve from Runtime. UIService still has
12 `ui_bridge_*` imports and needs the desktop IPC adapter. UIService still
embeds WindowServer; this build does not separate their process ownership.

`tools/gui_elf.py` inspects final program headers, dynamic tables, symbols and
relocations against loader limits. The audit also checks C exports, declared
dependencies and imports. `deployment_ready` deliberately remains false:
the generated libraries now use `-z relro -z now`. The next ordered gate is
loading WindowServer dynamically in its installed service and validating
scanout, input, lease reclamation and restart behavior.

## ARM64 relocation finalization

`NXU_DYRLD_FINALIZE` (operation 3, argument zero) is called by the interpreter
after all eager relocations and before any constructor. The kernel records
validated PT_GNU_RELRO ranges during LOAD; callers cannot supply protection
ranges or make code writable. RELRO must have a nonzero range, an aligned end
and fit within a single writable non-executable LOAD mapping. Multiple RELRO
headers are rejected. The VM restriction removes both write permission and the
software COW flag, then broadcasts TLB invalidation. Pages keep their ownership
and physical backing. Finalization is idempotent and rejects further LOAD calls;
exec clears state, fork inherits the sealed metadata and permissions.

Shared runtime tests pass for normal composition, Rust panic with SIGABRT
initially ignored/blocked, writing sealed RELRO and writing executable text.
Fatal probes produce persisted symbolicated SIGABRT/SIGSEGV reports. Artifacts:
`/private/tmp/nxu-gui-runtime-wahm8tnv/`. The fork RELRO probe also passes: the child faults with SIGSEGV and the
parent observes the expected killed status. Artifacts:
`/private/tmp/nxu-gui-runtime-k_vhqi49/`. The process-control regression passes
after the VM change; ARM64 and i386 builds pass.

`python3 tools/check_gui_elf.py <WindowServer.so>` checks the final ELF validator
with wrong machine, excess program headers, writable text, TLS, overlapping
LOAD pages and RELRO placed in executable text. This validation complements
the live fault probes; it does not replace kernel validation.

## Dynamic WindowServer service gate

The standalone `windowserver_service` is now a PIE executable interpreted by
`dyrld-runtime`. Its declared dependencies are WindowServer.framework,
CoreFoundation.framework and Runtime.framework. WindowServer's implementation
and allocator are no longer embedded in that service executable. The build
stages WindowServer.so in `/System/Library/Frameworks/WindowServer.framework/`.
The service negotiates host ABI 1.0 and claims the display lease before
initializing the compositor. Its idle IPC loop waits on an event sequence sampled
before inspecting IPC/input, so an enqueue between checking and sleeping cannot
be lost. Active requests yield after replying instead of sleeping for a timer
tick. CoreGraphics waits for each reply with a bounded IPC receive, which wakes
on enqueue and retains the five-second failure deadline.

The dynamic desktop host forwards signed wheel events and uses the same
render-before-idle-wait behavior as the legacy host. Raw input packets retain
position, button and modifier snapshots, preserving transitions queued between
userspace reads. The ARM64 boot supervisor blocks after handing off to userspace,
matching i386, so it no longer consumes kernel-task CPU by yielding forever.

`tools/check_desktop_input.py` checks ordered input, scrolling and idle waits.
The ARM64 desktop service test and i386 frame test also exercise real timed IPC,
wakeup races, kernel-task idle accounting, window dragging and signed scroll
routing on private QEMU disks. Recovery Terminal retains 256 output lines with
wheel and Page Up/Page Down navigation; `tools/check_recovery_terminal.py`
checks retention and scroll bounds.

Run `python3 tools/check_gui_service.py` after building the default ARM64 kernel.
The test creates a private disk and uses a PID 1 fixture linked against the
production bootd manager, job policy and bootstrap registry. It checks rendered
pixels through QEMU screenshots, relative pointer input, resize/destroy, inline
and shared-memory rendering, malformed IPC, withdrawn SHM handles, dead service
ports, and automatic restart after SIGABRT and SIGKILL. Successful restart and
scanout require reclaiming the display lease. The SIGABRT report is read back
from disk and checked for its exception type and matching image identity.
ASLR is disabled for this fixture; the separate loader tests cover ASLR.

This gate exercises the standalone compositor. The normal desktop still uses
the kernel-linked GUI stack. The following milestone replaces UIService's
`ui_bridge_*` imports with a desktop client adapter and removes its embedded
WindowServer dependency, splitting CoreGraphics and DesktopService. Trusted SAP presentation
and input isolation need their own live regression gate before switching the
normal boot path to the userland desktop.

The four-CPU ARM64 service gate passes, including the persisted symbolicated
SIGABRT report. Final artifacts: `/private/tmp/nxu-gui-service-d_kf_lg8/`.
The service ELF check confirms the three declared framework dependencies and
that it exports no embedded `WS_*` implementation. This does not establish
normal desktop migration, trusted-overlay isolation, or ASLR coverage.

## Dynamic desktop integration

`make BUILD/userland/desktop_service USER_CC=clang` builds the desktop host,
CoreGraphics.so and DesktopService.so from isolated copies of the framework
sources. Installed libraries live under `/System/Library/Frameworks`. The
UIService source repository and its legacy kernel build remain compatible.

`python3 tools/check_gui_service.py --desktop` exercises the actual ARM64
WindowServer and desktop processes with an ordinary app: frame submission,
mouse and keyboard delivery, process-exit window cleanup, compositor restart,
malformed requests and fatal-crash reports. Session tests also reject stale
epochs, invalid generation handles and callers without display capabilities.

App frames currently use bounded kernel snapshots copied into desktop memory.
Compositor frames use size-checked SHM mappings. The host does not advertise the
future SURFACE/DAMAGE/INPUT feature bits. No borrowed kernel pointers cross the
session interface. A failed frame copy preserves the pending frame for retry.

Trusted login/SAP handoff, filesystem and Activity Monitor callbacks, scale
configuration and window recreation are covered by the subsequent milestones
below. The authenticated compositor milestone below adds peer ownership of window
handles. At this milestone, normal-boot activation still required a complete production session
check.
The private test activates DesktopService explicitly; no production startup job
has been enabled by this milestone.

The four-CPU ARM64 desktop gate passes, including visible surface removal after
app exit and persisted symbolicated compositor crash reports. Artifacts:
`/private/tmp/nxu-gui-service-ytdo99z1/`. ARM64 and i386 kernel builds, native
ownership/allocator tests, formatting checks and malformed ELF validation for
WindowServer, CoreGraphics and DesktopService also pass.

## Trusted login and SAP handoff

The ARM64 integration path uses `ui.userland=1` to select a kernel-owned
login renderer independent of UIService.framework. The kernel installs the
presentation/input exclusion before PID 1 can run. With an enclave UART
present, a missing or unavailable tepOS keeps login locked. Only tepOS accepting
the password, or accepting initial password setup after confirmation, clears
the boundary. With no enclave UART, the existing no-login policy remains.
Recovery boot does not enter this login path.

WindowServer waits while trusted UI excludes lease negotiation; DesktopService
waits for compositor discovery and session startup. Authentication delays no
longer count as daemon crashes. The normal kernel desktop is skipped only for
this explicit integration mode. Production DesktopService jobs are still not
enabled by default; the integration check creates private bootd jobs.

SAP uses the same exclusion with either desktop backend. Its worker drains
in-flight untrusted operations before saving scanout, owns presentation and
keyboard input until the prompt finishes, restores the saved display and
flushes input again before resuming. The copied desktop adapter treats a
trusted-operation denial as temporary exclusion rather than process death,
so app sessions and surfaces survive the modal. After exclusion, the desktop
reestablishes its relative-pointer baseline. Only WindowServer's display lease
allows ordinary scanout; DesktopService's input permission grants no scanout
permission.

Run `python3 tools/check_gui_trusted.py` with the default ARM64 kernel built.
The test uses a private disk, bootd, dynamic frameworks and a mailbox fixture
through serial FIFOs. It checks login rejection/unlock, SAP cancellation,
password rejection, exact-file deletion, app session/surface preservation,
password exclusion from app input and simultaneous denial of input, host
operations and display presentation to a display-capable probe. Screenshots
verify the trusted panels and restored app pixels. `--unavailable` verifies
that an unreachable enclave never opens the desktop. `--setup` exercises
initial password setup with mismatched confirmation and then matching input.
The fixture tests the kernel's mailbox/authentication handling; it does not
establish real enclave persistence or boot signature enforcement.

The following integration milestone adds compositor/desktop restart recovery,
filesystem and Activity Monitor callbacks, scaling and full-resolution support.
The authenticated compositor milestone below addresses peers and window
ownership. At this milestone, the production desktop job remained disabled pending normal-boot
session validation.

Trusted handoff validation passes on four ARM64 CPUs with UIService unlinked
(`/private/tmp/nxu-gui-trusted-7f381x3f/`) and with legacy GUI libraries linked
(`/private/tmp/nxu-gui-trusted-r_1aqnvb/`). Unreachable-enclave and first-password
setup checks pass in `/private/tmp/nxu-gui-trusted-x0fg913q/` and
`/private/tmp/nxu-gui-trusted-w7iwdgv0/`. The full dynamic desktop/compositor
regression passes in `/private/tmp/nxu-gui-service-e5wsr56s/`. ARM64 and i386
builds, shared-library validation, ownership/allocator tests and formatting
checks pass. These earlier checks did not cover compositor restart during SAP.

### Desktop recovery and host callbacks

When DesktopService exits, the kernel returns live app connections to the
pending queue and retains their latest frames. It also terminates the leased
compositor so a replacement desktop cannot inherit orphaned compositor windows.
A lost compositor transport causes DesktopService to exit directly; bootd's
KeepAlive policy restarts both services. Existing app processes reconnect through
the retained kernel connections without losing their application data. Window
positions and ordering reset to the desktop's defaults.

DesktopService's directory callback uses bounded stat/open/readdir operations,
reports truncation explicitly, and closes descriptors on failure. Its Activity
Monitor callback uses copied statistics snapshots with stable process identities.
The statistics wire layout lives in `nxu/activity.h` and is available even when
UIService is unlinked. The legacy activity layout remains compatible.

The desktop reads `ui.scale` in permille, from 500 through 4000, and retains
fractional pointer movement when converting coordinates. Desktop and compositor
surfaces support up to 3840 by 2160 pixels; the shared-memory ceiling is 64 MiB.

Run `python3 tools/check_gui_service.py --desktop` to check both service restarts,
app survival, restored pixels and input delivery. Use `--width 3840 --height 2160
--scale 2000 --timeout 240` for the full-resolution check. The fixture waits for
an application input acknowledgment before triggering each restart.

The complete 3840 by 2160, 2× integration check passed in
`/private/tmp/nxu-gui-service-oz3tbm25/`, including restored app pixels, pointer
and keyboard input after both service restarts, process-exit cleanup and crash
report generation. Under emulation, use `--timeout 360` to allow slow scanout.

Run `python3 tools/check_gui_trusted.py --restart` to kill the compositor during
SAP's trusted modal. The test checks exclusive presentation and input, app
surface recovery, password rejection and selected-file deletion. This check
passed in `/private/tmp/nxu-gui-trusted-mzvl9ne0/`, including directory metadata,
truncation/error handling and Activity Monitor snapshots. The mailbox fixture
does not establish real enclave persistence or signature enforcement.


### Authenticated compositor registration and window ownership

NXPC messages now carry a kernel-issued 24-byte sender identity: PID, uniqueid,
idversion and a capability snapshot. Syscall 77 (`nxu_ipc_receive_peer`) returns
that metadata alongside the existing payload and transferred port. Syscall 78
(`nxu_process_identity`) lets receivers revalidate the exact live process
lifetime. The original receive and blocking receive signatures are unchanged.
Their sixth syscall argument is still ignored; callers use the new syscall to
request metadata explicitly.

Receive ownership is separate from send access. Only an allocated endpoint's
owner can dequeue messages. Transfers and fork inheritance grant send access,
including bootstrap discovery and one-shot reply ports. Receive permission and
port lookup happen under one space lock. Removing the receive endpoint, or
closing its process's space on exit, deactivates the port, wakes receivers and
drains queued messages. Outstanding send aliases then fail. This intentionally
replaces the earlier behavior where forked or transferred names could receive.

bootd records the kernel identity of the WindowServer job when it spawns and
pins `com.nxu.windowserver` to that exact identity. Registration checks the
sender metadata and current live identity; another process cannot republish
that label, even if it has DISPLAY capability. Job exit/replacement revokes the
registry entry. This policy protects the compositor label specifically; it
does not claim all other bootstrap labels have been authenticated.

WindowServer keeps a bounded ownership table for 256 client windows. Public
handles come from syscall 79 (`nxu_gui_window_handle`), which is restricted to
the leased display owner. The kernel never reuses those handles during a boot
and fails closed on 32-bit counter exhaustion. The service translates public
handles into private framework IDs only after matching the sender's complete
identity. Render, shared-memory render, resize, destroy and per-window actions
reject cross-client and stale handles before accessing pixels or mutating
framework state. Global pointer/cursor, background, scale and presentation
operations require DISPLAY capability. Display-size queries remain available
to live clients. The service reaps windows when their creating process exits;
a new process with a reused PID cannot retain them.

`python3 tools/check_compositor_policy.py` exercises the production registry and
ownership table with reused PIDs, changed uniqueids/idversions, queued messages
from exited senders and replacement service identities. The fixtures simulate
PID recycling; they do not force the live kernel to exhaust its PID allocator.

`python3 tools/check_gui_service.py --desktop --timeout 240` adds a real
unprivileged peer which attempts protected-label registration, receiving on a
send-only service port, cross-client render/resize/destroy/move and global
pointer changes. It checks the peer's identity/capabilities, visible cleanup
on exit, endpoint revocation, destroyed-handle rejection and stale handles
across SIGABRT/SIGKILL compositor restarts. The full desktop test passed in
`/private/tmp/nxu-gui-service-dbzfx7ui/`. The trusted SAP restart check passed in
`/private/tmp/nxu-gui-trusted-i_91i04r/`. ARM64 and i386 builds and kernel IPC
ownership/transfer/fork/revocation self-tests pass. Production startup remains
unchanged by this milestone.

The same adversarial and recovery suite passes at 3840 by 2160, 2× scale in
`/private/tmp/nxu-gui-service-nnduqkck/`. Run it with `--width 3840 --height 2160
--scale 2000 --timeout 360` to allow slower emulated presentation.


## Normal desktop activation

`make run` now selects `ui.userland=1` and builds the desktop kernel without
the legacy GUI archives. bootd starts `com.nxu.desktop` only when that boot
argument is present. Both desktop and compositor remain enabled in safe mode;
recovery continues to bypass the ordinary desktop startup. The old About demo
and intentional crash-test jobs require explicit demo boot arguments.

Kernel object dependencies include a compiler/flags stamp, so switching an
existing build directory between linked and dynamic GUI configurations rebuilds
its objects. This prevents stale legacy references from reaching the link.

`tools/check_desktop_jobs.py` checks the installed job configuration against the
production parser and job initializer. `tools/check_gui_trusted.py --production
--kernel BUILD/desktop/kernel.bin` uses the installed job set and real bootd,
authenticates login and SAP, launches Voyager, Activity Monitor and About through
Dock, and checks a saved crash report and visible Problem Reporter. Its restart
variant exercises service recovery with the same installed job configuration.

The isolated CoreGraphics build also supplies the original font, cursor, wallpaper
and branding asset defaults. Those assets are Make dependencies, so changes
rebuild the installed framework instead of silently selecting bitmap fallbacks.

Cold framework measurements use a static 64 KiB read buffer. The loader bounds
its measurement reply wait at 120 seconds to accommodate concurrent asset-heavy
startup requests; it still compares the daemon digest with the mapped image.


### Bootstrap polling cost

The manager drains the registry at the start and end of each job cycle, plus
after a potentially blocking spawn or a job-state transition. It no longer
drains after every unchanged or disabled job. This retains prompt registration
and compositor revocation while making idle registry polling independent of the
installed job count.

`python3 tools/check_bootd_polling.py` exercises the production manager with
mocked job/registry operations. For 32 jobs across 1,000 idle cycles, registry
polls fall from 33,000 to 2,000. It also checks immediate compositor revocation,
due-spawn servicing and throttled jobs. These are operation counts, not CPU
usage or frame-rate measurements.

The ARM64 production restart check passes with this manager
(`/private/tmp/nxu-gui-trusted-1gqk377_/`), including login, SAP, app launches and
visible crash reporting. The fixture waits for enclave health before injecting
login input instead of assuming a fixed startup delay under emulation.


### Reusable frame transfer storage

The CoreGraphics compositor adapter reuses one packed shared-memory transfer
buffer across synchronous window and cursor updates. WindowServer copies each
request into its owned window/cursor storage and unmaps the transfer before
sending the reply. The client can then overwrite that buffer for another update.
The pixel copy and window ownership checks remain unchanged.

The buffer grows to the largest submitted frame, bounded by 3840×2160×4 bytes
(about 31.6 MiB), and is retained until growth, a failed pixel exchange or process
exit. A failed/timed-out pixel exchange withdraws and unmaps the buffer, so a
queued request cannot observe a later frame through a reused region. The kernel
also withdraws SHM publications owned by an exiting process.

`python3 tools/check_gui_transfer.py` runs the production client adapter with
AddressSanitizer and UndefinedBehaviorSanitizer. It checks padded stride packing,
resizing, cursor reuse, maximum 4K transfers, rejected/failed/timed-out requests
and allocation failures. Across 1,000 same-size window updates, client SHM
lifecycle calls fall from 4,000 to one create and one map; server-side mapping
and pixel-copy work still occur. This is an operation-count result, not an FPS
claim.

The ARM64 desktop gate passes with reusable transfers
(`/private/tmp/nxu-gui-service-ols_ufi4/`): scanout, pointer/keyboard input,
resize/destroy, malformed IPC, compositor/desktop restart, app exit cleanup and
host boundary checks. Restart input waits for the replacement desktop to report
ready, since scanout can retain the old image during the handoff.


### Shared frame-copy optimization and i386

Both architectures use `ui_frame_copy_from_user` for app submission. Packed
frames make one `vm_copy_from_user` call for the full image instead of one call
per row. Padded frames still copy and pack individual rows. The helper rejects
invalid dimensions and source-span overflow before copying; a copy fault does
not publish the spare frame. User-memory access still validates every page and
resolves demand-zero/COW faults. Each validated page chunk now uses the existing
aligned word-copy routine instead of the byte loop in `vm/user_copy.c`.

The native sanitizer check (`tools/check_ui_frame_copy.py`) verifies a full
1080p packed submission, padded rows, faults and address overflow. Its packed
frame needs one copy call instead of 1,080; this does not eliminate the page
walks inside that call and is not a measured frame-rate claim.

i386 now uses the same userland service boundary. `make run-i386-desktop` builds
ELF32 Runtime, CoreFoundation, WindowServer, CoreGraphics and DesktopService
frameworks into `$(BUILD_ROOT)/i386/diskroot/System/Library/Frameworks`, then
starts `dyrld`, `windowserver_service` and `desktop_service` through bootd. The
kernel image has no linked WindowServer or UIService Rust implementation. Use
`make i386-frameworks BUILD_ROOT=BUILD/BUILD-i386-desktop` to build the framework
images alone, or `make run-i386-desktop` to boot the full desktop.

The ELF32 loader maps each image into its own bounded slot below the i386 shared
memory window. It validates ELF32 headers and segments, measures load bytes
through the bootd registry service, applies `R_386_32`, `R_386_GLOB_DAT`,
`R_386_JMP_SLOT` and `R_386_RELATIVE` relocations, runs constructors, and seals
GNU_RELRO pages read-only. Address slides use the enclave random service and
fail closed when that service is unavailable. The x86 relocation format uses
implicit addends; symbol versions, TLS, text relocations and RELR remain
unsupported, matching the current ARM64 loader's small ABI.

`python3 tools/check_i386_gui_frames.py --enclave` boots a private disk under
TCG with the mailbox fixture. It tests enclave random/measurement requests,
trusted display and input exclusion, rejected and accepted passwords, runtime
constructors and allocation, a faulting RELRO write, and packed/padded app
frames with mouse, keyboard, and process-exit cleanup. The fixture checks
architecture and memory behavior; the production enclave transport still needs
a normal `make run-i386-desktop` boot for hardware-specific validation.

Host content-scale detection also rejects NaN/out-of-range results, retaining
the documented 2000-permille fallback when no host screen is available.
