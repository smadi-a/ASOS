/*
 * kernel/syscall.c — Syscall MSR initialisation and handler dispatch.
 *
 * Sets up the syscall/sysret fast path using STAR, LSTAR, and FMASK MSRs.
 * Provides C handlers for SYS_READ, SYS_WRITE, SYS_EXIT, SYS_GETPID,
 * and SYS_YIELD.
 */

#include "syscall.h"
#include "serial.h"
#include "framebuffer.h"
#include "keyboard.h"
#include "scheduler.h"
#include "process.h"
#include "gdt.h"
#include "vmm.h"
#include "pmm.h"
#include "vfs.h"
#include "heap.h"
#include "elf.h"
#include <stdint.h>

/* ── MSR addresses ───────────────────────────────────────────────────────*/

#define MSR_EFER    0xC0000080
#define MSR_STAR    0xC0000081
#define MSR_LSTAR   0xC0000082
#define MSR_FMASK   0xC0000084

/* ── MSR helpers ─────────────────────────────────────────────────────────*/

static inline void wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile ("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/* ── Assembly entry point ────────────────────────────────────────────────*/

extern void syscall_entry(void);

/* ── Tiny serial printer ─────────────────────────────────────────────────*/

static void sc_put_dec(uint64_t v)
{
    if (v == 0) { serial_putc('0'); return; }
    char tmp[20];
    int i = 0;
    while (v) { tmp[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (i--) serial_putc(tmp[i]);
}

static void sc_put_hex(uint64_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    serial_puts("0x");
    for (int i = 60; i >= 0; i -= 4)
        serial_putc(hex[(v >> i) & 0xF]);
}

/* ── syscall_init ────────────────────────────────────────────────────────*/

void syscall_init(void)
{
    /* 1. Enable the SCE (Syscall Enable) bit in EFER. */
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= 1;   /* bit 0 = SCE */
    wrmsr(MSR_EFER, efer);

    /* 2. STAR: segment selectors for syscall/sysret.
     *
     *   Bits 47:32 = kernel CS (syscall loads CS from here, SS = CS+8)
     *                kernel CS = 0x08, kernel SS = 0x10.  Correct.
     *
     *   Bits 63:48 = sysret base (sysret loads SS = base+8, CS = base+16)
     *                We need user SS = 0x18 (user data), user CS = 0x20 (user code).
     *                So base = 0x20 - 16 = 0x10.  Check: 0x10+8=0x18, 0x10+16=0x20.
     *                sysret sets RPL to 3 automatically → 0x1B and 0x23.  Correct.
     */
    uint64_t star = ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32);
    wrmsr(MSR_STAR, star);

    /* 3. LSTAR: kernel entry point for 64-bit syscall. */
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);

    /* 4. FMASK: clear IF and DF on syscall entry. */
    wrmsr(MSR_FMASK, 0x600);   /* bit 9 = IF, bit 10 = DF */

    serial_puts("[SYSCALL] Initialized, entry at ");
    sc_put_hex((uint64_t)(uintptr_t)syscall_entry);
    serial_puts("\n");
}

/* ── Syscall handlers ────────────────────────────────────────────────────*/

#define USER_ADDR_LIMIT  0x0000800000000000ULL

static int64_t sys_write(uint64_t fd, uint64_t buf_addr, uint64_t count)
{
    if (fd != 1) return -1;
    if (count == 0) return 0;
    if (count > 4096) count = 4096;

    /* Validate user-space buffer address. */
    if (buf_addr >= USER_ADDR_LIMIT) return -1;
    if (buf_addr + count > USER_ADDR_LIMIT) return -1;

    /*
     * Copy user data to a kernel-stack buffer while CR3 still points
     * to the user's page tables, then switch to kernel page tables
     * to access the framebuffer (identity-mapped at physical 0x80000000).
     * Serial port I/O doesn't need page tables, so it works in either.
     */
    const char *ubuf = (const char *)(uintptr_t)buf_addr;
    task_t *cur = scheduler_get_current();

    /* Process in chunks to avoid a large stack buffer. */
    uint64_t written = 0;
    while (written < count) {
        char kbuf[256];
        uint64_t chunk = count - written;
        if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);

        /* Copy from user space while user page tables are active. */
        for (uint64_t i = 0; i < chunk; i++)
            kbuf[i] = ubuf[written + i];

        /* Write to serial (works with any CR3). */
        for (uint64_t i = 0; i < chunk; i++)
            serial_putc(kbuf[i]);

        /* Switch to kernel CR3 to access the framebuffer. */
        vmm_switch_address_space(vmm_get_kernel_pml4());

        for (uint64_t i = 0; i < chunk; i++) {
            char str[2] = { kbuf[i], '\0' };
            fb_puts(str, COLOR_WHITE, COLOR_BLACK);
        }

        /* Switch back to user CR3. */
        vmm_switch_address_space(cur->pml4_phys);

        written += chunk;
    }

    return (int64_t)count;
}

