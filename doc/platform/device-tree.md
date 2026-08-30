# Flattened Device Tree Parser

`platform/arm64/dtb.c` implements a read-only, bounds-checked parser for the
flattened Device Tree (FDT) blob that QEMU passes to the kernel in `x0`.

## Blob structure

```text
offset 0                +--------------------------------+
                        | header (40 bytes, big-endian)  |
                        +--------------------------------+
memory_reservation_     | memory reservation block       |
offset                  |   pairs of 64-bit address/size |
                        |   terminated by 0, 0           |
                        +--------------------------------+
structure_offset        | structure block                |
                        |   FDT_BEGIN_NODE / FDT_PROP /  |
                        |   FDT_END_NODE / FDT_NOP /     |
                        |   FDT_END tokens               |
                        +--------------------------------+
strings_offset          | strings block                  |
                        |   NUL-terminated property names|
                        +--------------------------------+
total_size              +--------------------------------+
```

Every multi-byte value in the blob is **big-endian**, regardless of the host.
AArch64 runs little-endian here, so every read goes through a conversion.

## Header validation

`dtb_init()` reads all ten header fields and validates before storing anything.

```c
typedef struct {
	uint32_t magic;
	uint32_t total_size;
	uint32_t structure_offset;
	uint32_t strings_offset;
	uint32_t memory_reservation_offset;
	uint32_t version;
	uint32_t last_compatible_version;
	uint32_t boot_cpu_id;
	uint32_t strings_size;
	uint32_t structure_size;
} dtb_header_t;
```

A compile-time assertion pins the layout:

```c
_Static_assert(sizeof(dtb_header_t) == DTB_HEADER_SIZE,   /* 40 */
               "dtb_header_t must match the DTB header layout");
```

Checks performed, in order:

| Check | Rejects |
| --- | --- |
| `magic == 0xD00DFEED` | Not an FDT blob at all |
| `total_size >= 40` | A blob too small to contain its own header |
| `structure_offset & 0x3 == 0` | Misaligned structure block — tokens are 4-byte |
| `memory_reservation_offset & 0x7 == 0` | Misaligned reservation block — entries are 8-byte |
| structure block within `total_size` | Out-of-bounds structure block |
| strings block within `total_size` | Out-of-bounds strings block |
| at least 16 bytes of reservation block | No room even for the terminating pair |

The range check is written to be overflow-proof:

```c
if (offset > total_size) {
	return false;
}
return size <= total_size - offset;
```

Computing `offset + size` first could wrap and pass a check it should fail.

The version fields are recorded into `dtb_t` and printed, but **not validated**.
NXU parses only the parts of the format that have been stable since version
16, so it does not need to branch on the version. QEMU reports version 17.

The `boot_cpu_id` and `memory_reservations` fields are likewise stored but never
used.

## Parsed view

```c
typedef struct {
	const void       *base;
	const dtb_header_t *header;

	uint32_t total_size;
	uint32_t version;
	uint32_t last_compatible_version;
	uint32_t boot_cpu_id;

	const uint8_t *memory_reservations;

	const uint8_t *structure;
	uint32_t       structure_size;

	const char *strings;
	uint32_t    strings_size;
} dtb_t;
```

All pointers are **into the blob**, so `dtb_t` is only valid while the blob
remains present and mapped. This is why `pmm_init()` permanently reserves the
DTB's physical range.

`dtb_init()` fills the structure only after every check passes, so a rejected
blob leaves it untouched.

## Tokens

The structure block is a stream of 4-byte big-endian tokens:

| Token | Value | Payload |
| --- | --- | --- |
| `FDT_BEGIN_NODE` | 1 | NUL-terminated node name, padded to 4 bytes |
| `FDT_END_NODE` | 2 | none |
| `FDT_PROP` | 3 | `uint32 length`, `uint32 name_offset`, then `length` bytes padded to 4 |
| `FDT_NOP` | 4 | none |
| `FDT_END` | 9 | none; terminates the block |

An unrecognized token fails the walk immediately rather than being skipped —
there is no way to know how many bytes an unknown token consumes.

## Traversal

```c
bool dtb_walk(const dtb_t *dtb, const dtb_visitor_t *visitor, void *context);
```

Walks the structure block from beginning to end, invoking visitor callbacks.

```c
typedef struct {
	void (*begin_node)(const char *name, uint32_t depth, void *context);
	void (*property)(const char *name, const void *value,
	                 uint32_t length, uint32_t depth, void *context);
	void (*end_node)(uint32_t depth, void *context);
} dtb_visitor_t;
```

Every callback is optional; a null pointer is skipped. The `visitor` pointer
itself may be null, which walks the tree purely for validation.

### Depth

