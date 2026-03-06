/*
 * bootloader/main.c — ASOS UEFI Bootloader (Milestone 3: higher-half)
 *
 * Responsibilities:
 *   1. Locate and initialise the GOP framebuffer.
 *   2. Load \EFI\asos\kernel.elf to its physical addresses (from p_paddr).
 *   3. Collect the UEFI memory map and exit boot services.
 *   4. Set up minimal page tables:
 *        • Identity map first 4 GB (1 GB pages, including page 0).
 *        • Higher-half: map 0xFFFFFFFF80000000 → physical 0x0 (1 GB page).
 *   5. Switch CR3 to those page tables.
 *   6. Jump to the kernel entry point at its virtual address.
 *
 * After step 5 the CPU can address both low physical memory (identity map)
 * and the higher-half kernel virtual addresses simultaneously.  The kernel
 * then replaces these minimal tables with its own fine-grained ones in
 * vmm_init().
 */

#include <efi.h>
#include <efilib.h>

#include "../shared/boot_info.h"

/* ── ELF64 structures ─────────────────────────────────────────────────────*/

#define ELF_MAGIC   0x464C457FU
#define ET_EXEC     2
#define EM_X86_64   62
#define PT_LOAD     1

typedef struct {
    UINT8  e_ident[16];
    UINT16 e_type;
    UINT16 e_machine;
    UINT32 e_version;
    UINT64 e_entry;
    UINT64 e_phoff;
    UINT64 e_shoff;
    UINT32 e_flags;
    UINT16 e_ehsize;
    UINT16 e_phentsize;
    UINT16 e_phnum;
    UINT16 e_shentsize;
    UINT16 e_shnum;
    UINT16 e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    UINT32 p_type;
    UINT32 p_flags;
    UINT64 p_offset;
    UINT64 p_vaddr;
    UINT64 p_paddr;
    UINT64 p_filesz;
    UINT64 p_memsz;
    UINT64 p_align;
} Elf64_Phdr;

/* ── Utility ──────────────────────────────────────────────────────────────*/

__attribute__((noreturn))
static void halt(void)
{
    for (;;) __asm__ volatile ("cli; hlt");
}

static void check(EFI_STATUS status, const CHAR16 *msg)
{
    if (EFI_ERROR(status)) {
        Print(L"\r\nFATAL: %s\r\n  EFI status: 0x%lx\r\n", msg, status);
        halt();
    }
}

/* ── Minimal page tables (static, 4 KB-aligned) ──────────────────────────
 *
 * Three 4 KB tables:
 *   boot_pml4      — root PML4
 *   boot_pdpt_low  — covers low 512 GB (PML4[0]); we use entries [0..3]
 *   boot_pdpt_high — covers high 512 GB (PML4[511]); we use entry [510]
 *
 * All three are static so they live in the bootloader's EfiLoaderData
 * pages, which persist after ExitBootServices().
 *
 * PML4[0]    → boot_pdpt_low:
 *   [0] = PA 0x0_0000_0000  (1 GB, present+writable+pagesize)
 *   [1] = PA 0x4000_0000    (1 GB)
 *   [2] = PA 0x8000_0000    (1 GB)
 *   [3] = PA 0xC000_0000    (1 GB)
 *
 * PML4[511]  → boot_pdpt_high:
 *   [510] = PA 0x0  (1 GB, maps VA 0xFFFFFFFF80000000 → PA 0x0)
 *
 * With these tables virtual 0xFFFFFFFF80100000 == physical 0x100000
 * (where the kernel is loaded), which matches the linker VMA.
 */

#define PAGE_PRESENT   (1ULL << 0)
#define PAGE_WRITABLE  (1ULL << 1)
#define PAGE_SIZE_BIT  (1ULL << 7)   /* 1 GB page at PDPT level */

static UINT64 boot_pml4     [512] __attribute__((aligned(4096)));
static UINT64 boot_pdpt_low [512] __attribute__((aligned(4096)));
static UINT64 boot_pdpt_high[512] __attribute__((aligned(4096)));

static void setup_page_tables(void)
{
    /* Zero the tables (they're in BSS but zero explicitly for clarity). */
    for (int i = 0; i < 512; i++) {
        boot_pml4[i]      = 0;
        boot_pdpt_low[i]  = 0;
        boot_pdpt_high[i] = 0;
    }

    /* PML4[0] → boot_pdpt_low */
    boot_pml4[0] = (UINT64)(UINTN)boot_pdpt_low | PAGE_WRITABLE | PAGE_PRESENT;

    /* PML4[511] → boot_pdpt_high */
    boot_pml4[511] = (UINT64)(UINTN)boot_pdpt_high | PAGE_WRITABLE | PAGE_PRESENT;

    /* Identity map first 4 GB: four 1 GB pages. */
    for (int gb = 0; gb < 4; gb++) {
        boot_pdpt_low[gb] = ((UINT64)gb << 30)
                          | PAGE_SIZE_BIT | PAGE_WRITABLE | PAGE_PRESENT;
    }

    /*
     * Higher-half: PDPT_high[510] maps the 1 GB starting at
     * VA 0xFFFFFFFF80000000 to PA 0x0.
     *
     * PML4[511] covers VAs 0xFFFF800000000000 – 0xFFFFFFFFFFFFFFFF.
     * Within that, PDPT index 510 covers the 1 GB:
     *   0xFFFFFFFF80000000 – 0xFFFFFFFFBFFFFFFF
     *
     * Mapping it to PA 0x0 means virtual 0xFFFFFFFF80100000 → PA 0x100000.
     */
    boot_pdpt_high[510] = 0ULL | PAGE_SIZE_BIT | PAGE_WRITABLE | PAGE_PRESENT;
}