static int64_t sys_read(uint64_t fd, uint64_t buf_addr, uint64_t count)
{
    if (fd != 0) return -1;
    if (count == 0) return 0;
    if (buf_addr >= USER_ADDR_LIMIT) return -1;
    if (buf_addr + count > USER_ADDR_LIMIT) return -1;

    char *buf = (char *)(uintptr_t)buf_addr;

    /* Block until at least one character is available. */
    uint64_t bytes_read = 0;
    while (bytes_read == 0) {
        char c;
        if (keyboard_read_char(&c)) {
            buf[bytes_read++] = c;
        } else {
            __asm__ volatile ("sti" ::: "memory");
            scheduler_yield();
            __asm__ volatile ("cli" ::: "memory");
        }
    }

    /* Read any additional available characters (non-blocking). */
    while (bytes_read < count) {
        char c;
        if (!keyboard_read_char(&c)) break;
        buf[bytes_read++] = c;
    }

    return (int64_t)bytes_read;
}

static void sys_exit(int status)
{
    task_t *cur = scheduler_get_current();
    cur->exit_status = status;

    serial_puts("[SYSCALL] Task ");
    sc_put_dec(cur->id);
    serial_puts(" (");
    serial_puts(cur->name);
    serial_puts(") exited with status ");
    sc_put_dec((uint64_t)(unsigned)status);
    serial_puts("\n");

    cur->state = TASK_DEAD;
    __asm__ volatile ("sti" ::: "memory");
    scheduler_yield();

    for (;;) __asm__ volatile ("hlt");
}

static int64_t sys_getpid(void)
{
    return (int64_t)scheduler_get_current()->id;
}

static void sys_yield(void)
{
    __asm__ volatile ("sti" ::: "memory");
    scheduler_yield();
    __asm__ volatile ("cli" ::: "memory");
}

static int64_t sys_spawn(uint64_t path_addr, uint64_t argv_addr)
{
    (void)argv_addr;   /* TODO: pass argv to child */

    if (path_addr >= USER_ADDR_LIMIT) return -1;

    /* Copy path from user space to a kernel buffer.
     * We must do this while still in the user's address space. */
    char path[256];
    const char *user_path = (const char *)(uintptr_t)path_addr;
    int i;
    for (i = 0; i < 255; i++) {
        path[i] = user_path[i];
        if (path[i] == '\0') break;
    }
    path[255] = '\0';

    /* Switch to kernel address space for VFS/PMM/VMM operations. */
    task_t *cur = scheduler_get_current();
    uint64_t kernel_pml4 = vmm_get_kernel_pml4();
    vmm_switch_address_space(kernel_pml4);

    /* Open the file. */
    vfs_file_t file;
    if (vfs_open(path, &file) != 0) {
        serial_puts("[SPAWN] File not found: ");
        serial_puts(path);
        serial_puts("\n");
        vmm_switch_address_space(cur->pml4_phys);
        return -1;
    }

    /* Read the file into a kernel buffer. */
    uint32_t fsz = vfs_size(&file);
    void *elf_buf = kmalloc(fsz);
    uint32_t got = 0;
    if (vfs_read(&file, elf_buf, fsz, &got) != 0 || got == 0) {
        serial_puts("[SPAWN] Failed to read: ");
        serial_puts(path);
        serial_puts("\n");
        kfree(elf_buf);
        vfs_close(&file);
        vmm_switch_address_space(cur->pml4_phys);
        return -1;
    }
    vfs_close(&file);

    /* Extract filename for process name. */
    const char *name = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') name = p + 1;
    }

    /* Create process from ELF. */
    task_t *child = task_create_from_elf(name, elf_buf, got);
    kfree(elf_buf);

    if (!child) {
        serial_puts("[SPAWN] Failed to create process from ");
        serial_puts(path);
        serial_puts("\n");
        vmm_switch_address_space(cur->pml4_phys);
        return -1;
    }

    child->parent_pid = cur->id;
    scheduler_add_task(child);

    serial_puts("[SPAWN] Spawned ");
    serial_puts(name);
    serial_puts(" (pid=");
    sc_put_dec(child->id);
    serial_puts(", parent=");
    sc_put_dec(cur->id);
    serial_puts(")\n");

    /* Switch back to user address space. */
    vmm_switch_address_space(cur->pml4_phys);

    return (int64_t)child->id;
}

