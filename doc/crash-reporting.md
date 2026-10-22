# Crash reporting on ARM64

`crashd` is a persistent bootd service (`com.nxu.crashd`). Fatal uncaught
SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP and SIGQUIT produce crash
reports. Normal exits, handled signals, SIGTERM and SIGKILL do not.

## Capture and lifetime

After claiming the process exit, the kernel reserves an allocation-free crash
slot, stops every thread using the normal scheduler termination path, and waits
for remote CPUs to leave those threads. Terminal ARM64 traps save their general
register state before unwind. Blocked syscalls retain their exception frames.
The kernel reads bounded frame-pointer chains only after all siblings have
stopped and before reclaiming their address space or stacks. It never directly
dereferences user pointers or demand-faults stack pages during collection.
The faulting instruction remains the first frame; caller frames follow it.

The process then exits normally. The daemon consumes the retained snapshot,
not a live process; neither disk latency nor an absent reporter can indefinitely
suspend the application. There is no Mach exception port: the NXU crash syscall
provides a protected blocking notification channel. At most eight snapshots
are retained, each with 32 threads and 32 frames per thread. A full queue drops
new reports without blocking process termination; a later report includes the
cumulative dropped count. Truncated, unreadable or unavailable state is marked.
Existing task teardown has a five-second remote-thread timeout; a failed
rendezvous marks the snapshot incomplete and retains the unsafe resources.

Captured registers are x0–x30, PC, SP, PSTATE, FAR and ESR. SIMD/FP registers
are not saved by NXU's exception entry and are explicitly unavailable. FAR and
ESR describe the fault only for a synchronous faulting thread; other threads'
trap metadata is not an additional exception. Threads that never entered user
mode may have no exception frame. Frame-pointer unwinding is bounded and best
effort; optimized code without frame pointers can produce partial traces.
User C builds retain frame pointers and debug information. Registers of remote
threads reflect where each CPU stopped them, not a simultaneous machine snapshot.

## Reports and symbolication

Guest paths (the `/Volumes/mainOS` mount prefix is used by syscalls):

- System services: `/Library/Logs/DiagnosticReports/`
- Application bundles: `/Users/Shared/Library/Logs/DiagnosticReports/`

NXU currently has no login credentials or home-directory mapping, so the latter
is an explicit shared-user directory, not an invented `~/` path. Filenames use
uptime, process unique ID and incident ID. Failed writes are retried; the kernel
snapshot is acknowledged only after write, close and filesystem sync succeed.
An interrupted write can leave a partial file until retry; there is currently
no atomic rename syscall. Crashd requires a writable disk with these staged
directories. It does not claim a report was saved when storage fails.

The default disk size is 128 MiB to leave room for symbol maps and reports.
`tools/crash_symbols.py` runs during ARM64 disk staging. It reads ELF function
symbols and DWARF line tables into sorted, fixed-record `.crashsym` sidecars
next to each executable. The loader measures SHA-256 over PT_LOAD file bytes
in program-header order while loading the image; fork preserves that identity
and exec replaces it. The reporter only uses a sidecar with that exact identity,
then binary-searches address ranges for function names, offsets and source
lines. Missing debug information gives function names without source lines;
a missing or mismatched map leaves raw addresses. Return PCs are adjusted to
the call instruction for lookup. Version 2 reports include dyrld's mapped
application, frameworks and interpreter, with their SHA-256 identities, segment
ranges and load biases. Symbol lookup chooses the executable segment containing
the frame and subtracts that image's slide. Static applications retain their
single-image report. Rust release archives may lack source-line data.
Sidecar names are currently truncated to 95 bytes for functions and 127 for
source locations. Inline call chains and DWARF CFI unwinding are not implemented.

## Desktop presentation and permissions

`com.nxu.problemreporter` blocks on a separate saved-report notification queue.
It presents a compact application window using antialiased SF Pro, with Ignore,
Report and Reopen (for application bundles). Report displays the first 8 KiB and
supports scrolling. Reopen goes through the existing Dock launch mechanism;
it does not silently restart the failed process. No network upload occurs.
Report persistence continues when the desktop is unavailable. Eight UI notices
can queue independently; excess notices are discarded without losing reports.

`AllowCrashReports` grants NXU_CAP_CRASH to crashd: read snapshots, acknowledge
persisted incidents and publish UI notifications. `AllowCrashNotifications`
grants NXU_CAP_CRASH_UI to the desktop listener, which sees only names and saved
paths, not raw register snapshots. The crash consumer has an exclusive lease;
a second consumer is rejected and process exit releases the lease without
removing unacknowledged reports. FS_WRITE remains separately required to write
reports. Both services sleep while idle.

Boot arguments: `-no-crashd` disables capture consumption; `-no-crash-ui` disables
dialogs. Crashd remains available in safe mode; the UI helper does not. This
implementation captures ARM64 processes; i386 retains its existing fault path.

## Verification

Run `python3 tools/check_crashd.py` (or `make test-crashd`). It builds a private
staged disk, boots four ARM64 CPUs, generates SIGABRT, SIGILL and a three-thread
SIGSEGV, and checks the files on disk for register state, sibling threads, fault
address, frame symbols, source lines and matching image identity. It also checks
that normal exits and caught signals produce no reports and unprivileged
processes cannot read snapshots or publish/consume UI notifications. Artifacts
and reports remain in the printed `/private/tmp/nxu-crashd-*` directory.

`python3 tools/check_crashd.py --ui --stale-symbols` also stages a temporary
application bundle, verifies shared-user report routing and stale-map fallback,
and captures the Problem Reporter dialog in `screen.ppm`.

For manual desktop testing, launch `/Volumes/mainOS/System/Library/CoreServices/crashtest`
from a temporary bootd job with RunAtLoad=true. It deliberately faults at
0x12345000. KeepAlive should be false to avoid a crash loop. Remove the job when
finished. The registered `com.nxu.crashtest` job runs once only when the boot
arguments contain `crash.test=1` (`KeepAlive` is false). Normal startup does
not deliberately crash a test process.
