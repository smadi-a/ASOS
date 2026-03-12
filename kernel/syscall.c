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
#include "gfx.h"
#include "wm.h"
#include "string.h"
#include "power.h"
#include "pit.h"
#include "../shared/gfx.h"
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

/*
 * wm_kbd_allowed — Check if the calling process is allowed to read the
 * keyboard.  When the WM has user windows (g_window_count > 1), only the
 * process that owns the focused window (or a direct child of that owner)
 * may consume key events.  When no user windows exist, any process may
 * read the keyboard (backward compat with the text-only shell).
 */
static bool wm_kbd_allowed(task_t *cur)
{
    if (g_window_count <= 1)
        return true;   /* No user windows — text-mode, allow all */

    uint32_t owner = wm_get_focused_owner();
    if (owner == 0)
        return false;  /* No focused window — nobody gets keys */

    if ((uint32_t)cur->id == owner)
        return true;
    if ((uint32_t)cur->parent_pid == owner)
        return true;

    return false;
}

static int64_t sys_read(uint64_t fd, uint64_t buf_addr, uint64_t count)
{
    if (fd != 0) return -1;
    if (count == 0) return 0;
    if (buf_addr >= USER_ADDR_LIMIT) return -1;
    if (buf_addr + count > USER_ADDR_LIMIT) return -1;

    char *buf = (char *)(uintptr_t)buf_addr;

    task_t *cur = scheduler_get_current();

    /* Block until at least one character is available. */
    uint64_t bytes_read = 0;
    while (bytes_read == 0) {
        /* Check for pending signal (Ctrl+C). */
        if (cur->pending_signal) {
            uint8_t sig = cur->pending_signal;
            cur->pending_signal = 0;
            if (sig == 0x03) {
                /* Deliver ETX (0x03) to the user buffer so gets_s can detect it. */
                buf[0] = 0x03;
                return 1;
            }
        }

        /* Only consume keyboard input if this process is associated
         * with the currently focused WM window. */
        if (wm_kbd_allowed(cur)) {
            char c;
            if (keyboard_read_char(&c)) {
                buf[bytes_read++] = c;
                continue;
            }
        }

        __asm__ volatile ("sti" ::: "memory");
        scheduler_yield();
        __asm__ volatile ("cli" ::: "memory");
    }

    /* Read any additional available characters (non-blocking). */
    while (bytes_read < count) {
        if (!wm_kbd_allowed(cur)) break;
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
    cur->is_foreground = 0;

    /* Destroy any WM windows owned by this process. */
    wm_destroy_by_owner((uint32_t)cur->id);

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

    /* Resolve path against cwd. */
    task_t *cur = scheduler_get_current();
    char resolved[256];
    if (vfs_resolve_path(path, cur->cwd, resolved, sizeof(resolved)) != 0)
        return -1;

    /* Switch to kernel address space for VFS/PMM/VMM operations. */
    uint64_t kernel_pml4 = vmm_get_kernel_pml4();
    vmm_switch_address_space(kernel_pml4);

    /* Open the file. */
    vfs_file_t file;
    if (vfs_open(resolved, &file) != 0) {
        serial_puts("[SPAWN] File not found: ");
        serial_puts(resolved);
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
        serial_puts(resolved);
        serial_puts("\n");
        kfree(elf_buf);
        vfs_close(&file);
        vmm_switch_address_space(cur->pml4_phys);
        return -1;
    }
    vfs_close(&file);

    /* Extract filename for process name. */
    const char *name = resolved;
    for (const char *p = resolved; *p; p++) {
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
    child->is_foreground = 1;
    cur->is_foreground = 0;
    strncpy(child->cwd, cur->cwd, sizeof(child->cwd) - 1);
    child->cwd[sizeof(child->cwd) - 1] = '\0';
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
            cur->is_foreground = 1;
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

    /* Resolve path against cwd. */
    task_t *cur = scheduler_get_current();
    char resolved[256];
    if (vfs_resolve_path(path, cur->cwd, resolved, sizeof(resolved)) != 0)
        return -1;

    /* Switch to kernel CR3 for VFS access. */
    vmm_switch_address_space(vmm_get_kernel_pml4());

    /* List directory via VFS. */
    uint32_t cap = (max_entries < 64) ? (uint32_t)max_entries : 64;
    vfs_dirent_t vfs_ents[64];
    uint32_t count = 0;
    int rc = vfs_list_dir(resolved, vfs_ents, cap, &count);

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

    /* Destroy any WM windows owned by the killed process. */
    wm_destroy_by_owner((uint32_t)target->id);

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

static int64_t sys_getcwd(uint64_t buf_addr, uint64_t size)
{
    if (buf_addr >= USER_ADDR_LIMIT) return -1;
    if (size == 0) return -1;

    task_t *cur = scheduler_get_current();
    size_t cwd_len = strlen(cur->cwd);

    if (cwd_len + 1 > size) return -1;  /* Buffer too small */

    char *user_buf = (char *)(uintptr_t)buf_addr;
    memcpy(user_buf, cur->cwd, cwd_len + 1);
    return 0;
}

static int64_t sys_chdir(uint64_t path_addr)
{
    if (path_addr >= USER_ADDR_LIMIT) return -1;

    /* Copy path from user space. */
    const char *user_path = (const char *)(uintptr_t)path_addr;
    char input[256];
    int i;
    for (i = 0; i < 255 && user_path[i]; i++)
        input[i] = user_path[i];
    input[i] = '\0';

    task_t *cur = scheduler_get_current();

    /* Resolve against current working directory. */
    char resolved[256];
    if (vfs_resolve_path(input, cur->cwd, resolved, sizeof(resolved)) != 0)
        return -1;

    /* Verify the directory exists (only root "/" is fully supported by VFS).
     * For root, we know it exists. For subdirectories, try listing.
     * If VFS doesn't support subdirectories, skip validation — just
     * store the path and let subsequent file ops fail naturally. */
    if (strcmp(resolved, "/") != 0) {
        /* TODO: validate subdirectory exists when VFS supports it. */
    }

    /* Store resolved path, ensuring it ends with '/'. */
    strncpy(cur->cwd, resolved, sizeof(cur->cwd) - 2);
    cur->cwd[sizeof(cur->cwd) - 1] = '\0';
    size_t len = strlen(cur->cwd);
    if (len > 0 && cur->cwd[len - 1] != '/') {
        cur->cwd[len] = '/';
        cur->cwd[len + 1] = '\0';
    }

    serial_puts("[VFS] Task ");
    sc_put_dec(cur->id);
    serial_puts(" cwd changed to: ");
    serial_puts(cur->cwd);
    serial_puts("\n");

    return 0;
}

static int64_t sys_fsstat(uint64_t buf_addr)
{
    if (buf_addr >= USER_ADDR_LIMIT) return -1;

    task_t *cur = scheduler_get_current();
    fs_stat_t kstat;

    /* Switch to kernel CR3 for FAT sector reads. */
    vmm_switch_address_space(vmm_get_kernel_pml4());
    int rc = vfs_get_stats(&kstat);
    vmm_switch_address_space(cur->pml4_phys);

    if (rc != 0) return -1;

    fs_stat_t *ubuf = (fs_stat_t *)(uintptr_t)buf_addr;
    memcpy(ubuf, &kstat, sizeof(fs_stat_t));
    return 0;
}

/* ── File I/O syscalls ───────────────────────────────────────────────────*/

#define FD_MIN 3
#define FD_MAX (FD_MIN + MAX_OPEN_FILES - 1)

static int64_t sys_fopen(uint64_t path_addr)
{
    if (path_addr >= USER_ADDR_LIMIT) return -1;

    /* Copy path from user space. */
    const char *user_path = (const char *)(uintptr_t)path_addr;
    char path[256];
    int i;
    for (i = 0; i < 255 && user_path[i]; i++)
        path[i] = user_path[i];
    path[i] = '\0';

    task_t *cur = scheduler_get_current();

    /* Resolve against cwd. */
    char resolved[256];
    if (vfs_resolve_path(path, cur->cwd, resolved, sizeof(resolved)) != 0)
        return -1;

    /* Switch to kernel CR3 for VFS access. */
    vmm_switch_address_space(vmm_get_kernel_pml4());

    vfs_file_t file;
    int rc = vfs_open(resolved, &file);

    vmm_switch_address_space(cur->pml4_phys);

    if (rc != 0) return -1;

    /* Find a free fd slot. */
    for (int fd = 0; fd < MAX_OPEN_FILES; fd++) {
        if (!cur->fd_table[fd].in_use) {
            cur->fd_table[fd].first_cluster = file._fat.first_cluster;
            cur->fd_table[fd].file_size     = file._fat.size;
            cur->fd_table[fd].offset        = 0;
            cur->fd_table[fd].dir_cluster   = file.dir_cluster;
            cur->fd_table[fd].in_use        = 1;
            memcpy(cur->fd_table[fd].name_83, file.name_83, 11);

            serial_puts("[FOPEN] fd=");
            sc_put_dec((uint64_t)(fd + FD_MIN));
            serial_puts(" file=");
            serial_puts(resolved);
            serial_puts(" size=");
            sc_put_dec(file._fat.size);
            serial_puts("\n");

            return (int64_t)(fd + FD_MIN);
        }
    }

    return -1;  /* No free slots */
}

static int64_t sys_fread(uint64_t fd, uint64_t buf_addr, uint64_t count)
{
    if (buf_addr >= USER_ADDR_LIMIT) return -1;
    if (fd < FD_MIN || fd > FD_MAX) return -1;

    task_t *cur = scheduler_get_current();
    int idx = (int)(fd - FD_MIN);
    if (!cur->fd_table[idx].in_use) return -1;

    if (count == 0) return 0;
    if (count > 65536) count = 65536;

    /* Reconstruct a vfs_file_t from the fd entry. */
    vfs_file_t vf;
    vf._fat.first_cluster = cur->fd_table[idx].first_cluster;
    vf._fat.size          = cur->fd_table[idx].file_size;
    vf.offset             = cur->fd_table[idx].offset;

    /* We need kernel CR3 for FAT32 reads. But we also need to
     * write to the user buffer. Read into a kernel stack buffer
     * in chunks, then copy to user space. */
    uint8_t *ubuf = (uint8_t *)(uintptr_t)buf_addr;
    uint64_t total_read = 0;

    while (total_read < count) {
        uint8_t kbuf[512];
        uint64_t chunk = count - total_read;
        if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);

        vmm_switch_address_space(vmm_get_kernel_pml4());

        uint32_t got = 0;
        int rc = vfs_read(&vf, kbuf, (uint32_t)chunk, &got);

        vmm_switch_address_space(cur->pml4_phys);

        if (rc != 0) break;
        if (got == 0) break;

        /* Copy to user buffer. */
        for (uint32_t j = 0; j < got; j++)
            ubuf[total_read + j] = kbuf[j];

        total_read += got;
    }

    /* Update fd offset. */
    cur->fd_table[idx].offset = vf.offset;

    return (int64_t)total_read;
}

static int64_t sys_fclose(uint64_t fd)
{
    if (fd < FD_MIN || fd > FD_MAX) return -1;

    task_t *cur = scheduler_get_current();
    int idx = (int)(fd - FD_MIN);
    if (!cur->fd_table[idx].in_use) return -1;

    cur->fd_table[idx].in_use = 0;
    return 0;
}

static int64_t sys_fsize(uint64_t fd)
{
    if (fd < FD_MIN || fd > FD_MAX) return -1;

    task_t *cur = scheduler_get_current();
    int idx = (int)(fd - FD_MIN);
    if (!cur->fd_table[idx].in_use) return -1;

    return (int64_t)cur->fd_table[idx].file_size;
}

static int64_t sys_fseek(uint64_t fd, uint64_t offset_raw, uint64_t whence)
{
    if (fd < FD_MIN || fd > FD_MAX) return -1;

    task_t *cur = scheduler_get_current();
    int idx = (int)(fd - FD_MIN);
    if (!cur->fd_table[idx].in_use) return -1;

    /* Build a temporary vfs_file_t to use vfs_seek. */
    vfs_file_t vf;
    vf._fat.first_cluster = cur->fd_table[idx].first_cluster;
    vf._fat.size          = cur->fd_table[idx].file_size;
    vf.offset             = cur->fd_table[idx].offset;

    int rc = vfs_seek(&vf, (int64_t)offset_raw, (int)whence);
    if (rc != 0) return -1;

    cur->fd_table[idx].offset = vf.offset;
    return 0;
}

static int64_t sys_ftell(uint64_t fd)
{
    if (fd < FD_MIN || fd > FD_MAX) return -1;

    task_t *cur = scheduler_get_current();
    int idx = (int)(fd - FD_MIN);
    if (!cur->fd_table[idx].in_use) return -1;

    return (int64_t)cur->fd_table[idx].offset;
}

static int64_t sys_fwrite(uint64_t fd, uint64_t buf_addr, uint64_t count)
{
    if (buf_addr >= USER_ADDR_LIMIT) return -1;
    if (fd < FD_MIN || fd > FD_MAX) return -1;

    task_t *cur = scheduler_get_current();
    int idx = (int)(fd - FD_MIN);
    if (!cur->fd_table[idx].in_use) return -1;

    if (count == 0) return 0;
    if (count > 65536) count = 65536;

    /* Copy user data to kernel buffer, then write under kernel CR3. */
    const uint8_t *ubuf = (const uint8_t *)(uintptr_t)buf_addr;
    uint64_t total_written = 0;

    /* Reconstruct vfs_file_t. */
    vfs_file_t vf;
    vf._fat.first_cluster = cur->fd_table[idx].first_cluster;
    vf._fat.size          = cur->fd_table[idx].file_size;
    vf.offset             = cur->fd_table[idx].offset;
    vf.dir_cluster        = cur->fd_table[idx].dir_cluster;
    memcpy(vf.name_83, cur->fd_table[idx].name_83, 11);

    while (total_written < count) {
        uint8_t kbuf[512];
        uint64_t chunk = count - total_written;
        if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);

        /* Copy from user space. */
        for (uint64_t j = 0; j < chunk; j++)
            kbuf[j] = ubuf[total_written + j];

        vmm_switch_address_space(vmm_get_kernel_pml4());
        uint32_t got = vfs_write(&vf, kbuf, (uint32_t)chunk);
        vmm_switch_address_space(cur->pml4_phys);

        if (got == 0) break;
        total_written += got;
    }

    /* Update fd entry from vfs_file_t. */
    cur->fd_table[idx].first_cluster = vf._fat.first_cluster;
    cur->fd_table[idx].file_size     = vf._fat.size;
    cur->fd_table[idx].offset        = vf.offset;

    return (int64_t)total_written;
}

static int64_t sys_fcreate(uint64_t path_addr)
{
    if (path_addr >= USER_ADDR_LIMIT) return -1;

    const char *user_path = (const char *)(uintptr_t)path_addr;
    char path[256];
    int i;
    for (i = 0; i < 255 && user_path[i]; i++)
        path[i] = user_path[i];
    path[i] = '\0';

    task_t *cur = scheduler_get_current();
    char resolved[256];
    if (vfs_resolve_path(path, cur->cwd, resolved, sizeof(resolved)) != 0)
        return -1;

    vmm_switch_address_space(vmm_get_kernel_pml4());
    int rc = vfs_create(resolved);
    vmm_switch_address_space(cur->pml4_phys);

    return (int64_t)rc;
}

static int64_t sys_fdelete(uint64_t path_addr)
{
    if (path_addr >= USER_ADDR_LIMIT) return -1;

    const char *user_path = (const char *)(uintptr_t)path_addr;
    char path[256];
    int i;
    for (i = 0; i < 255 && user_path[i]; i++)
        path[i] = user_path[i];
    path[i] = '\0';

    task_t *cur = scheduler_get_current();
    char resolved[256];
    if (vfs_resolve_path(path, cur->cwd, resolved, sizeof(resolved)) != 0)
        return -1;

    vmm_switch_address_space(vmm_get_kernel_pml4());
    int rc = vfs_delete(resolved);
    vmm_switch_address_space(cur->pml4_phys);

    return (int64_t)rc;
}

/* ── Directory/file management syscalls ───────────────────────────────────*/

static int64_t sys_mkdir(uint64_t path_addr)
{
    if (path_addr >= USER_ADDR_LIMIT) return -1;

    char path[256];
    const char *user_path = (const char *)(uintptr_t)path_addr;
    int i;
    for (i = 0; i < 255 && user_path[i]; i++) path[i] = user_path[i];
    path[i] = '\0';

    task_t *cur = scheduler_get_current();
    char resolved[256];
    if (vfs_resolve_path(path, cur->cwd, resolved, sizeof(resolved)) != 0)
        return -1;

    vmm_switch_address_space(vmm_get_kernel_pml4());
    int rc = vfs_mkdir(resolved);
    vmm_switch_address_space(cur->pml4_phys);

    return (int64_t)rc;
}

static int64_t sys_rename(uint64_t old_path_addr, uint64_t new_path_addr)
{
    if (old_path_addr >= USER_ADDR_LIMIT) return -1;
    if (new_path_addr >= USER_ADDR_LIMIT) return -1;

    char old_path[256], new_path[256];

    const char *u_old = (const char *)(uintptr_t)old_path_addr;
    int i;
    for (i = 0; i < 255 && u_old[i]; i++) old_path[i] = u_old[i];
    old_path[i] = '\0';

    const char *u_new = (const char *)(uintptr_t)new_path_addr;
    for (i = 0; i < 255 && u_new[i]; i++) new_path[i] = u_new[i];
    new_path[i] = '\0';

    task_t *cur = scheduler_get_current();
    char resolved_old[256], resolved_new[256];
    vfs_resolve_path(old_path, cur->cwd, resolved_old, sizeof(resolved_old));
    vfs_resolve_path(new_path, cur->cwd, resolved_new, sizeof(resolved_new));

    vmm_switch_address_space(vmm_get_kernel_pml4());
    int rc = vfs_rename(resolved_old, resolved_new);
    vmm_switch_address_space(cur->pml4_phys);

    return (int64_t)rc;
}

static int64_t sys_copy(uint64_t src_path_addr, uint64_t dst_path_addr)
{
    if (src_path_addr >= USER_ADDR_LIMIT) return -1;
    if (dst_path_addr >= USER_ADDR_LIMIT) return -1;

    char src_path[256], dst_path[256];

    const char *u_src = (const char *)(uintptr_t)src_path_addr;
    int i;
    for (i = 0; i < 255 && u_src[i]; i++) src_path[i] = u_src[i];
    src_path[i] = '\0';

    const char *u_dst = (const char *)(uintptr_t)dst_path_addr;
    for (i = 0; i < 255 && u_dst[i]; i++) dst_path[i] = u_dst[i];
    dst_path[i] = '\0';

    task_t *cur = scheduler_get_current();
    char resolved_src[256], resolved_dst[256];
    vfs_resolve_path(src_path, cur->cwd, resolved_src, sizeof(resolved_src));
    vfs_resolve_path(dst_path, cur->cwd, resolved_dst, sizeof(resolved_dst));

    vmm_switch_address_space(vmm_get_kernel_pml4());
    int rc = vfs_copy(resolved_src, resolved_dst);
    vmm_switch_address_space(cur->pml4_phys);

    return (int64_t)rc;
}

static int64_t sys_rmdir(uint64_t path_addr)
{
    if (path_addr >= USER_ADDR_LIMIT) return -1;

    char path[256];
    const char *user_path = (const char *)(uintptr_t)path_addr;
    int i;
    for (i = 0; i < 255 && user_path[i]; i++) path[i] = user_path[i];
    path[i] = '\0';

    task_t *cur = scheduler_get_current();
    char resolved[256];
    if (vfs_resolve_path(path, cur->cwd, resolved, sizeof(resolved)) != 0)
        return -1;

    vmm_switch_address_space(vmm_get_kernel_pml4());
    int rc = vfs_rmdir(resolved);
    vmm_switch_address_space(cur->pml4_phys);

    return (int64_t)rc;
}

/* ── GFX syscalls ────────────────────────────────────────────────────────*/

/*
 * sys_gfx_draw — Decode a GfxCmd from user space and call the appropriate
 *                kernel gfx_* drawing function.
 *
 * The caller (ring-3 task) passes a user-VA pointer to a GfxCmd struct.
 * We copy the struct to a kernel stack buffer, validate pointer fields,
 * copy any variable-length payload (string / pixel data) to a heap
 * buffer, then call the kernel gfx function.
 *
 * Drawing targets the back buffer (kernel heap, accessible with any CR3
 * at ring-0).  No CR3 switch is needed for drawing, only for flush.
 */
static int64_t sys_gfx_draw(uint64_t cmd_addr)
{
#define GFX_MAX_PTR_LEN  (4U * 1024U * 1024U)   /* 4 MB */
#define USER_ADDR_MAX    0x0000800000000000ULL

    if (cmd_addr >= USER_ADDR_MAX) return -1;
    if (cmd_addr + sizeof(GfxCmd) > USER_ADDR_MAX) return -1;

    /* Copy the command packet from user space. */
    GfxCmd cmd;
    const GfxCmd *ucmd = (const GfxCmd *)(uintptr_t)cmd_addr;
    for (size_t i = 0; i < sizeof(GfxCmd); i++)
        ((uint8_t *)&cmd)[i] = ((const uint8_t *)ucmd)[i];

    /* Validate variable-length pointer fields. */
    if (cmd.ptr != 0) {
        if (cmd.ptr >= USER_ADDR_MAX) return -1;
        if (cmd.ptr_len > GFX_MAX_PTR_LEN) return -1;
        if (cmd.ptr + cmd.ptr_len > USER_ADDR_MAX) return -1;
    }

    switch (cmd.op) {
    case GFX_OP_CLEAR:
        gfx_clear(cmd.color);
        break;

    case GFX_OP_FILL_RECT:
        gfx_fill_rect(cmd.x, cmd.y, cmd.w, cmd.h, cmd.color);
        break;

    case GFX_OP_DRAW_RECT:
        gfx_draw_rect(cmd.x, cmd.y, cmd.w, cmd.h, cmd.color);
        break;

    case GFX_OP_HLINE:
        gfx_hline(cmd.x, cmd.y, cmd.w, cmd.color);
        break;

    case GFX_OP_VLINE:
        gfx_vline(cmd.x, cmd.y, cmd.h, cmd.color);
        break;

    case GFX_OP_PUT_PIXEL:
        gfx_put_pixel(cmd.x, cmd.y, cmd.color);
        break;

    case GFX_OP_DRAW_STRING: {
        if (cmd.ptr == 0 || cmd.ptr_len == 0) return -1;
        if (cmd.ptr_len > 4096) return -1;  /* max string length */

        /* Copy string to kernel heap buffer (avoid 4 KB on the stack). */
        char *kstr = (char *)kmalloc(cmd.ptr_len + 1);
        if (!kstr) return -1;
        const char *ustr = (const char *)(uintptr_t)cmd.ptr;
        uint32_t i;
        for (i = 0; i < cmd.ptr_len; i++)
            kstr[i] = ustr[i];
        kstr[i] = '\0';

        uint32_t bg = (uint32_t)cmd.w;   /* w field repurposed as bg color */
        gfx_draw_string(cmd.x, cmd.y, kstr, cmd.color, bg);
        kfree(kstr);
        break;
    }

    case GFX_OP_BLIT: {
        if (cmd.ptr == 0 || cmd.ptr_len == 0) return -1;
        if (cmd.w <= 0 || cmd.h <= 0) return -1;
        uint64_t expected = (uint64_t)(uint32_t)cmd.w * (uint32_t)cmd.h * 4U;
        if (expected > GFX_MAX_PTR_LEN) return -1;
        if (cmd.ptr_len != (uint32_t)expected) return -1;

        /* Copy pixel data to kernel buffer. */
        uint32_t *kpix = (uint32_t *)kmalloc(cmd.ptr_len);
        if (!kpix) return -1;
        const uint8_t *usrc = (const uint8_t *)(uintptr_t)cmd.ptr;
        for (uint32_t i = 0; i < cmd.ptr_len; i++)
            ((uint8_t *)kpix)[i] = usrc[i];

        gfx_blit(cmd.x, cmd.y, kpix, cmd.w, cmd.h);
        kfree(kpix);
        break;
    }

    default:
        return -1;
    }

    return 0;

#undef GFX_MAX_PTR_LEN
#undef USER_ADDR_MAX
}

/*
 * sys_gfx_flush — Copy the back buffer to the hardware framebuffer.
 *
 * The hardware framebuffer is accessed via the identity map, which is only
 * present in the kernel page tables.  Switch to kernel CR3, flush, then
 * switch back.
 */
static int64_t sys_gfx_flush(void)
{
    task_t *cur = scheduler_get_current();

    /*
     * Let the window manager composite all windows into the back buffer.
     * wm_compose() only accesses kernel heap and the mouse ring buffer —
     * the identity map is not required, so no CR3 switch needed here.
     */
    wm_compose();

    /* gfx_flush() writes to the identity-mapped hardware framebuffer,
     * which requires the kernel page tables. */
    vmm_switch_address_space(vmm_get_kernel_pml4());
    gfx_flush();
    vmm_switch_address_space(cur->pml4_phys);

    return 0;
}

/*
 * sys_win_create — Create a new WM window.
 *
 * arg1 = title_addr : user VA of NUL-terminated title string
 * arg2 = x, arg3 = y : top-left corner (including title bar)
 * arg4 = w, arg5 = h : client area dimensions
 * Returns the window ID (>= 0) on success, -1 on failure.
 */
static int64_t sys_win_create(uint64_t title_addr, uint64_t x_u, uint64_t y_u,
                               uint64_t w_u, uint64_t h_u)
{
#define WIN_USER_MAX 0x0000800000000000ULL
    if (title_addr == 0 || title_addr >= WIN_USER_MAX) return -1;

    int w = (int)(int64_t)w_u;
    int h = (int)(int64_t)h_u;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return -1;

    /* Copy title string from user space (user CR3 is active; kernel
     * heap is mapped in all page tables via PML4[256–511]). */
    char title[64];
    const char *utitle = (const char *)(uintptr_t)title_addr;
    int i;
    for (i = 0; i < 63; i++) {
        char c = utitle[i];
        title[i] = c;
        if (c == '\0') break;
    }
    title[63] = '\0';

    task_t *cur = scheduler_get_current();
    return (int64_t)wm_create(title,
                              (int)(int64_t)x_u,
                              (int)(int64_t)y_u,
                              w, h,
                              (uint32_t)cur->id);
#undef WIN_USER_MAX
}

/*
 * sys_win_update — Copy user pixels into a window's kernel buffer.
 *
 * arg1 = win_id     : window ID returned by sys_win_create
 * arg2 = buf_addr   : user VA of w*h uint32_t pixels (0xAARRGGBB)
 * arg3 = user_size  : byte size of the pixel buffer (must equal w*h*4)
 * Returns 0 on success, -1 on error.
 */
static int64_t sys_win_update(uint64_t win_id, uint64_t buf_addr,
                               uint64_t user_size)
{
#define WIN_USER_MAX2 0x0000800000000000ULL
    if (win_id >= MAX_WINDOWS) return -1;

    window_t *w = g_windows[(int)win_id];
    if (!w || !w->in_use) return -1;

    uint64_t buf_size = (uint64_t)w->w * (uint64_t)w->h * 4ULL;
    if (buf_size == 0 || buf_size > 4U * 1024U * 1024U) return -1;

    /* Validate that the user-provided size matches the window dimensions.
     * A size of 0 is accepted for backward compatibility (old callers
     * that don't pass the size argument — the register will be zero). */
    if (user_size != 0 && user_size != buf_size) return -1;

    if (buf_addr == 0 || buf_addr >= WIN_USER_MAX2) return -1;
    if (buf_addr + buf_size > WIN_USER_MAX2) return -1;

    /* Direct copy: user buffer is in user page tables; window buffer is
     * in kernel heap (PML4[256–511], present in all address spaces). */
    const uint8_t *src = (const uint8_t *)(uintptr_t)buf_addr;
    uint8_t       *dst = (uint8_t *)w->buffer;
    for (uint64_t i = 0; i < buf_size; i++)
        dst[i] = src[i];

    return 0;
#undef WIN_USER_MAX2
}

/*
 * sys_gfx_info — Write screen width and height to a user-space buffer.
 *
 * arg1 = user VA of uint32_t[2];  out[0] = width, out[1] = height.
 */
static int64_t sys_gfx_info(uint64_t buf_addr)
{
#define USER_ADDR_MAX_GI  0x0000800000000000ULL
    if (buf_addr >= USER_ADDR_MAX_GI) return -1;
    if (buf_addr + 8 > USER_ADDR_MAX_GI) return -1;

    uint32_t *out = (uint32_t *)(uintptr_t)buf_addr;
    out[0] = gfx_screen_width();
    out[1] = gfx_screen_height();
    return 0;
#undef USER_ADDR_MAX_GI
}

/*
 * sys_key_poll — Non-blocking keyboard read.
 * Returns the next key character (0–255) if one is pending, or -1 if none.
 * Only delivers keys to the process that owns the focused WM window.
 */
static int64_t sys_key_poll(void)
{
    task_t *cur = scheduler_get_current();
    if (!wm_kbd_allowed(cur))
        return -1;

    char c;
    if (keyboard_read_char(&c))
        return (int64_t)(uint8_t)c;
    return -1;
}

/*
 * sys_get_event — Pop one event from the calling process's event queue.
 *
 * arg1 = user VA of event_t to fill.
 * Returns 0 if an event was written, -1 if the queue is empty.
 */
static int64_t sys_get_event(uint64_t out_addr)
{
#define EVT_USER_MAX 0x0000800000000000ULL
    if (out_addr == 0 || out_addr >= EVT_USER_MAX) return -1;
    if (out_addr + sizeof(event_t) > EVT_USER_MAX) return -1;

    task_t *cur = scheduler_get_current();

    /* Interrupts are already disabled by FMASK on syscall entry.
     * Do NOT call interrupts_enable() here — that would re-enable
     * interrupts in the middle of the syscall fast path, allowing
     * preemption before sysret and corrupting the return sequence. */

    if (cur->event_head == cur->event_tail)
        return -1;   /* Queue empty */

    event_t evt = cur->event_queue[cur->event_head];
    cur->event_head = (cur->event_head + 1) & (EVENT_QUEUE_SIZE - 1);

    /* Copy to user space (user CR3 is still active). */
    event_t *uout = (event_t *)(uintptr_t)out_addr;
    *uout = evt;

    return 0;
#undef EVT_USER_MAX
}

/* ── Dispatch ────────────────────────────────────────────────────────────*/

int64_t syscall_dispatch(uint64_t num, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
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
    case SYS_GETCWD:  return sys_getcwd(arg1, arg2);
    case SYS_CHDIR:   return sys_chdir(arg1);
    case SYS_FSSTAT:  return sys_fsstat(arg1);
    case SYS_FOPEN:   return sys_fopen(arg1);
    case SYS_FREAD:   return sys_fread(arg1, arg2, arg3);
    case SYS_FCLOSE:  return sys_fclose(arg1);
    case SYS_FSIZE:   return sys_fsize(arg1);
    case SYS_FSEEK:   return sys_fseek(arg1, arg2, arg3);
    case SYS_FTELL:   return sys_ftell(arg1);
    case SYS_FWRITE:  return sys_fwrite(arg1, arg2, arg3);
    case SYS_FCREATE: return sys_fcreate(arg1);
    case SYS_FDELETE: return sys_fdelete(arg1);
    case SYS_MKDIR:   return sys_mkdir(arg1);
    case SYS_RENAME:  return sys_rename(arg1, arg2);
    case SYS_COPY:    return sys_copy(arg1, arg2);
    case SYS_RMDIR:     return sys_rmdir(arg1);
    case SYS_GFX_DRAW:   return sys_gfx_draw(arg1);
    case SYS_GFX_FLUSH:  return sys_gfx_flush();
    case SYS_GFX_INFO:   return sys_gfx_info(arg1);
    case SYS_WIN_CREATE: return sys_win_create(arg1, arg2, arg3, arg4, arg5);
    case SYS_WIN_UPDATE: return sys_win_update(arg1, arg2, arg3);
    case SYS_KEY_POLL:   return sys_key_poll();
    case SYS_GET_EVENT:  return sys_get_event(arg1);
    case SYS_SHUTDOWN:   sys_shutdown(); return 0; /* never reached */
    case SYS_UPTIME:     return (int64_t)pit_get_ticks();
    default:
        serial_puts("[SYSCALL] Unknown syscall ");
        sc_put_dec(num);
        serial_puts("\n");
        return -1;
    }
}
