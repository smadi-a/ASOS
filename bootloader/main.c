/*
 * bootloader/main.c — ASOS UEFI Bootloader
 *
 * This is a PE32+ UEFI application built with GNU-EFI.  It runs before the
 * OS kernel and is responsible for:
 *
 *   1. Locating and initialising the GOP framebuffer.
 *   2. Opening \EFI\asos\kernel.elf from the same ESP volume.
 *   3. Parsing the ELF64 header and loading every PT_LOAD segment to its
 *      physical address.
 *   4. Collecting the UEFI memory map.
 *   5. Calling ExitBootServices() to hand control to the kernel.
 *   6. Jumping to the ELF entry point with a BootInfo pointer.
 *
 * Build: see the top-level Makefile.
 *
 * Design notes
 * ─────────────
 * • We deliberately avoid C99 VLAs; all dynamic allocations use
 *   AllocatePool / AllocatePages so the UEFI firmware tracks them.
 * • Error handling is "print and halt" — this is a bootloader, not a kernel.
 *   After ExitBootServices() we can no longer call Print(), so failures there
 *   simply halt the CPU.
 * • Byte-for-byte copies use our own loop rather than gnu-efi CopyMem so
 *   the dependency set stays minimal.
 */

#include <efi.h>
#include <efilib.h>

#include "../shared/boot_info.h"

/* ── ELF64 structures ─────────────────────────────────────────────────────
 *
 * We define our own rather than pulling in <elf.h> from the host system,
 * which may not be present in every cross-build environment.
 */

#define ELF_MAGIC   0x464C457FU  /* little-endian representation of "\x7FELF" */
#define ET_EXEC     2            /* executable file                            */
#define EM_X86_64   62           /* AMD x86-64 machine type                    */
#define PT_LOAD     1            /* loadable segment                           */

/* 64-bit ELF file header (Elf64_Ehdr) */
typedef struct {
    UINT8  e_ident[16]; /* Magic, class, data encoding, version, OS/ABI …    */
    UINT16 e_type;      /* Object file type (ET_EXEC, ET_DYN, …)             */
    UINT16 e_machine;   /* Target ISA (EM_X86_64 = 62)                       */
    UINT32 e_version;   /* ELF version (always 1)                            */
    UINT64 e_entry;     /* Virtual address of entry point                    */
    UINT64 e_phoff;     /* Offset of program header table in file            */
    UINT64 e_shoff;     /* Offset of section header table in file            */
    UINT32 e_flags;     /* Processor-specific flags                          */
    UINT16 e_ehsize;    /* Size of this header                               */
    UINT16 e_phentsize; /* Size of one program header entry                  */
    UINT16 e_phnum;     /* Number of program header entries                  */
    UINT16 e_shentsize; /* Size of one section header entry                  */
    UINT16 e_shnum;     /* Number of section header entries                  */
    UINT16 e_shstrndx;  /* Index of section name string table                */
} Elf64_Ehdr;

/* 64-bit ELF program header (Elf64_Phdr) */
typedef struct {
    UINT32 p_type;   /* Segment type (PT_LOAD, PT_DYNAMIC, …)               */
    UINT32 p_flags;  /* Segment flags (PF_R / PF_W / PF_X)                  */
    UINT64 p_offset; /* Byte offset of segment data in the file              */
    UINT64 p_vaddr;  /* Virtual address in memory                            */
    UINT64 p_paddr;  /* Physical address (used directly here, pre-paging)    */
    UINT64 p_filesz; /* Bytes of data to copy from the file                  */
    UINT64 p_memsz;  /* Bytes to map in memory (>= p_filesz; excess = .bss) */
    UINT64 p_align;  /* Alignment constraint (power of 2)                    */
} Elf64_Phdr;

/* ── Utility: spin forever ────────────────────────────────────────────────*/
__attribute__((noreturn))
static void halt(void)
{
    for (;;)
        __asm__ volatile ("cli; hlt");
}

/* ── Utility: check EFI status, print and halt on error ──────────────────*/
static void check(EFI_STATUS status, const CHAR16 *msg)
{
    if (EFI_ERROR(status)) {
        Print(L"\r\nFATAL: %s\r\n  EFI status: 0x%lx\r\n", msg, status);
        halt();
    }
}

/* ── Stage 1: GOP framebuffer initialisation ──────────────────────────────
 *
 * We ask GOP for every available video mode and pick the first one that
 * matches our preferred resolution (1024×768, 32 bpp).  If none matches we
 * fall back to whatever is already active.
 */