static int64_t sys_waitpid(uint64_t pid, uint64_t status_addr)
{
    if (status_addr != 0 && status_addr >= USER_ADDR_LIMIT)
        return -1;

    task_t *cur = scheduler_get_current();

    for (;;) {
        interrupts_disable();
        task_t *child = scheduler_find_dead_child(cur->id, (int64_t)pid);

        if (child) {
            int64_t child_pid = (int64_t)child->id;
            int child_status = child->exit_status;
            scheduler_cleanup_task(child);
            interrupts_enable();

            /* Store exit status in user space. */
            if (status_addr != 0) {
                int *user_status = (int *)(uintptr_t)status_addr;
                *user_status = child_status;
            }

            return child_pid;
        }

        /* Check if the requested child exists at all. */
        if ((int64_t)pid != -1) {
            task_t *target = scheduler_find_task_by_pid(pid);
            if (!target || target->parent_pid != cur->id) {
                interrupts_enable();
                return -1;  /* Not our child or doesn't exist */
            }
        }

        interrupts_enable();

        /* No dead child yet — yield and try again. */
        __asm__ volatile ("sti" ::: "memory");
        scheduler_yield();
        __asm__ volatile ("cli" ::: "memory");
    }
}

static int64_t sys_sbrk(int64_t increment)
{
    task_t *cur = scheduler_get_current();

    /* sbrk(0) returns the current break. */
    if (increment == 0)
        return (int64_t)cur->heap_break;

    uint64_t old_break = cur->heap_break;
    uint64_t new_break = old_break + (uint64_t)increment;

    /* Bounds check. */
    if (new_break < cur->heap_start || new_break > cur->heap_max)
        return -1;

    /* Map any new pages needed between old and new break.
     * pmm_alloc_frame and vmm_map_user_page use the identity map,
     * which is only present in the kernel page tables.  Switch CR3
     * temporarily. */
    uint64_t old_page = (old_break + 0xFFF) & ~0xFFFULL;
    uint64_t new_page = (new_break + 0xFFF) & ~0xFFFULL;

    if (old_page < new_page) {
        uint64_t kernel_pml4 = vmm_get_kernel_pml4();
        vmm_switch_address_space(kernel_pml4);

        for (uint64_t page = old_page; page < new_page; page += 4096) {
            uint64_t frame = pmm_alloc_frame();
            vmm_map_user_page(cur->pml4_phys, page, frame,
                              PTE_PRESENT | PTE_USER | PTE_WRITABLE | PTE_NO_EXEC);
        }

        vmm_switch_address_space(cur->pml4_phys);
    }

    cur->heap_break = new_break;
    return (int64_t)old_break;
}

/* User-space directory entry — must match user/libc/include/unistd.h. */
typedef struct {
    char     name[12];
    uint32_t size;
    uint8_t  is_directory;
    uint8_t  padding[3];
} sys_dirent_t;

static int64_t sys_readdir(uint64_t path_addr, uint64_t buf_addr,
                           uint64_t max_entries)
{
    if (path_addr >= USER_ADDR_LIMIT) return -1;
    if (buf_addr  >= USER_ADDR_LIMIT) return -1;
    if (max_entries == 0 || max_entries > 256) return -1;

    /* Copy path from user space. */
    const char *user_path = (const char *)(uintptr_t)path_addr;
    char path[256];
    int i;
    for (i = 0; i < 255 && user_path[i]; i++)
        path[i] = user_path[i];
    path[i] = '\0';

    /* Switch to kernel CR3 for VFS access. */
    task_t *cur = scheduler_get_current();
    vmm_switch_address_space(vmm_get_kernel_pml4());

    /* List directory via VFS. */
    uint32_t cap = (max_entries < 64) ? (uint32_t)max_entries : 64;
    vfs_dirent_t vfs_ents[64];
    uint32_t count = 0;
    int rc = vfs_list_dir(path, vfs_ents, cap, &count);

    vmm_switch_address_space(cur->pml4_phys);

    if (rc != 0) return -1;

    /* Copy results to user buffer. */
    sys_dirent_t *ubuf = (sys_dirent_t *)(uintptr_t)buf_addr;
    for (uint32_t j = 0; j < count; j++) {
        /* vfs_dirent_t.name is already "HELLO.ELF" display format. */
        int k;
        for (k = 0; k < 11 && vfs_ents[j].name[k]; k++)
            ubuf[j].name[k] = vfs_ents[j].name[k];
        ubuf[j].name[k] = '\0';
        ubuf[j].size = vfs_ents[j].size;
        ubuf[j].is_directory = vfs_ents[j].is_dir ? 1 : 0;
        ubuf[j].padding[0] = 0;
        ubuf[j].padding[1] = 0;
        ubuf[j].padding[2] = 0;
    }

    serial_puts("[READDIR] Registered SYS_READDIR (syscall ");
    sc_put_dec(SYS_READDIR);
    serial_puts("), returned ");
    sc_put_dec(count);
    serial_puts(" entries\n");

    return (int64_t)count;
}