/* ── Stage 1: GOP ────────────────────────────────────────────────────────*/

static void init_gop(Framebuffer *fb)
{
    EFI_STATUS status;
    EFI_GUID   gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;

    Print(L"[boot] Locating GOP...\r\n");

    status = uefi_call_wrapper(BS->LocateProtocol, 3,
                               &gop_guid, NULL, (VOID **)&gop);
    check(status, L"LocateProtocol(GOP)");

    UINT32  best_mode  = gop->Mode->Mode;
    BOOLEAN found_pref = FALSE;

    for (UINT32 i = 0; i < gop->Mode->MaxMode; i++) {
        UINTN info_sz;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;

        status = uefi_call_wrapper(gop->QueryMode, 4, gop, i, &info_sz, &info);
        if (EFI_ERROR(status)) continue;

        if (info->HorizontalResolution == 1024 &&
            info->VerticalResolution   == 768  &&
            (info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor ||
             info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor)) {
            best_mode  = i;
            found_pref = TRUE;
            break;
        }
    }

    if (!found_pref)
        Print(L"[boot] 1024x768 not found — using mode %u\r\n", best_mode);

    if (best_mode != gop->Mode->Mode) {
        status = uefi_call_wrapper(gop->SetMode, 2, gop, best_mode);
        check(status, L"GOP SetMode");
    }

    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = gop->Mode->Info;

    fb->base   = (UINT64)gop->Mode->FrameBufferBase;
    fb->width  = info->HorizontalResolution;
    fb->height = info->VerticalResolution;
    fb->pitch  = info->PixelsPerScanLine * 4;
    fb->format = (info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor)
                 ? PIXEL_FORMAT_BGR : PIXEL_FORMAT_RGB;

    Print(L"[boot] GOP: %ux%u pitch=%u base=0x%lx\r\n",
          fb->width, fb->height, fb->pitch, fb->base);
}

/* ── Stage 2: kernel ELF loading ─────────────────────────────────────────*/

static UINT64 load_kernel(EFI_HANDLE ImageHandle)
{
    EFI_STATUS status;
    EFI_GUID lip_guid  = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID sfsp_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

    EFI_LOADED_IMAGE_PROTOCOL       *loaded_image;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_FILE_PROTOCOL               *root, *kfile;

    Print(L"[boot] Opening kernel file...\r\n");

    status = uefi_call_wrapper(BS->HandleProtocol, 3,
                               ImageHandle, &lip_guid,
                               (VOID **)&loaded_image);
    check(status, L"HandleProtocol(LoadedImage)");

    status = uefi_call_wrapper(BS->HandleProtocol, 3,
                               loaded_image->DeviceHandle, &sfsp_guid,
                               (VOID **)&fs);
    check(status, L"HandleProtocol(SimpleFileSystem)");

    status = uefi_call_wrapper(fs->OpenVolume, 2, fs, &root);
    check(status, L"OpenVolume");

    status = uefi_call_wrapper(root->Open, 5, root, &kfile,
                               L"\\EFI\\asos\\kernel.elf",
                               EFI_FILE_MODE_READ, 0ULL);
    check(status, L"Open(kernel.elf)");

    Elf64_Ehdr ehdr;
    UINTN      ehdr_sz = sizeof(ehdr);

    status = uefi_call_wrapper(kfile->Read, 3, kfile, &ehdr_sz, &ehdr);
    check(status, L"Read(ELF header)");

    if (*(UINT32 *)ehdr.e_ident != ELF_MAGIC) {
        Print(L"FATAL: bad ELF magic\r\n"); halt();
    }
    if (ehdr.e_machine != EM_X86_64) {
        Print(L"FATAL: not x86-64\r\n"); halt();
    }
    if (ehdr.e_type != ET_EXEC) {
        Print(L"FATAL: not executable\r\n"); halt();
    }

    Print(L"[boot] ELF entry=0x%lx  phdrs=%u\r\n",
          ehdr.e_entry, (UINT32)ehdr.e_phnum);

    UINTN      phdrs_sz = (UINTN)ehdr.e_phnum * sizeof(Elf64_Phdr);
    Elf64_Phdr *phdrs;

    status = uefi_call_wrapper(BS->AllocatePool, 3,
                               EfiLoaderData, phdrs_sz, (VOID **)&phdrs);
    check(status, L"AllocatePool(phdrs)");

    status = uefi_call_wrapper(kfile->SetPosition, 2, kfile, ehdr.e_phoff);
    check(status, L"SetPosition(e_phoff)");

    status = uefi_call_wrapper(kfile->Read, 3, kfile, &phdrs_sz, phdrs);
    check(status, L"Read(phdrs)");

    for (UINT16 i = 0; i < ehdr.e_phnum; i++) {
        Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) continue;

        Print(L"[boot]   LOAD[%u] paddr=0x%lx filesz=%lu memsz=%lu\r\n",
              (UINT32)i, ph->p_paddr, ph->p_filesz, ph->p_memsz);

        /* Load to the physical address (p_paddr). */
        UINTN                pages = (UINTN)((ph->p_memsz + 0xFFFULL) >> 12);
        EFI_PHYSICAL_ADDRESS phys  = (EFI_PHYSICAL_ADDRESS)ph->p_paddr;

        status = uefi_call_wrapper(BS->AllocatePages, 4,
                                   AllocateAddress, EfiLoaderData,
                                   pages, &phys);
        check(status, L"AllocatePages(segment)");

        UINT8 *dest = (UINT8 *)(UINTN)ph->p_paddr;
        for (UINT64 j = 0; j < ph->p_memsz; j++) dest[j] = 0;

        status = uefi_call_wrapper(kfile->SetPosition, 2, kfile, ph->p_offset);
        check(status, L"SetPosition(segment)");

        UINTN filesz = (UINTN)ph->p_filesz;
        status = uefi_call_wrapper(kfile->Read, 3, kfile, &filesz, dest);
        check(status, L"Read(segment)");
    }

    uefi_call_wrapper(BS->FreePool, 1, phdrs);
    uefi_call_wrapper(kfile->Close, 1, kfile);
    uefi_call_wrapper(root->Close, 1, root);

    Print(L"[boot] Kernel loaded. Entry=0x%lx\r\n", ehdr.e_entry);
    return ehdr.e_entry;
}

