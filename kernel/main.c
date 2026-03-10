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
#include "string.h"
#include "ata.h"
#include "gpt.h"
#include "vfs.h"
#include "scheduler.h"
#include "process.h"
#include "syscall.h"
#include "user_syscall.h"

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
            vfs_close(&file);
        } else {
            serial_puts("[WARN] HELLO.TXT not found\n");
        }
    } else {
        serial_puts("[WARN] Could not mount ESP\n");
        fb_puts("[WARN] Could not mount ESP\n", COLOR_YELLOW, COLOR_BLACK);
    }

    pic_init();
    pit_init();
    keyboard_init();

    /* Enable hardware interrupts — must come AFTER PIC init and all
     * handler registrations.  Before this point any IRQ would be
     * interpreted as a CPU exception. */
    __asm__ volatile ("sti" ::: "memory");
    serial_puts("[OK] Interrupts enabled.\n");

    /* ── Syscall mechanism ─────────────────────────────────────────── */
    syscall_init();
    serial_puts("[OK] Syscall\n");

    /* ── Ring-3 user process test ────────────────────────────────────── */
    scheduler_init();

    task_t *u1 = task_create_user("hello",   user_program_hello,   4096);
    task_t *u2 = task_create_user("counter", user_program_counter, 4096);
    scheduler_add_task(u1);
    scheduler_add_task(u2);

    serial_puts("[OK] Starting ring-3 syscall test: 2 user processes\n");
    fb_puts("[OK] Syscall test started\n", COLOR_GREEN, COLOR_BLACK);

    /* ── Banner ───────────────────────────────────────────────────────── */
    fb_puts(ASOS_VERSION " — Keyboard active. Type something:\n",
            COLOR_WHITE, COLOR_BLACK);
    serial_puts(ASOS_VERSION " — Keyboard active. Type something:\n");

    /* ── Main event loop ─────────────────────────────────────────────── */
    uint64_t last_uptime_s = 0;

    for (;;) {
        /* Sleep until the next interrupt (timer or keyboard). */
        __asm__ volatile ("hlt" ::: "memory");

        /* ── Uptime reporting (serial only, once per second) ── */
        uint64_t ticks = pit_get_ticks();
        uint64_t now_s = ticks / 1000;
        if (now_s != last_uptime_s) {
            last_uptime_s = now_s;
            serial_puts("[TIMER] Uptime: ");
            serial_put_dec(now_s);
            serial_puts("s\n");
        }

        /* ── Drain keyboard ring buffer ── */
        char c;
        while (keyboard_read_char(&c)) {
            /* Echo to serial. */
            if (c == '\b') {
                serial_putc('\b');
                serial_putc(' ');
                serial_putc('\b');
            } else {
                serial_putc(c);
            }

            /* Echo to framebuffer via cursor-based terminal. */
            char str[2] = { c, '\0' };
            fb_puts(str, COLOR_WHITE, COLOR_BLACK);
        }
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