static int64_t sys_pidof(uint64_t name_addr)
{
    if (name_addr >= USER_ADDR_LIMIT) return -1;

    const char *user_name = (const char *)(uintptr_t)name_addr;
    char name[32];
    int i;
    for (i = 0; i < 31 && user_name[i]; i++)
        name[i] = user_name[i];
    name[i] = '\0';

    task_t *t = scheduler_find_task_by_name(name);
    if (t) return (int64_t)t->id;
    return -1;
}

static int64_t sys_kill(uint64_t target_pid)
{
    /* Cannot kill idle (PID 0). */
    if (target_pid == 0) return -1;

    task_t *cur = scheduler_get_current();

    /* Cannot kill yourself — use sys_exit. */
    if (target_pid == cur->id) return -1;

    task_t *target = scheduler_find_task_by_pid(target_pid);
    if (!target) return -1;
    if (target->state == TASK_DEAD) return -1;

    /* TODO: proper cleanup — close open fds, free user pages/PTs. */
    target->exit_status = -1;
    target->state = TASK_DEAD;

    interrupts_disable();
    scheduler_remove_from_ready_queue(target);
    interrupts_enable();

    serial_puts("[KILL] Task ");
    sc_put_dec(target->id);
    serial_puts(" (");
    serial_puts(target->name);
    serial_puts(") killed by task ");
    sc_put_dec(cur->id);
    serial_puts("\n");

    return 0;
}

/*
 * proc_info_t — must match user/libc/include/unistd.h layout.
 * state values: 0=CREATED, 1=RUNNING, 2=READY, 3=BLOCKED, 4=DEAD
 */
typedef struct {
    uint64_t pid;
    uint64_t parent_pid;
    char     name[32];
    uint8_t  state;
    uint8_t  padding[3];
    uint32_t reserved;
} sys_proc_info_t;

static int64_t sys_proclist(uint64_t buf_addr, uint64_t max_entries)
{
    if (buf_addr >= USER_ADDR_LIMIT) return -1;
    if (max_entries == 0 || max_entries > 256) return -1;

    sys_proc_info_t *ubuf = (sys_proc_info_t *)(uintptr_t)buf_addr;
    uint64_t count = 0;

    /* Snapshot the task list with interrupts disabled. */
    interrupts_disable();

    /* Walk the global task list via scheduler internal iteration.
     * We access it through the find functions, but for a full walk
     * we need direct access.  Use scheduler_find_task_by_pid with
     * incrementing PIDs?  No — just iterate from the scheduler. */

    /* We need access to the global list head.  Rather than expose it,
     * iterate by PID starting from 0.  This is O(n^2) but n is small. */

    /* Actually, let's just iterate sensibly.  We'll call a helper. */
    interrupts_enable();

    /* Walk PIDs 0..max reasonable. The task list has monotonic IDs. */
    for (uint64_t pid = 0; count < max_entries; pid++) {
        task_t *t = scheduler_find_task_by_pid(pid);
        if (!t) {
            /* No task with this PID.  If we've gone past a reasonable
             * gap, stop scanning. */
            if (pid > 1000) break;
            continue;
        }
        if (t->state == TASK_DEAD) continue;

        ubuf[count].pid = t->id;
        ubuf[count].parent_pid = t->parent_pid;
        int i;
        for (i = 0; i < 31 && t->name[i]; i++)
            ubuf[count].name[i] = t->name[i];
        ubuf[count].name[i] = '\0';
        ubuf[count].state = (uint8_t)t->state;
        ubuf[count].padding[0] = 0;
        ubuf[count].padding[1] = 0;
        ubuf[count].padding[2] = 0;
        ubuf[count].reserved = 0;
        count++;
    }

    return (int64_t)count;
}

/* ── Dispatch ────────────────────────────────────────────────────────────*/

int64_t syscall_dispatch(uint64_t num, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;

    switch (num) {
    case SYS_READ:    return sys_read(arg1, arg2, arg3);
    case SYS_WRITE:   return sys_write(arg1, arg2, arg3);
    case SYS_EXIT:    sys_exit((int)arg1); return 0;
    case SYS_GETPID:  return sys_getpid();
    case SYS_YIELD:   sys_yield(); return 0;
    case SYS_SBRK:    return sys_sbrk((int64_t)arg1);
    case SYS_WAITPID: return sys_waitpid(arg1, arg2);
    case SYS_SPAWN:   return sys_spawn(arg1, arg2);
    case SYS_READDIR: return sys_readdir(arg1, arg2, arg3);
    case SYS_PIDOF:   return sys_pidof(arg1);
    case SYS_KILL:    return sys_kill(arg1);
    case SYS_PROCLIST:return sys_proclist(arg1, arg2);
    default:
        serial_puts("[SYSCALL] Unknown syscall ");
        sc_put_dec(num);
        serial_puts("\n");
        return -1;
    }
}
