/*
 * kernel/elf.h — ELF64 structures and constants (freestanding).
 *
 * Defines everything needed to parse and load ELF64 executables.
 * No system headers — all types defined locally.
 */

#ifndef ELF_H
#define ELF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── ELF identification constants ────────────────────────────────────────*/

#define ELF_MAGIC       0x464C457FU   /* "\x7FELF" as little-endian uint32 */
#define ELFCLASS64      2
#define ELFDATA2LSB     1             /* Little-endian */
#define ET_EXEC         2             /* Executable file */
#define EM_X86_64       0x3E
#define PT_LOAD         1
#define PF_X            0x1
#define PF_W            0x2
#define PF_R            0x4

/* ── ELF64 Header (64 bytes) ────────────────────────────────────────────*/

typedef struct {
    uint8_t  e_ident[16];       /* Magic, class, endian, version, OS/ABI */
    uint16_t e_type;            /* Object type (ET_EXEC = 2)             */
    uint16_t e_machine;         /* Architecture (EM_X86_64 = 0x3E)       */
    uint32_t e_version;         /* ELF version                           */
    uint64_t e_entry;           /* Entry point virtual address           */
    uint64_t e_phoff;           /* Program header table offset           */
    uint64_t e_shoff;           /* Section header table offset           */
    uint32_t e_flags;           /* Processor flags                       */
    uint16_t e_ehsize;          /* ELF header size                       */
    uint16_t e_phentsize;       /* Program header entry size             */
    uint16_t e_phnum;           /* Number of program headers             */
    uint16_t e_shentsize;       /* Section header entry size             */
    uint16_t e_shnum;           /* Number of section headers             */
    uint16_t e_shstrndx;        /* Section name string table index       */
} __attribute__((packed)) elf64_ehdr_t;

/* ── ELF64 Program Header (56 bytes) ────────────────────────────────────*/

typedef struct {
    uint32_t p_type;            /* Segment type (PT_LOAD = 1)            */
    uint32_t p_flags;           /* Segment flags (PF_X=1, PF_W=2, PF_R=4) */
    uint64_t p_offset;          /* Offset in file                        */
    uint64_t p_vaddr;           /* Virtual address in memory             */
    uint64_t p_paddr;           /* Physical address (ignored)            */
    uint64_t p_filesz;          /* Size in file                          */
    uint64_t p_memsz;           /* Size in memory (≥ p_filesz; excess=BSS) */
    uint64_t p_align;           /* Alignment                             */
} __attribute__((packed)) elf64_phdr_t;

/* ── Public API ─────────────────────────────────────────────────────────*/

/*
 * Validate that 'elf_data' (of 'size' bytes) is a well-formed ELF64
 * x86-64 executable.  Prints diagnostic failures to serial.
 */
bool elf_validate(const void *elf_data, size_t size);

/*
 * Load an ELF64 executable into the address space rooted at pml4_phys.
 * Maps PT_LOAD segments with correct page flags.
 *
 * Returns the entry point virtual address, or 0 on failure.
 */
uint64_t elf_load(const void *elf_data, size_t size, uint64_t pml4_phys);

#endif /* ELF_H */
