# Runtime frameworks and dyrld

GUI migration boundaries, proposed host ABI and audited linker blockers are
defined in [GUI framework migration](architecture/gui-framework-migration.md).

ARM64 now has an ELF runtime loader. Frameworks are installed in
`/System/Library/Frameworks` (accessed through `/disk` by current VFS clients).
The build produces `Runtime.framework/Runtime.so` and
`CoreFoundation.framework/CoreFoundation.so` separately from the kernel.

## Launch and binding

Build a position-independent application with `ld.lld -pie --hash-style=sysv
-z max-page-size=4096 -z relro -z now --dynamic-linker
/System/Library/CoreServices/dyrld-runtime`. Build libraries with `-fPIC` and
`ld.lld -shared --hash-style=sysv -z max-page-size=4096 -z relro -z now`, giving each
a relative soname such as `CoreFoundation.framework/CoreFoundation.so`.

The kernel recognizes this interpreter and starts the static `dyrld-runtime`
inside the application's task. It preserves the original executable path and
arguments. The runtime requests bounded ET_DYN mappings, walks DT_NEEDED
dependencies, binds symbols eagerly, runs dependency constructors first, and
enters the application's NXU `_start`. The persistent `com.nxu.dyrld` service
measures image content and checks it against the kernel's mapped identity.
Measurement detects changed content during launch; it is not a signature policy
or a guarantee that a framework is trusted.
Measurement replies time out after 30 seconds rather than leaving an
application stranded when the daemon restarts during launch.

Mappings reject overlapping segments, writable executable segments, excessive
size, unsupported interpreters and dependency paths outside the framework root.
ASLR obtains randomness from tepOS and fails closed when unavailable. The
explicit developer boot argument `dyrld.aslr=0` disables random sliding for
isolated tests. Each process can load 16 images plus its interpreter.

Supported relocations are AArch64 RELATIVE, ABS64, GLOB_DAT and JUMP_SLOT.
The first version requires SysV hash tables, RELA and ordinary global/local/weak
symbols. TLS, IFUNC, symbol versioning, DT_SYMBOLIC, REL/RELR, lazy binding,
unloading, destructor dispatch and a shared cache are not implemented. These
formats are rejected where detected. The runtime finalizes kernel-recorded PT_GNU_RELRO ranges after eager binding
and before initializers; these pages then reject writes. Executable pages are
never user-writable. Further image loads are closed until exec. Use the build
flags above for the supported format. Initializers use bounded DT_INIT and
DT_INIT_ARRAY records. Crash reports record each mapped image and its slide.

## Updating and migration

Replacing a compatible library changes future launches without relinking the
application or kernel. Running processes retain private copies of their mapped
code. Updates must preserve the ABI. There is no atomic version activation or
rollback transaction yet; replacing mutually dependent libraries requires a
controlled maintenance boundary.

Existing applications and boot services still use their static link rules.
UIService is still linked into the kernel and WindowServer into its service
executable. Converting those consumers requires a separate host ABI migration;
this implementation does not claim they are independently replaceable today.
i386 retains its static loader.

Run `make test-dyrld` for a private disk test of interpreter handoff, nested
dependencies, relocations, weak symbols, constructor order and changing a
library without changing the application executable. It also calls the shared
CoreFoundation parser, verifies different slides with isolated enclave
randomness, and checks framework symbols and source lines in a persisted crash
report from a fault inside a randomized library.

GUI PIC preparation now has an offline freestanding-sysroot path:
`python3 tools/build_gui_pic.py --offline --freestanding-sysroot`. It builds
userland Rust allocation/panic adapters in copied sources and audits the final
ELF files. It does not replace the current desktop or install the generated
frameworks; see the GUI migration spec for the remaining gates.