static void init_gop(Framebuffer *fb)
{
    EFI_STATUS status;
    EFI_GUID   gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;

    Print(L"[boot] Locating GOP...\r\n");

    status = uefi_call_wrapper(BS->LocateProtocol, 3,
                               &gop_guid, NULL, (VOID **)&gop);
    check(status, L"LocateProtocol(GOP)");

    /* Search for the preferred 1024×768 32-bit mode. */
    UINT32   best_mode     = gop->Mode->Mode;
    BOOLEAN  found_pref    = FALSE;

    for (UINT32 i = 0; i < gop->Mode->MaxMode; i++) {
        UINTN info_sz;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;

        status = uefi_call_wrapper(gop->QueryMode, 4, gop, i, &info_sz, &info);
        if (EFI_ERROR(status))
            continue;

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
        Print(L"[boot] Preferred 1024x768 mode not found — using mode %u\r\n",
              best_mode);

    /* Switch to the chosen mode only if it is not already active. */
    if (best_mode != gop->Mode->Mode) {
        status = uefi_call_wrapper(gop->SetMode, 2, gop, best_mode);
        check(status, L"GOP SetMode");
    }

    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = gop->Mode->Info;

    fb->base   = (UINT64)gop->Mode->FrameBufferBase;
    fb->width  = info->HorizontalResolution;
    fb->height = info->VerticalResolution;
    fb->pitch  = info->PixelsPerScanLine * 4; /* 32-bit (4 bytes) per pixel */
    fb->format = (info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor)
                 ? PIXEL_FORMAT_BGR
                 : PIXEL_FORMAT_RGB;

    Print(L"[boot] GOP: %ux%u pitch=%u fmt=%s base=0x%lx\r\n",
          fb->width, fb->height, fb->pitch,
          (fb->format == PIXEL_FORMAT_BGR) ? L"BGR" : L"RGB",
          fb->base);
}

/* ── Stage 2: kernel ELF loading ──────────────────────────────────────────
 *
 * Opens \EFI\asos\kernel.elf on the same volume the bootloader was loaded
 * from, validates the ELF64 header, then iterates over PT_LOAD segments and
 * maps each one to its specified physical address using AllocatePages().
 *
 * Returns the ELF entry point address.
 */
static UINT64 load_kernel(EFI_HANDLE ImageHandle)
{
    EFI_STATUS status;

    /* ── 2a. Get a file system handle on our boot volume ─────────────── */
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
    check(status, L"Open(\\EFI\\asos\\kernel.elf)");

    /* ── 2b. Read and validate the ELF header ────────────────────────── */
    Elf64_Ehdr ehdr;
    UINTN      ehdr_sz = sizeof(ehdr);

    status = uefi_call_wrapper(kfile->Read, 3, kfile, &ehdr_sz, &ehdr);
    check(status, L"Read(ELF header)");

    if (*(UINT32 *)ehdr.e_ident != ELF_MAGIC) {
        Print(L"FATAL: kernel.elf: bad ELF magic\r\n");
        halt();
    }
    if (ehdr.e_machine != EM_X86_64) {
        Print(L"FATAL: kernel.elf: not an x86-64 ELF\r\n");
        halt();
    }
    if (ehdr.e_type != ET_EXEC) {
        Print(L"FATAL: kernel.elf: not an executable (type=%u)\r\n",
              ehdr.e_type);
        halt();
    }

    Print(L"[boot] ELF valid — entry=0x%lx  phdrs=%u\r\n",
          ehdr.e_entry, (UINT32)ehdr.e_phnum);

    /* ── 2c. Read all program headers ────────────────────────────────── */
    UINTN      phdrs_sz = (UINTN)ehdr.e_phnum * sizeof(Elf64_Phdr);
    Elf64_Phdr *phdrs;

    status = uefi_call_wrapper(BS->AllocatePool, 3,
                               EfiLoaderData, phdrs_sz, (VOID **)&phdrs);
    check(status, L"AllocatePool(phdrs)");

    status = uefi_call_wrapper(kfile->SetPosition, 2, kfile, ehdr.e_phoff);
    check(status, L"SetPosition(e_phoff)");

    status = uefi_call_wrapper(kfile->Read, 3, kfile, &phdrs_sz, phdrs);
    check(status, L"Read(program headers)");

    /* ── 2d. Load each PT_LOAD segment ───────────────────────────────── */
    for (UINT16 i = 0; i < ehdr.e_phnum; i++) {
        Elf64_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD)
            continue;

        Print(L"[boot]   LOAD[%u] paddr=0x%lx filesz=%lu memsz=%lu\r\n",
              (UINT32)i, ph->p_paddr, ph->p_filesz, ph->p_memsz);

        /*
         * Reserve the physical pages that the segment will occupy.
         * AllocateAddress asks UEFI to use *exactly* this physical range,
         * so if anything collides (e.g., firmware data) this will fail
         * loudly rather than silently stomping on it.
         */
        UINTN                pages = (UINTN)((ph->p_memsz + 0xFFFULL) >> 12);
        EFI_PHYSICAL_ADDRESS phys  = (EFI_PHYSICAL_ADDRESS)ph->p_paddr;

        status = uefi_call_wrapper(BS->AllocatePages, 4,
                                   AllocateAddress, EfiLoaderData,
                                   pages, &phys);
        check(status, L"AllocatePages(segment)");

        /* Zero the entire mapped region first — this handles .bss. */
        UINT8 *dest = (UINT8 *)(UINTN)ph->p_paddr;
        for (UINT64 j = 0; j < ph->p_memsz; j++)
            dest[j] = 0;

        /* Seek to the segment's file offset and read its data. */
        status = uefi_call_wrapper(kfile->SetPosition, 2,
                                   kfile, ph->p_offset);
        check(status, L"SetPosition(segment)");

        UINTN filesz = (UINTN)ph->p_filesz;
        status = uefi_call_wrapper(kfile->Read, 3, kfile, &filesz, dest);
        check(status, L"Read(segment data)");
    }

    uefi_call_wrapper(BS->FreePool, 1, phdrs);
    uefi_call_wrapper(kfile->Close, 1, kfile);
    uefi_call_wrapper(root->Close, 1, root);

    Print(L"[boot] Kernel loaded.\r\n");
    return ehdr.e_entry;
}

