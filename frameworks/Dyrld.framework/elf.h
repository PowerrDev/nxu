#ifndef NXU_RUNTIME_ELF_H
#define NXU_RUNTIME_ELF_H
#include <stdint.h>

#if defined(__i386__)
#define DYR_ELF_CLASS 1
#define DYR_ELF_MACHINE 3
#define DYR_RELATIVE 8
#define DYR_RELOCATION_TAG 17
#define DYR_RELOCATION_SIZE_TAG 18
#define DYR_RELOCATION_ENTRY_TAG 19
#define DYR_SYMBOL_SHIFT 8

typedef struct {
	uint8_t ident[16];
	uint16_t type, machine;
	uint32_t version, entry, phoff, shoff, flags;
	uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstr;
} dyr_elf_header_t;

typedef struct {
	uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
} dyr_elf_program_t;

typedef struct {
	int32_t tag;
	uint32_t value;
} dyr_dynamic_t;

typedef struct {
	uint32_t name, value, size;
	uint8_t info, other;
	uint16_t section;
} dyr_symbol_t;

/* ELF32/i386 uses implicit addends stored in the relocation target. */
typedef struct {
	uint32_t offset, info;
} dyr_rela_t;
#else
#define DYR_ELF_CLASS 2
#define DYR_ELF_MACHINE 183
#define DYR_RELATIVE 1027
#define DYR_RELOCATION_TAG 7
#define DYR_RELOCATION_SIZE_TAG 8
#define DYR_RELOCATION_ENTRY_TAG 9
#define DYR_SYMBOL_SHIFT 32

typedef struct {
	uint8_t ident[16];
	uint16_t type, machine;
	uint32_t version;
	uint64_t entry, phoff, shoff;
	uint32_t flags;
	uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstr;
} dyr_elf_header_t;

typedef struct {
	uint32_t type, flags;
	uint64_t offset, vaddr, paddr, filesz, memsz, align;
} dyr_elf_program_t;

typedef struct {
	int64_t tag;
	uint64_t value;
} dyr_dynamic_t;

typedef struct {
	uint32_t name;
	uint8_t info, other;
	uint16_t section;
	uint64_t value, size;
} dyr_symbol_t;

typedef struct {
	uint64_t offset, info;
	int64_t addend;
} dyr_rela_t;
#endif
#endif
