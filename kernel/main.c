/*
 * kernel/main.c — ASOS kernel entry point (Milestone 6C).
 *
 * Boot sequence
 * ─────────────
 *  1.  Clear BSS.
 *  2.  serial_init()
 *  3.  fb_init() + fb_clear()
 *  4.  gdt_init()
 *  5.  idt_init()
 *  6.  pmm_init()
 *  7.  Switch RSP to kernel BSS stack → call kernel_main2().
 *  8.  vmm_init()
 *  9.  heap_init()
 * 10.  ata_init()      — detect IDE drives (master only)
 * 11.  gpt_find_esp() — locate ESP start LBA on master drive
 * 12.  vfs_mount()    — mount FAT32 at ESP's LBA offset
 * 13.  FAT32 demo     — list root dir, read HELLO.TXT
 * 14.  pic_init()      — remap IRQs, mask all lines
 * 15.  pit_init()      — 1000 Hz timer, unmask IRQ 0
 * 16.  keyboard_init() — PS/2 keyboard, unmask IRQ 1
 * 17.  sti             — enable interrupts
 * 18.  scheduler_init + ring-3 user process test
 * 19.  Keyboard echo demo with uptime reporting
 *
 * IMPORTANT: kernel_main() switches RSP via inline asm.  With -O2 GCC
 * omits the frame pointer and uses RSP-relative addressing for locals.
 * After the RSP change, those RSP-relative offsets point into BSS
 * instead of the real stack frame, silently corrupting globals.
 *
 * The fix is to split the function: kernel_main() does only the early
 * init that needs the UEFI stack, switches RSP, and then calls
 * kernel_main2() (marked noinline) so the compiler builds a fresh
 * stack frame on the new stack.
 */

#include <stdint.h>
#include <stddef.h>

#include "../shared/boot_info.h"
#include "serial.h"
#include "framebuffer.h"
#include "gdt.h"
#include "idt.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "pic.h"
#include "pit.h"
#include "keyboard.h"
#include "mouse.h"
#include "string.h"
#include "ata.h"
#include "gpt.h"
#include "vfs.h"
#include "scheduler.h"
#include "process.h"
#include "syscall.h"
#include "user_syscall.h"
#include "elf.h"

#define ASOS_VERSION "ASOS v0.1.0"

extern char __bss_start[];
extern char __bss_end[];

#define KSTACK_SIZE  16384
static uint8_t g_kstack[KSTACK_SIZE] __attribute__((aligned(16)));

static void clear_bss(void)
{
    volatile char *p = __bss_start;
    while (p < __bss_end) *p++ = 0;
}

/* ── Simple decimal-to-string for uptime display ──────────────────────── */

