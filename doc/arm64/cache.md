# Cache Control

`kern/arm64/cache.c` discovers the CPU's cache topology, invalidates
stale cache contents, and enables the EL1 instruction and data caches.

## Why caches are enabled separately from the MMU

On AArch64 the data cache is only meaningful once the MMU is on. Whether a
memory access is cached is determined by the memory attributes in the
translation-table descriptor, resolved through `MAIR_EL1`. With
`SCTLR_EL1.M == 0` all data accesses behave as Device memory regardless of
`SCTLR_EL1.C`.

NXU therefore separates the two steps: `vmm_init()` enables translation and
establishes the attributes, and `cache_init()` then turns the caches on.
`cache_init()` enforces this explicitly:

```c
if ((sctlr & CACHE_SCTLR_M) == 0ULL) {
	return false;
}
```

## `SCTLR_EL1.C` and `SCTLR_EL1.I`

| Bit | Name | Effect |
| --- | --- | --- |
| 0 | `M` | Stage-1 MMU enable — a precondition, not modified here |
| 2 | `C` | Data and unified cache enable at EL0/EL1 |
| 12 | `I` | Instruction cache enable at EL0/EL1 |

`C` and `I` are not "make memory cacheable" switches; the attributes already
say that. They are closer to "honour the cacheability the tables specify".
With `C` clear, Normal cacheable memory is treated as Non-cacheable.

## Cache discovery

Three architectural registers describe the cache hierarchy:

| Register | Content |
| --- | --- |
| `CLIDR_EL1` | Cache type at each of up to 7 levels, 3 bits per level |
| `CCSIDR_EL1` | Geometry of the cache currently selected by `CSSELR_EL1` |
| `CTR_EL0` | Minimum line sizes across all caches |

`CLIDR_EL1` encodes each level's `Ctype` in bits `[level*3 + 2 : level*3]`:

| Value | Meaning | Has data content? |
| --- | --- | --- |
| 0 | No cache | No |
| 1 | Instruction cache only | No |
| 2 | Data cache only | Yes |
| 3 | Separate instruction and data caches | Yes |
| 4 | Unified cache | Yes |

`cache_type_has_data()` maps this to whether a level needs data-cache
invalidation. Levels reporting 0 or 1 are skipped.

### Reported line sizes

`cache_instruction_line_size()` and `cache_data_line_size()` are derived from
`CTR_EL0`, not from `CCSIDR_EL1`:

| Field | Bits | Meaning |
| --- | --- | --- |
| `IminLine` | [3:0] | log2 of words in the smallest instruction cache line |
| `DminLine` | [19:16] | log2 of words in the smallest data/unified cache line |

Both encode words, and one word is four bytes, so:

```c
line_size_bytes = 4 << encoding;
```

`CTR_EL0` reports the *minimum* line size across the hierarchy, which is exactly
what cache-maintenance loops need: a stride that is safe for every level. The
observed QEMU `cortex-a72` reports 64 bytes for both.

Both accessors return 0 if `cache_init()` has not completed successfully, so 0
is the "unknown" value rather than a real line size.

## `CCSIDR_EL1` and the CCIDX question

`CCSIDR_EL1` has two possible layouts, selected by
`ID_AA64MMFR2_EL1.CCIDX` (bits [23:20]):

| CCIDX | Layout |
| --- | --- |
| 0 | Legacy: `LineSize` [2:0], `Associativity` [12:3], `NumSets` [27:13] |
| non-zero | Extended: wider `Associativity` and `NumSets` fields |

`cache_uses_legacy_ccsidr()` reads `ID_AA64MMFR2_EL1` and returns true only for
CCIDX 0. `cache_invalidate_data_all()` **fails** if the extended format is in
use, and that failure propagates out of `cache_init()` and halts the kernel.