/* ── Stage 3: memory map + ExitBootServices ───────────────────────────────
 *
 * UEFI mandates that we call GetMemoryMap() immediately before
 * ExitBootServices() and use the MapKey from *that* call.  Any allocation
 * between the two calls (including our own AllocatePool below) invalidates
 * the key and forces a retry.
 *
 * Strategy:
 *   1. Call GetMemoryMap() once to determine the required buffer size.
 *   2. AllocatePool() for that buffer (this invalidates the key).
 *   3. Call GetMemoryMap() again with the real buffer.
 *   4. Call ExitBootServices().  If it fails (another allocation sneaked in)
 *      do one more GetMemoryMap() + ExitBootServices().
 */
static void collect_mmap_and_exit(EFI_HANDLE ImageHandle, MemoryMap *mm)
{
    EFI_STATUS status;
    UINTN      map_size = 0, map_key, desc_size;
    UINT32     desc_ver;
    VOID      *buf = NULL;

    /* First call: query the required buffer size (will return TOO_SMALL). */
    uefi_call_wrapper(BS->GetMemoryMap, 5,
                      &map_size, buf, &map_key, &desc_size, &desc_ver);

    /* Add slack for the allocation below and any new firmware descriptors. */
    map_size += 4 * desc_size;

    status = uefi_call_wrapper(BS->AllocatePool, 3,
                               EfiLoaderData, map_size, &buf);
    check(status, L"AllocatePool(memory map)");

    /* Second call: fill the buffer; capture the key. */
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
        /*
         * The map was updated between our GetMemoryMap and ExitBootServices
         * calls (this can happen if a firmware timer callback ran).  Fetch
         * the map one more time — we MUST NOT call Print() here because
         * ConOut may already be gone on some firmware.
         */
        status = uefi_call_wrapper(BS->GetMemoryMap, 5,
                                   &map_size, buf, &map_key,
                                   &desc_size, &desc_ver);
        mm->map_size = (UINT64)map_size;

        status = uefi_call_wrapper(BS->ExitBootServices, 2,
                                   ImageHandle, map_key);
        if (EFI_ERROR(status))
            halt(); /* Cannot Print() here — firmware state is undefined. */
    }

    /* Boot services are now terminated.  Do NOT call any BS functions. */
}

/* ── UEFI entry point ─────────────────────────────────────────────────────*/
EFI_STATUS
efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    InitializeLib(ImageHandle, SystemTable);

    Print(L"\r\n============================\r\n");
    Print(L"  ASOS Bootloader\r\n");
    Print(L"============================\r\n\r\n");

    /*
     * Allocate BootInfo in static storage.  This lives inside our
     * EfiLoaderData pages, which remain mapped after ExitBootServices()
     * because we never call FreePages() on them.
     */
    static BootInfo boot_info;

    /* Stage 1: framebuffer. */
    init_gop(&boot_info.framebuffer);

    /* Stage 2: kernel ELF. */
    UINT64 kernel_entry = load_kernel(ImageHandle);

    /* Stage 3: memory map + exit. */
    collect_mmap_and_exit(ImageHandle, &boot_info.memory_map);

    /*
     * At this point boot services are gone.  Jump directly to the kernel.
     * The kernel entry point signature is:
     *
     *   void kernel_main(BootInfo *info);
     */
    typedef void (*KernelEntry)(BootInfo *);
    KernelEntry kernel = (KernelEntry)(UINTN)kernel_entry;
    kernel(&boot_info);

    /* Should never be reached. */
    halt();
}