static void serial_put_dec(uint64_t v)
{
    if (v == 0) { serial_putc('0'); return; }
    char tmp[20];
    int i = 0;
    while (v) { tmp[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (i--) serial_putc(tmp[i]);
}

/* ── User programs (copied to ring-3 pages, run via syscalls) ────────── */

/*
 * These functions are compiled into the kernel binary but copied to
 * user-accessible pages and run in ring 3.  They use syscall wrappers
 * from user_syscall.h to interact with the kernel.
 *
 * IMPORTANT: String literals live in kernel .rodata and are NOT
 * accessible from ring 3.  All strings must be stack-allocated char
 * arrays so GCC emits immediate mov instructions at -O2.
 */

/* Helper: write a decimal number to fd via syscalls.
 * Must be always_inline — user code is copied to ring-3 pages, so
 * any out-of-line call would jump to a wrong address. */
__attribute__((always_inline))
static inline void user_put_dec(int fd, uint64_t v)
{
    if (v == 0) {
        char z = '0';
        user_write(fd, &z, 1);
        return;
    }
    char tmp[20];
    int i = 0;
    while (v) { tmp[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
    /* Reverse */
    for (int a = 0, b = i - 1; a < b; a++, b--) {
        char t = tmp[a]; tmp[a] = tmp[b]; tmp[b] = t;
    }
    user_write(fd, tmp, (uint64_t)i);
}

__attribute__((noinline))
static void user_program_hello(void)
{
    /* "Hello from user space! My PID is: " — stack-allocated */
    char msg[] = { 'H','e','l','l','o',' ','f','r','o','m',' ',
                   'u','s','e','r',' ','s','p','a','c','e','!',' ',
                   'M','y',' ','P','I','D',' ','i','s',':',' ','\0' };
    char nl = '\n';

    /* Write greeting. */
    int len = 0;
    while (msg[len]) len++;
    user_write(1, msg, (uint64_t)len);

    /* Write PID. */
    int64_t pid = user_getpid();
    user_put_dec(1, (uint64_t)pid);
    user_write(1, &nl, 1);

    user_exit(0);
}

__attribute__((noinline))
static void user_program_counter(void)
{
    /* "User counter: " — stack-allocated */
    char prefix[] = { 'U','s','e','r',' ','c','o','u','n','t','e','r',':',' ','\0' };
    char nl = '\n';

    int plen = 0;
    while (prefix[plen]) plen++;

    for (uint64_t i = 0; i < 5; i++) {
        user_write(1, prefix, (uint64_t)plen);
        user_put_dec(1, i);
        user_write(1, &nl, 1);
        user_yield();
    }

    user_exit(0);
}

/* ── kernel_main2 — runs entirely on the kernel BSS stack ─────────────── */

__attribute__((noinline, noreturn))
static void kernel_main2(void)
{
    vmm_init();
    serial_puts("[OK] VMM\n");

    heap_init();
    serial_puts("[OK] Heap\n");

    /* ── ATA + GPT + FAT32 ───────────────────────────────────────────── */
    ata_init();

    uint32_t esp_lba = gpt_find_esp(ATA_DRIVE_MASTER);

    if (esp_lba != 0 && vfs_mount(ATA_DRIVE_MASTER, esp_lba) == 0) {
        serial_puts("[OK] FAT32 mounted on ESP\n");
        fb_puts("[OK] FAT32 mounted\n", COLOR_GREEN, COLOR_BLACK);

        /* List root directory. */
        vfs_dirent_t dir_ents[16];
        uint32_t dir_count = 0;
        if (vfs_list_dir("/", dir_ents, 16, &dir_count) == 0) {
            serial_puts("[DIR] /\n");
            for (uint32_t i = 0; i < dir_count; i++) {
                serial_puts("  ");
                serial_puts(dir_ents[i].is_dir ? "[DIR] " : "      ");
                serial_puts(dir_ents[i].name);
                serial_puts("\n");
            }
        }

        /* Print filesystem stats. */
        {
            fs_stat_t fst;
            if (fat32_get_stats(&fst) == 0) {
                serial_puts("[FAT32] Volume: total=");
                serial_put_dec(fst.total_bytes / 1024);
                serial_puts(" KB, used=");
                serial_put_dec(fst.used_bytes / 1024);
                serial_puts(" KB, free=");
                serial_put_dec(fst.free_bytes / 1024);
                serial_puts(" KB, cluster_size=");
                serial_put_dec(fst.cluster_size);
                serial_puts("\n[FAT32] Clusters: ");
                serial_put_dec(fst.total_clusters);
                serial_puts(" total, ");
                serial_put_dec(fst.used_clusters);
                serial_puts(" used, ");
                serial_put_dec(fst.free_clusters);
                serial_puts(" free\n");
            }
        }

        /* Read and print HELLO.TXT. */
        vfs_file_t file;
        if (vfs_open("/HELLO.TXT", &file) == 0) {
            char buf[128];
            uint32_t got = 0;
            if (vfs_read(&file, buf, sizeof(buf) - 1U, &got) == 0 && got > 0) {
                buf[got] = '\0';
                serial_puts("[FILE] HELLO.TXT:\n");
                serial_puts(buf);
                fb_puts(buf, COLOR_GREEN, COLOR_BLACK);
            }
            /* Test offset-aware reads and seeking. */
            vfs_seek(&file, 0, VFS_SEEK_SET);
            char tbuf[16];
            uint32_t tgot = 0;
            vfs_read(&file, tbuf, 5, &tgot);
            tbuf[tgot] = '\0';
            serial_puts("[VFS] Read 5 from offset 0: '");
            serial_puts(tbuf);
            serial_puts("'\n");

            vfs_read(&file, tbuf, 5, &tgot);
            tbuf[tgot] = '\0';
            serial_puts("[VFS] Read 5 from offset 5: '");
            serial_puts(tbuf);
            serial_puts("'\n");

            vfs_seek(&file, 0, VFS_SEEK_SET);
            vfs_read(&file, tbuf, 5, &tgot);
            tbuf[tgot] = '\0';
            serial_puts("[VFS] After seek(0): '");
            serial_puts(tbuf);
            serial_puts("'\n");

            vfs_close(&file);
        } else {
            serial_puts("[WARN] HELLO.TXT not found\n");
        }

        /* ── FAT32 write test ─────────────────────────────────────── */
        serial_puts("[TEST] FAT32 write: create WTEST.TXT...\n");
        if (vfs_create("/WTEST.TXT") == 0) {
            serial_puts("[TEST] Created WTEST.TXT\n");

            vfs_file_t wf;
            if (vfs_open("/WTEST.TXT", &wf) == 0) {
                const char *msg = "Hello from ASOS!\n";
                int mlen = 0;
                while (msg[mlen]) mlen++;
                uint32_t wr = vfs_write(&wf, msg, (uint32_t)mlen);
                serial_puts("[TEST] Wrote ");
                serial_put_dec(wr);
                serial_puts(" bytes\n");
                vfs_close(&wf);

                /* Read back and verify. */
                vfs_file_t rf;
                if (vfs_open("/WTEST.TXT", &rf) == 0) {
                    char rbuf[64];
                    uint32_t rgot = 0;
                    vfs_read(&rf, rbuf, sizeof(rbuf) - 1, &rgot);
                    rbuf[rgot] = '\0';
                    serial_puts("[TEST] Read back: '");
                    serial_puts(rbuf);
                    serial_puts("'\n");
                    vfs_close(&rf);
                }

                /* Delete and verify. */
                if (vfs_delete("/WTEST.TXT") == 0) {
                    serial_puts("[TEST] Deleted WTEST.TXT\n");
                    vfs_file_t df;
                    if (vfs_open("/WTEST.TXT", &df) != 0) {
                        serial_puts("[TEST] Verified: WTEST.TXT gone\n");
                    } else {
                        serial_puts("[TEST] ERROR: WTEST.TXT still exists!\n");
                        vfs_close(&df);
                    }
                } else {
                    serial_puts("[TEST] ERROR: delete failed\n");
                }
            } else {
                serial_puts("[TEST] ERROR: could not open WTEST.TXT\n");
            }
        } else {
            serial_puts("[TEST] ERROR: create failed\n");
        }
        /* ── FAT32 write B tests ─────────────────────────────────────── */
        serial_puts("[TEST] FAT32 write B tests...\n");

        /* Test 1: Create a directory */
        int r = vfs_mkdir("/TESTDIR");
        serial_puts("[TEST] mkdir /TESTDIR: ");
        serial_puts(r == 0 ? "OK\n" : "FAIL\n");

        /* Test 2: Verify directory appears in root listing */
        {
            vfs_dirent_t td_ents[32];
            uint32_t td_count = 0;
            vfs_list_dir("/", td_ents, 32, &td_count);
            for (uint32_t j = 0; j < td_count; j++) {
                if (td_ents[j].is_dir) {
                    serial_puts("[TEST] Found dir: ");
                    serial_puts(td_ents[j].name);
                    serial_puts("\n");
                }
            }
        }

        /* Test 3: Create a file inside the directory */
        r = vfs_create("/TESTDIR/INNER.TXT");
        serial_puts("[TEST] Create /TESTDIR/INNER.TXT: ");
        serial_puts(r == 0 ? "OK\n" : "FAIL\n");

        /* Test 4: Write to the inner file */
        {
            vfs_file_t inf;
            if (vfs_open("/TESTDIR/INNER.TXT", &inf) == 0) {
                const char *data = "File inside a subdirectory!";
                int dlen = 0;
                while (data[dlen]) dlen++;
                vfs_write(&inf, data, (uint32_t)dlen);
                vfs_close(&inf);
                serial_puts("[TEST] Wrote to inner file: OK\n");
            } else {
                serial_puts("[TEST] Could not open inner file\n");
            }
        }

        /* Test 5: Read it back */
        {
            vfs_file_t inf;
            if (vfs_open("/TESTDIR/INNER.TXT", &inf) == 0) {
                char rbuf[64];
                uint32_t rgot = 0;
                vfs_read(&inf, rbuf, 63, &rgot);
                rbuf[rgot] = '\0';
                serial_puts("[TEST] Read inner file: '");
                serial_puts(rbuf);
                serial_puts("'\n");
                vfs_close(&inf);
            }
        }

        /* Test 6: Copy a file */
        r = vfs_copy("/HELLO.TXT", "/HELLO2.TXT");
        serial_puts("[TEST] Copy HELLO.TXT -> HELLO2.TXT: ");
        serial_puts(r == 0 ? "OK\n" : "FAIL\n");

        /* Verify copy */
        {
            vfs_file_t cf;
            if (vfs_open("/HELLO2.TXT", &cf) == 0) {
                char rbuf[64];
                uint32_t rgot = 0;
                vfs_read(&cf, rbuf, 63, &rgot);
                rbuf[rgot] = '\0';
                serial_puts("[TEST] Copy contents: '");
                serial_puts(rbuf);
                serial_puts("'\n");
                vfs_close(&cf);
            }
        }

        /* Test 7: Rename */
        r = vfs_rename("/HELLO2.TXT", "/RENAMED.TXT");
        serial_puts("[TEST] Rename: ");
        serial_puts(r == 0 ? "OK\n" : "FAIL\n");

        /* Verify old name is gone */
        {
            vfs_file_t rf;
            serial_puts("[TEST] Old name gone: ");
            serial_puts(vfs_open("/HELLO2.TXT", &rf) != 0 ? "OK\n" : "FAIL\n");
        }

        /* Verify new name exists */
        {
            vfs_file_t rf;
            serial_puts("[TEST] New name exists: ");
            serial_puts(vfs_open("/RENAMED.TXT", &rf) == 0 ? "OK\n" : "FAIL\n");
        }

        /* Test 8: Move between directories */
        r = vfs_rename("/RENAMED.TXT", "/TESTDIR/MOVED.TXT");
        serial_puts("[TEST] Move to subdir: ");
        serial_puts(r == 0 ? "OK\n" : "FAIL\n");

        /* Cleanup */
        vfs_delete("/TESTDIR/INNER.TXT");
        vfs_delete("/TESTDIR/MOVED.TXT");
        /* TODO: vfs_rmdir for removing empty directories */

        serial_puts("[TEST] FAT32 write B tests complete.\n");

    } else {
        serial_puts("[WARN] Could not mount ESP\n");
        fb_puts("[WARN] Could not mount ESP\n", COLOR_YELLOW, COLOR_BLACK);
    }

    pic_init();
    pit_init();
    keyboard_init();
    mouse_init();

    /* Enable hardware interrupts — must come AFTER PIC init and all
     * handler registrations.  Before this point any IRQ would be
     * interpreted as a CPU exception. */
    __asm__ volatile ("sti" ::: "memory");
    serial_puts("[OK] Interrupts enabled.\n");

    /* ── Syscall mechanism ─────────────────────────────────────────── */
    syscall_init();
    serial_puts("[OK] Syscall\n");

    /* ── Ring-3 user processes ───────────────────────────────────────── */
    scheduler_init();

    /* Load shell from disk. */
    serial_puts("[INIT] Loading shell...\n");
    {
        vfs_file_t elf_file;
        if (vfs_open("/SHELL.ELF", &elf_file) == 0) {
            uint32_t fsz = vfs_size(&elf_file);
            void *elf_buf = kmalloc(fsz);
            uint32_t got = 0;
            if (vfs_read(&elf_file, elf_buf, fsz, &got) == 0 && got > 0) {
                serial_puts("[INIT] Read ");
                serial_put_dec(got);
                serial_puts(" bytes of SHELL.ELF\n");

                task_t *shell_task = task_create_from_elf("shell",
                                                          elf_buf, got);
                if (shell_task) {
                    shell_task->parent_pid = 0;
                    scheduler_add_task(shell_task);
                    serial_puts("[INIT] Shell started (pid=");
                    serial_put_dec(shell_task->id);
                    serial_puts(")\n");
                } else {
                    serial_puts("[INIT] FATAL: Failed to create shell process\n");
                }
            } else {
                serial_puts("[INIT] FATAL: Failed to read SHELL.ELF\n");
            }
            kfree(elf_buf);
            vfs_close(&elf_file);
        } else {
            serial_puts("[INIT] SHELL.ELF not found, falling back to HELLO.ELF\n");
            vfs_file_t hello_file;
            if (vfs_open("/HELLO.ELF", &hello_file) == 0) {
                uint32_t fsz = vfs_size(&hello_file);
                void *elf_buf = kmalloc(fsz);
                uint32_t got = 0;
                if (vfs_read(&hello_file, elf_buf, fsz, &got) == 0 && got > 0) {
                    task_t *t = task_create_from_elf("hello.elf", elf_buf, got);
                    if (t) scheduler_add_task(t);
                }
                kfree(elf_buf);
                vfs_close(&hello_file);
            } else {
                serial_puts("[INIT] No user programs found, running in-kernel\n");
                task_t *u1 = task_create_user("hello", user_program_hello, 4096);
                task_t *u2 = task_create_user("counter", user_program_counter, 4096);
                scheduler_add_task(u1);
                scheduler_add_task(u2);
            }
        }
    }

    serial_puts("[INIT] ASOS boot complete.\n\n");
    fb_puts(ASOS_VERSION " — booted.\n", COLOR_GREEN, COLOR_BLACK);

    /* ── Idle loop — the scheduler runs everything else ───────────── */
    for (;;) {
        /* Drain mouse events and print to serial for testing. */
        mouse_event_t mevt;
        while (mouse_read_event(&mevt)) {
            serial_puts("[MOUSE] dx=");
            if (mevt.dx < 0) { serial_putc('-'); serial_put_dec((uint64_t)(-(int64_t)mevt.dx)); }
            else              serial_put_dec((uint64_t)mevt.dx);
            serial_puts(" dy=");
            if (mevt.dy < 0) { serial_putc('-'); serial_put_dec((uint64_t)(-(int64_t)mevt.dy)); }
            else              serial_put_dec((uint64_t)mevt.dy);
            serial_puts(" btn=");
            serial_put_dec(mevt.buttons);
            serial_putc('\n');
        }
        __asm__ volatile ("hlt" ::: "memory");
    }
}

/* ── kernel_main — entry from bootloader, runs on UEFI stack ─────────── */

void kernel_main(BootInfo *info)
{
    clear_bss();

    serial_init();
    serial_puts(ASOS_VERSION "\n");

    fb_init(&info->framebuffer);
    fb_clear(COLOR_BLACK);
    fb_set_cursor(0, 0);

    gdt_init();
    serial_puts("[OK] GDT\n");

    idt_init();
    serial_puts("[OK] IDT\n");

    pmm_init(info);
    serial_puts("[OK] PMM\n");

    /*
     * Switch to the kernel BSS stack and call kernel_main2().
     *
     * This MUST be done via a function call — not by continuing in the
     * same function — because GCC with -O2 uses RSP-relative addressing
     * for locals (frame pointer omitted).  After the inline RSP switch
     * the compiler's RSP-relative offsets would land in BSS instead of
     * the actual stack frame, silently corrupting kernel globals.
     *
     * By calling a noinline function the compiler emits a fresh prologue
     * that builds a correct frame on the new stack.
     */
    __asm__ volatile ("mov %0, %%rsp"
        :: "r"((uint64_t)(uintptr_t)(g_kstack + KSTACK_SIZE)) : "memory");

    kernel_main2();
}
