# Freestanding Runtime (`libk`)

`libk` is the small set of definitions and functions a freestanding C kernel
must supply for itself. It is not a libc and does not aim to become one.

## Why a kernel cannot use a hosted libc

C distinguishes two conformance modes. A **hosted** implementation provides the
full standard library and starts at `main()`; a **freestanding** one provides
only a handful of headers and starts wherever the implementation says.

NXU is built with `-ffreestanding`, which is not a preference:

- A hosted libc calls into an operating system for everything that matters —
  `malloc()` needs a memory manager, `printf()` needs a file descriptor,
  `exit()` needs a process. NXU *is* the operating system; there is nothing
  below it to call.
- The kernel's entry point is `_start`, established by the linker script, not
  `main()`. There is no C runtime startup, no `.init_array` processing and no
  `atexit()`.
- The build links with `-nostdlib`, so no library is available even if the code
  tried.

What remains available in freestanding mode is the set of headers that define
types and limits without requiring runtime support: `<float.h>`, `<limits.h>`,
`<stdarg.h>`, `<stddef.h>`, `<stdint.h>`, `<stdbool.h>` and a few others. NXU
takes `<stddef.h>` from the compiler and supplies its own `<stdint.h>` and
`<stdbool.h>`.

## `stdint.h`

```c
#define UINT8_MAX  255U
#define UINT16_MAX 65535U
#define UINT32_MAX 4294967295U
#define UINT64_MAX 18446744073709551615ULL

typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
typedef unsigned long  uint64_t;
```

The `unsigned long` definition of `uint64_t` is correct for the LP64 model that
`aarch64-none-elf` uses, where `long` is 64 bits.

Only unsigned types are defined. NXU uses no signed fixed-width integer
anywhere: addresses, sizes, counts, register values and bitmaps are all
naturally unsigned, and avoiding signed arithmetic avoids implementation-defined
behavior on overflow and on right shifts.

`UINT64_MAX` is used throughout the memory managers for overflow checks —
`pmm_add()`, `pmm_align_up()`, `vmm_align_up()`, `heap_size_to_pages()` and
`vm_kern_size_to_pages()` all test against it before performing arithmetic that
could wrap.

## `stdbool.h`

```c
#define bool _Bool
#define true 1
#define false 0
```

`_Bool` is a language keyword and needs no library support; the header only
supplies the conventional spellings.

`bool` is the universal NXU result type. `pmm_init()`, `vmm_map_page()`,
`vm_kern_allocate()`, `kfree()`, `cache_init()`, `dtb_walk()` and
`platform_discover()` all return it. There is no error code enum and no `errno`:
a function either succeeded or it did not, and the caller's only recourse is to
propagate or halt.

## `stddef.h`

Not supplied by `libk`. It comes from the compiler's own freestanding headers,
which is where `size_t`, `ptrdiff_t` and `NULL` are defined.

`size_t` appears in the heap and `vm_kern` interfaces, so `heap.h`,
`vm_kern.h` and `string.h` all include `<stddef.h>`.

Note that NXU code conventionally writes `0` rather than `NULL` for null
pointer comparisons throughout.

## `string.h`

`libk/string.h` currently exports the byte-copy and byte-fill primitives needed
by the kernel:

```c
void *memcpy(void *destination, const void *source, size_t size);
void *memset(void *destination, int value, size_t size);
```

Both routines are deliberately small freestanding implementations. `memcpy()`
requires non-overlapping ranges; `memset()` fills the destination with the low
eight bits of `value`. The signatures match the standard C forms because Clang
may emit calls to these names while lowering aggregate copies and zeroing even
when the original source did not contain an explicit function call.

The current implementations use byte loops. They are correct for kernel bring-up
but have no word-sized, SIMD or cache-line fast path. `-mgeneral-regs-only` also
means any future optimized implementation must remain within the kernel's
register-state contract.

## CRC32C

`libk/crc32c.c` provides the Castagnoli checksum primitive used by ext4 and
JBD2 metadata protection:

```c
uint32_t crc32c(uint32_t crc, const void *buffer, size_t size);
```

The routine accepts a running CRC value and returns the extended value without a
final complement. Filesystem callers therefore seed it with `~0U` or with a
UUID-derived checksum seed and may extend the result over multiple fields.

The implementation is table-free and processes one bit at a time. That keeps the
freestanding runtime auditable while the filesystem formats are still being
brought up. A later ARMv8 CRC32C implementation can replace it without changing
callers.

`kern_init()` validates the implementation against the fixed `"123456789"`
CRC32C test vector using NXU's running-checksum convention before mounting the
VFS.

## Compiler-generated runtime calls

`-ffreestanding` does not guarantee that Clang will inline every memory
operation. Large aggregate initialization or assignment may still lower to a
runtime helper such as `memset()` or `memcpy()`. Keeping those exact symbols in
`libk` prevents source constructs from acquiring hidden hosted-libc dependencies.

`memmove()` and `memcmp()` are not currently provided. Code must not rely on the
compiler materializing them until matching `libk` implementations exist.

## Current limitations

- `memcpy()` and `memset()` are byte loops with no architecture-specific fast
  path.
- CRC32C is table-free and has no ARMv8 CRC instruction acceleration.
- `memmove()`, `memcmp()` and general string routines are not implemented.
- The freestanding C library has no hosted stdio; kernel diagnostics use the machine-independent `kprintf()` console formatter.
- There is no hosted allocator interface; kernel allocation remains
  `kmalloc()`/`kfree()`.
- Only the fixed-width integer types required by the kernel are provided.

## Source files

- [`libk/stdint.h`](../../libk/stdint.h)
- [`libk/stdbool.h`](../../libk/stdbool.h)
- [`libk/string.h`](../../libk/string.h)
- [`libk/string.c`](../../libk/string.c)
- [`Makefile`](../../Makefile)

## Related documentation

- [Build system](../build-system.md)
- [Heap](../kern/heap.md)
- [UART](../platform/uart.md)
- [Physical memory](../vm/physical-memory.md)