This is a deliberate refusal rather than a silent misparse: decoding the
extended layout with legacy field positions would compute wrong set and way
counts and leave parts of the cache un-invalidated. The current QEMU
`cortex-a72` model reports CCIDX 0, so NXU runs; a CPU with the extended
format would stop with `cache: initialization failed`.

`CSSELR_EL1` selects which cache `CCSIDR_EL1` describes:

```text
bit  0     InD    0 = data or unified, 1 = instruction
bits [3:1] Level  cache level, zero-based
```

NXU writes `level << 1`, always selecting the data/unified cache, and restores
`CSSELR_EL1 = 0` when finished so the register is left in its default state.
Each write is followed by `isb`, because the subsequent `CCSIDR_EL1` read must
observe the new selection.

## Data cache invalidation by set and way

`cache_invalidate_data_all()` walks every data or unified level and invalidates
every line with `DC ISW`.

The operand encodes level, set and way in one 64-bit value:

```text
63                                                    0
+------------------+-----------------+-------+--------+
|       way        |       set       | level |   0    |
+------------------+-----------------+-------+--------+
   at way_shift       at line_shift    [3:1]
```

The two shifts are computed from `CCSIDR_EL1`:

```c
line_shift = (ccsidr & 0x7) + 4;          /* LineSize + 4 */
way_shift  = clz(ways_minus_one);
```

`LineSize` encodes log2(words per line), so log2(bytes per line) is
`LineSize + 2`. The set field begins two bits above that, at `LineSize + 4`,
because the low bits of the operand are architecturally reserved.

The way field is right-aligned at the top of the 32-bit operand.
`CLZ(ways - 1)` gives the number of leading zeros in the maximum way index,
which is precisely the shift that places the field flush against bit 31.

Both loops count downward and use a `for(;;)` with a post-check:

```c
for (;;) {
	/* ... */
	if (set == 0U) {
		break;
	}
	set--;
}
```

This form is required because the loop variable is unsigned and the terminal
value is 0; a conventional `while (set >= 0)` would never terminate.

### Safety precondition

```c
if ((cache_read_sctlr() & CACHE_SCTLR_C) != 0ULL) {
	return false;
}
```

`DC ISW` **invalidates without cleaning**. Any dirty line is discarded, not
written back. Running it against a live data cache would silently destroy
recently written data. `cache_invalidate_data_all()` therefore refuses to run
unless `SCTLR_EL1.C` is clear.

This is why `cache_init()` treats a partially enabled state as an error: if
exactly one of `C` and `I` is set, it cannot safely invalidate and cannot assume
the other cache is clean, so it returns `false`. If *both* are already set, it
accepts the state as an already-completed initialization, records the discovery
data and returns success without touching anything.

## Instruction cache invalidation

```asm
ic iallu
dsb sy
isb
```

`IC IALLU` invalidates the entire instruction cache to the Point of Unification.
Unlike `DC ISW` this is unconditionally safe, because an instruction cache is
never dirty — nothing writes through it.

## Barriers

Cache maintenance needs explicit ordering; the required barriers and their
reasons:

| Location | Barrier | Reason |
| --- | --- | --- |
| Before the set/way walk | `dsb sy` | Complete earlier (uncached) stores before maintenance begins |
| After each `CSSELR_EL1` write | `isb` | The following `CCSIDR_EL1` read must see the new selection |
| After the set/way walk | `dsb sy`, `isb` | All `DC ISW` operations complete and are visible |
| After `IC IALLU` | `dsb sy`, `isb` | Invalidation completes; the pipeline refetches |
| Around the `SCTLR_EL1` write | `dsb sy` before, `isb` after | Prior maintenance completes; later instructions execute under the new configuration |

`isb` is what makes a context-changing system-register write take effect for
subsequent instructions. Without it after the `SCTLR_EL1` write, instructions
already in the pipeline could execute with the old cache configuration.

## Normal versus Device memory

Cacheability is decided per mapping, not per cache. The VMM installs two memory
types through `MAIR_EL1`:

| Type | `MAIR` index | Attribute | Used for |
| --- | --- | --- | --- |
| Normal | 0 | `0xFF` — inner and outer write-back, read/write-allocate | RAM |
| Device | 1 | `0x04` — Device-nGnRE | UART, GIC, PCI ECAM, VirtIO |

RAM is Normal cacheable because caching is the entire point: the kernel reads
and writes it constantly and the values have no meaning beyond their contents.

MMIO must be Device because a hardware register is not memory. Reading the
PL011 flag register has to actually reach the device on every read; writing the
data register has to be a single access of the right width, in program order,
and must not be repeated. Device-nGnRE provides exactly those guarantees:
non-Gathering (accesses are not merged), non-Reordering (program order is
preserved between Device accesses), Early write acknowledgement.

Mapping MMIO as Normal cacheable would break every driver in the kernel: the
UART busy-wait loop could spin on a cached flag value forever, and GIC
acknowledgements could be reordered or coalesced.

## Verification

`cache_init()` reads `SCTLR_EL1` back after the write and fails if either `C` or
`I` is clear, rather than assuming the write took effect. Only then does it set
`g_cache.initialized`.

`cache_dump()` prints `CLIDR_EL1`, `CTR_EL0`, both line sizes, the enable state
of each cache and the final `SCTLR_EL1` value.

## Concurrency and interrupt context

- `cache_init()` must be called exactly once, from single-threaded
  initialization context, with the MMU enabled. It is not reentrant.
- It must **not** be called from interrupt context.
- `cache_instruction_enabled()`, `cache_data_enabled()` and both line-size
  accessors read `SCTLR_EL1` or immutable state and are safe anywhere.
- Set/way maintenance is architecturally a local operation. It affects only the
  calling core's caches, which is one reason it is unsuitable for SMP.

## Current limitations

- **Legacy `CCSIDR_EL1` only.** A CPU reporting a non-zero
  `ID_AA64MMFR2_EL1.CCIDX` halts the kernel.
- **No by-address maintenance.** There is no `DC CVAC`, `DC CIVAC`, `DC IVAC` or
  `IC IVAU` wrapper, so NXU cannot clean or invalidate a specific range.
- **No cleaning at all.** Only invalidation is implemented, and only for the
  whole hierarchy at initialization time.
- **Set/way maintenance is core-local** and does not scale to SMP, where
  by-address operations broadcast to the inner shareable domain are required.
- `cache_instruction_line_size()` and `cache_data_line_size()` are computed and
  exposed but currently unused by any other subsystem.
- No cache is ever disabled once enabled.

## Future instruction-cache synchronization

Once NXU loads executable code at run time — an ELF binary from an initramfs,
for instance — invalidating the whole instruction cache at boot stops being
sufficient. Writing instructions goes through the data cache, while fetching
them goes through the instruction cache, and the two are not coherent. The
architecturally required sequence for each modified range is:

```text
DC CVAU  on each data cache line of the new code   (clean to PoU)
DSB ISH
IC IVAU  on each instruction cache line            (invalidate to PoU)
DSB ISH
ISB
```

This is why the reported line sizes matter: those loops must step by the value
`CTR_EL0` reports. Neither `DC CVAU` nor `IC IVAU` exists in NXU today. See
[Roadmap to userland](../roadmap-to-userland.md).

## Source files

- [`kern/arm64/cache.c`](../../kern/arm64/cache.c)
- [`kern/arm64/cache.h`](../../kern/arm64/cache.h)
- [`vm/vmm.c`](../../vm/vmm.c)

## Related documentation

- [ARM64 overview](overview.md)
- [System registers](system-registers.md)
- [Virtual memory](../vm/virtual-memory.md)
- [Translation tables](../vm/translation-tables.md)
- [Initialization](../initialization.md)
- [Roadmap to userland](../roadmap-to-userland.md)