`depth` starts at 0 for the root node and increments after each
`FDT_BEGIN_NODE`, decrementing before `end_node` is invoked. The consequence is
a subtle but important asymmetry:

- `begin_node` receives the depth **of the node itself**.
- `property` receives `depth - 1`, so a property is reported at the depth of its
  **owning node** rather than the current nesting level.
- `end_node` receives the depth of the node being closed.

This means a visitor can index a per-depth array with the same value in all
three callbacks — which is exactly what `platform_discover()` does.

`DTB_MAX_DEPTH` (64) caps nesting. Exceeding it fails the walk rather than
letting a visitor index past its own array.

### Bounds and alignment

Every read is guarded before it happens:

```c
static bool dtb_has_bytes(const uint8_t *cursor, const uint8_t *end,
                          uint32_t byte_count)
{
	if (cursor > end) {
		return false;
	}
	return byte_count <= (uint32_t)(end - cursor);
}
```

The subtraction form avoids the pointer overflow that `cursor + count <= end`
could produce.

Node names and property payloads are padded to 4-byte boundaries.
`dtb_align4()` computes the padded length and refuses values above
`0xFFFFFFFC`, where rounding up would wrap.

`dtb_string_length()` scans for a NUL within an explicit end pointer and fails
if none is found, so an unterminated string in a truncated blob cannot run off
the end.

For each `FDT_PROP`, the parser additionally checks that `name_offset` is inside
the strings block **and** that the name is NUL-terminated within it. The
resulting length is discarded — the check exists purely to prove the property
name is a valid C string before it is handed to a visitor.

### Structural validation

Three structural rules are enforced:

| Rule | Violation |
| --- | --- |
| `FDT_END_NODE` requires `depth > 0` | Unbalanced close |
| `FDT_PROP` requires `depth > 0` | A property outside any node |
| `FDT_END` requires `depth == 0` and `cursor == end` | Unclosed nodes, or trailing bytes |

Falling out of the loop without seeing `FDT_END` also fails: it means the block
was truncated.

**Return:** `true` only if the block was fully and correctly consumed. `false`
on any malformation, with no indication of where.

## Dump visitor

```c
bool dtb_dump(const dtb_t *dtb);
```

Walks the tree with a visitor that prints each node and property, indented two
spaces per level, and finishes with a count:

```text
dtb: node /
dtb:   node platform-bus@c000000
dtb:     property ranges (24 bytes)
dtb:     property compatible (26 bytes)
dtb: walk complete: 57 nodes, 223 properties
```

The root node's name is empty in the binary format, so the visitor prints `/`
for it explicitly.

Property **values** are not printed — only names and lengths. Dumping values
would require type inference the parser does not perform, since the FDT format
carries no type information.

`dtb_dump()` runs before discovery so that a malformed tree is diagnosed with a
clear message, and so the boot log records the machine's device list.

**Return:** whatever `dtb_walk()` returns.

## Malformed-tree handling

Every failure mode returns `false` and stops. There is no recovery, no partial
result and no diagnostic detail — a caller learns only that the blob is
unusable. `kern_init()` treats both `dtb_init()` and `dtb_dump()` failures as
fatal.

This is a deliberate choice for a firmware-supplied structure: a blob that
fails validation cannot be trusted for anything, and continuing with partial
data would produce a machine description that is wrong in an unknown way.

## Concurrency and interrupt context

The parser holds no state. `dtb_init()`, `dtb_walk()` and `dtb_dump()` are
reentrant with respect to each other and safe to call concurrently on distinct
`dtb_t` values.

They must not be called from interrupt context in practice, because the visitors
in use print to the UART and because discovery is not something an interrupt
should trigger.

## Current limitations

- **Read-only.** No modification, no node insertion, no blob generation.
- **No path lookup.** There is no `dtb_find_node("/soc/uart@9000000")`; finding
  something requires a full walk with a visitor.
- **No `phandle` resolution**, so nodes cannot be cross-referenced.
- **No property type interpretation.** Values are handed to visitors as raw
  bytes with a length.
- **Memory reservation block is validated but never parsed.** Its entries could
  describe firmware-reserved ranges the PMM should avoid; NXU ignores them.
- **No `chosen` node handling**, so no `bootargs`, `stdout-path` or initrd
  location is recovered.
- **Version fields are recorded but unchecked.**
- **Failures carry no detail** — no offset, no reason.
- **Full traversal per query.** `kern_init()` walks the tree twice, once to dump
  and once to discover.

## Source files

- [`platform/arm64/dtb.c`](../../platform/arm64/dtb.c)
- [`platform/dtb.h`](../../platform/dtb.h)

## Related documentation

- [Platform overview](overview.md)
- [Hardware discovery](hardware-discovery.md)
- [Boot](../boot.md)
- [Initialization](../initialization.md)
- [Physical memory](../vm/physical-memory.md)
