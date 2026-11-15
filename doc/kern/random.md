# Random numbers

`kern/security/random.c` is the kernel's own source of random bytes. It needs
nothing outside the machine NXU runs on. It replaces tepOS's `RANDOM` command,
which the loader and the setup assistant used before tepOS became optional
([Trusted Enclave Mailbox](../drivers/tep-mailbox.md)).

| Caller | For |
|--------|-----|
| `kern/loader/elf.c` | ASLR: each dynamically loaded image's slide (`dyrld.aslr=0` turns slides off, for debugging) |
| `kern/security/passcode.c` | The login passcode's salt |
| `drivers/video/setup_assistant.c` | The salt of the new account's `/etc/shadow` line |

## Interface

```c
void random_init(void);                                  /* at boot; random_bytes calls it if nobody has */
void random_bytes(void *out, size_t size);               /* never fails */
void random_add_entropy(const void *data, size_t size);  /* mix in more */
bool random_seed_refresh(void);                          /* the seed file, once the system volume is mounted */
```

`random_init` runs at boot on both architectures: in `kern_init` on arm64,
in `devices_init` on i386, and logs "initializing" and "initialized ...
successfully" lines.

## How it works

A SHA-256 hash DRBG: a 32-byte key and a 64-bit counter, under a spinlock.

- **Output:** block *i* of a request is
  SHA-256("out" ‖ key ‖ counter++ ‖ cycle counter).
- **Forward secrecy:** after every request the key becomes
  SHA-256("next" ‖ key ‖ counter++), so bytes already handed out cannot be
  recomputed from a later state.
- **Entropy:** hashed into the key.
  - At init: 16 words from the CPU's random number instruction when it has
    one (RDRAND, CPUID.1:ECX bit 30, on x86; RNDR, ID_AA64ISAR0_EL1 bits
    63:60, on arm64), 32 bytes folded from the jitter of the cycle counter
    over a timed loop, the RTC, and a stack address.
  - Every request: the cycle counter, plus one more word from the CPU
    instruction when there is one.
  - Once per boot, the first time the system volume is there
    (`random_seed_refresh`, called by the passcode store): the 64 bytes the
    previous boot left in `/var/db/random-seed` (root, 0600), then a new
    seed for the next boot. Two boots of the same disk image therefore do
    not start alike even without a CPU instruction.

## Limits

With a CPU instruction (HVF with `-cpu host` on a recent Mac, for example)
the seed is hardware-grade. Without one (QEMU's `qemu32`/`qemu64` and
`cortex-a72` models) it is the counter jitter and the stored seed: good enough
for ASLR slides and salts, not a generator to make long-term keys with. A
virtio-rng driver would be the next source to add. Userland has no
`getentropy` call yet.
