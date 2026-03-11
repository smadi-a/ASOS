/*
 * kernel/elf.c — ELF64 parser and loader.
 *
 * Validates and loads ELF64 executables into a user address space.
 * Works entirely on an in-memory buffer — does not touch the filesystem.
 */

#include "elf.h"
#include "serial.h"
#include "vmm.h"
#include "pmm.h"
#include "string.h"
#include <stdint.h>

/* ── Tiny serial printers ───────────────────────────────────────────────*/

static void elf_put_dec(uint64_t v)
{
    if (v == 0) { serial_putc('0'); return; }
    char tmp[20];
    int i = 0;
    while (v) { tmp[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (i--) serial_putc(tmp[i]);
}

static void elf_put_hex(uint64_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    serial_puts("0x");
    int started = 0;
    for (int i = 60; i >= 0; i -= 4) {
        int d = (int)((v >> i) & 0xF);
        if (d || started || i == 0) {
            serial_putc(hex[d]);
            started = 1;
        }
    }
}

/* ── User address limit ─────────────────────────────────────────────────*/

#define USER_ADDR_LIMIT  0x0000800000000000ULL

/* ── Validation ─────────────────────────────────────────────────────────*/

bool elf_validate(const void *elf_data, size_t size)
{
    if (size < sizeof(elf64_ehdr_t)) {
        serial_puts("[ELF] Too small for ELF header\n");
        return false;
    }

    const elf64_ehdr_t *ehdr = (const elf64_ehdr_t *)elf_data;

    /* Magic bytes. */
    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        serial_puts("[ELF] Bad magic\n");
        return false;
    }

    if (ehdr->e_ident[4] != ELFCLASS64) {
        serial_puts("[ELF] Not 64-bit\n");
        return false;
    }

    if (ehdr->e_ident[5] != ELFDATA2LSB) {
        serial_puts("[ELF] Not little-endian\n");
        return false;
    }

    if (ehdr->e_type != ET_EXEC) {
        serial_puts("[ELF] Not an executable (e_type=");
        elf_put_dec(ehdr->e_type);
        serial_puts(")\n");
        return false;
    }

    if (ehdr->e_machine != EM_X86_64) {
        serial_puts("[ELF] Not x86_64 (e_machine=");
        elf_put_hex(ehdr->e_machine);
        serial_puts(")\n");
        return false;
    }

    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        serial_puts("[ELF] No program headers\n");
        return false;
    }

    uint64_t ph_end = ehdr->e_phoff + (uint64_t)ehdr->e_phnum * ehdr->e_phentsize;
    if (ph_end > size) {
        serial_puts("[ELF] Program headers extend beyond file\n");
        return false;
    }

    serial_puts("[ELF] Validated: x86_64 executable, entry=");
    elf_put_hex(ehdr->e_entry);
    serial_puts(", ");
    elf_put_dec(ehdr->e_phnum);
    serial_puts(" segments\n");

    return true;
}

/* ── Loading ────────────────────────────────────────────────────────────*/

uint64_t elf_load(const void *elf_data, size_t size, uint64_t pml4_phys,
                  uint64_t *highest_addr_out)
{
    if (!elf_validate(elf_data, size))
        return 0;

    const elf64_ehdr_t *ehdr = (const elf64_ehdr_t *)elf_data;
    const uint8_t *file = (const uint8_t *)elf_data;
    uint64_t highest_addr = 0;

    for (uint16_t seg = 0; seg < ehdr->e_phnum; seg++) {
        const elf64_phdr_t *phdr = (const elf64_phdr_t *)
            (file + ehdr->e_phoff + (uint64_t)seg * ehdr->e_phentsize);

        if (phdr->p_type != PT_LOAD)
            continue;

        /* ── Validate segment ───────────────────────────────────────── */

        if (phdr->p_offset + phdr->p_filesz < phdr->p_offset ||
            phdr->p_offset + phdr->p_filesz > size) {
            serial_puts("[ELF] Segment data extends beyond file\n");
            return 0;
        }

        if (phdr->p_memsz < phdr->p_filesz) {
            serial_puts("[ELF] memsz < filesz\n");
            return 0;
        }

        if (phdr->p_vaddr >= USER_ADDR_LIMIT) {
            serial_puts("[ELF] Segment vaddr outside user space\n");
            return 0;
        }

        if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr ||
            phdr->p_vaddr + phdr->p_memsz > USER_ADDR_LIMIT) {
            serial_puts("[ELF] Segment extends beyond user space\n");
            return 0;
        }

        /* ── Determine page flags ───────────────────────────────────── */

        uint64_t flags = PTE_PRESENT | PTE_USER;
        if (phdr->p_flags & PF_W)
            flags |= PTE_WRITABLE;
        if (!(phdr->p_flags & PF_X))
            flags |= PTE_NO_EXEC;

        /* ── Calculate page range ───────────────────────────────────── */

        uint64_t start_page = phdr->p_vaddr & ~0xFFFULL;
        uint64_t end_page   = (phdr->p_vaddr + phdr->p_memsz + 0xFFF) & ~0xFFFULL;

        /* ── Allocate, map, and copy ────────────────────────────────── */

        for (uint64_t page = start_page; page < end_page; page += 4096) {
            uint64_t frame = pmm_alloc_frame();     /* Returns zeroed */
            vmm_map_user_page(pml4_phys, page, frame, flags);

            /* Copy file data that falls within this page. */
            uint64_t page_end = page + 4096;

            uint64_t copy_start = (phdr->p_vaddr > page)
                                    ? phdr->p_vaddr : page;
            uint64_t copy_end   = (phdr->p_vaddr + phdr->p_filesz < page_end)
                                    ? (phdr->p_vaddr + phdr->p_filesz) : page_end;

            if (copy_start < copy_end) {
                uint64_t dest_off = copy_start - page;
                uint64_t src_off  = copy_start - phdr->p_vaddr;
                uint64_t len      = copy_end - copy_start;
                memcpy((void *)(uintptr_t)(frame + dest_off),
                       file + phdr->p_offset + src_off,
                       len);
            }
        }

        /* ── Log ────────────────────────────────────────────────────── */

        serial_puts("[ELF] Loaded segment: vaddr=");
        elf_put_hex(phdr->p_vaddr);
        serial_puts(", filesz=");
        elf_put_hex(phdr->p_filesz);
        serial_puts(", memsz=");
        elf_put_hex(phdr->p_memsz);
        serial_puts(", flags=");
        serial_putc((phdr->p_flags & PF_R) ? 'R' : '-');
        serial_putc((phdr->p_flags & PF_W) ? 'W' : '-');
        serial_putc((phdr->p_flags & PF_X) ? 'X' : '-');
        serial_putc('\n');

        /* Track highest mapped address. */
        uint64_t seg_end = (phdr->p_vaddr + phdr->p_memsz + 0xFFF) & ~0xFFFULL;
        if (seg_end > highest_addr)
            highest_addr = seg_end;
    }

    if (highest_addr_out)
        *highest_addr_out = highest_addr;

    return ehdr->e_entry;
}