/* ── Stage 3: memory map + ExitBootServices ──────────────────────────────*/

static void collect_mmap_and_exit(EFI_HANDLE ImageHandle, MemoryMap *mm)
{
    EFI_STATUS status;
    UINTN      map_size = 0, map_key, desc_size;
    UINT32     desc_ver;
    VOID      *buf = NULL;

    uefi_call_wrapper(BS->GetMemoryMap, 5,
                      &map_size, buf, &map_key, &desc_size, &desc_ver);
    map_size += 4 * desc_size;

    status = uefi_call_wrapper(BS->AllocatePool, 3,
                               EfiLoaderData, map_size, &buf);
    check(status, L"AllocatePool(mmap)");

    status = uefi_call_wrapper(BS->GetMemoryMap, 5,
                               &map_size, buf, &map_key, &desc_size, &desc_ver);
    check(status, L"GetMemoryMap");

    mm->map                = buf;
    mm->map_size           = (UINT64)map_size;
    mm->descriptor_size    = (UINT64)desc_size;
    mm->descriptor_version = desc_ver;

    Print(L"[boot] Memory map: %lu bytes, %lu entries.\r\n",
          mm->map_size, mm->map_size / mm->descriptor_size);
    Print(L"[boot] Exiting boot services...\r\n");

    status = uefi_call_wrapper(BS->ExitBootServices, 2, ImageHandle, map_key);
    if (EFI_ERROR(status)) {
        status = uefi_call_wrapper(BS->GetMemoryMap, 5,
                                   &map_size, buf, &map_key,
                                   &desc_size, &desc_ver);
        mm->map_size = (UINT64)map_size;
        status = uefi_call_wrapper(BS->ExitBootServices, 2,
                                   ImageHandle, map_key);
        if (EFI_ERROR(status)) halt();
    }
}

/* ── UEFI entry point ─────────────────────────────────────────────────────*/

EFI_STATUS
efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    InitializeLib(ImageHandle, SystemTable);

    Print(L"\r\n============================\r\n");
    Print(L"  ASOS Bootloader (M3)\r\n");
    Print(L"============================\r\n\r\n");

    static BootInfo boot_info;

    init_gop(&boot_info.framebuffer);
    UINT64 kernel_entry = load_kernel(ImageHandle);
    collect_mmap_and_exit(ImageHandle, &boot_info.memory_map);

    /*
     * Boot services are now gone.  Set up the minimal higher-half page
     * tables and switch CR3 before jumping to the kernel virtual entry.
     *
     * After this point: no UEFI calls, no Print().
     */
    setup_page_tables();

    __asm__ volatile (
        "mov %0, %%cr3"
        :: "r"((UINT64)(UINTN)boot_pml4)
        : "memory"
    );

    /*
     * Jump to the kernel entry point.
     * kernel_entry is a higher-half virtual address (0xFFFFFFFF80XXXXXX).
     * Our new page tables map it correctly via boot_pdpt_high[510].
     */
    typedef void (*KernelEntry)(BootInfo *);
    KernelEntry kernel = (KernelEntry)(UINTN)kernel_entry;
    kernel(&boot_info);

    halt();
}
