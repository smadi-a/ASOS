# ASOS — Claude Code Guide

ASOS is a hobbyist x86-64 operating system written in C and NASM assembly, booting via UEFI (gnu-efi) and targeting QEMU + VirtualBox. It is a monolithic kernel designed with message-passing-friendly internal APIs for a future microkernel refactor.

## Build & Run

```bash
make            # Build bootloader, kernel, user programs, disk image, VDI
make run        # Launch QEMU (exit: Ctrl+A then X)
make clean      # Remove build/ (deps/ is preserved)
make deps       # Bootstrap local sysroot if system packages are missing
```

Serial output (COM1 at 115200-8N1) appears on stdout when running under QEMU via `-nographic`. Framebuffer output goes to the QEMU display window if `-nographic` is removed.

For VirtualBox: attach `build/asos.vdi` to an **IDE controller (Primary Master)**. SATA/AHCI will NOT work — the ATA PIO driver targets ports 0x1F0–0x1F7.

## Repository Layout

```
bootloader/main.c         UEFI application; sets up page tables, jumps to kernel
kernel/                   Monolithic kernel (C + NASM)
  main.c                  Entry: BSS clear → serial → fb → GDT → IDT → PMM → VMM → heap → gfx → wm → scheduler → DESKTOP.ELF
  *.c / *.h               One subsystem per file pair (see Subsystems below)
  *.asm                   NASM assembly (GDT flush, ISR stubs, context switch, syscall entry)
shared/boot_info.h        BootInfo struct shared between bootloader and kernel
shared/gfx.h              GfxCmd struct + GFX_OP_* constants (shared kernel/user)
linker.ld                 Kernel linker script (VMA 0xFFFFFFFF80100000, LMA 0x100000)
user/                     Ring-3 user programs
  libc/                   libasos.a — freestanding C runtime (printf, malloc, string, etc.)
  shell.c, hello.c, …     User programs linked against libasos.a
  desktop.c, win_test.c   GUI user programs (window manager clients)
  gfxtest.c, loop.c       Additional user programs
  user.ld                 User linker script (base 0x400000)
  start.asm               _start: zero BSS, align stack, call main, exit
build/                    All build outputs (gitignored)
deps/                     Downloaded package sysroot (gitignored, NOT cleaned by make clean)
```

## Toolchain

| Component | Tool |
|-----------|------|
| Kernel/user C | `gcc` (host x86-64, or `x86_64-elf-gcc` via `CROSS=x86_64-elf-`) |
| NASM assembly | `nasm -f elf64` |
| Bootloader | `gcc` (host, with gnu-efi) + `objcopy` → PE32+ `.efi` |
| Linker | `ld` (no host sysroot; kernel uses `linker.ld`) |
| Disk image | `parted` (GPT) + `mtools` (FAT32, no root needed) |
| Emulator | `qemu-system-x86_64` + OVMF firmware |

### Kernel CFLAGS (critical flags)

```
-ffreestanding -nostdlib -mno-red-zone -mcmodel=kernel -fno-pic -fno-pie
-mno-mmx -mno-sse -mno-sse2 -O2 -Wall -Wextra -std=c11
```

Never use `-msse`, `-mavx`, or any flag that enables SSE/AVX — the kernel does not save/restore FPU state.

### NASM files

All `.asm` files must include:
```nasm
section .note.GNU-stack noalloc noexec nowrite progbits
```

## Kernel Subsystems

### Memory Layout

| Region | Virtual Address | Notes |
|--------|----------------|-------|
| Identity map | 0x0 – 0xFFFFFFFF | 2 MB pages; first 2 MB is null-guard (unmapped) |
| Kernel text/data | 0xFFFFFFFF80100000 | VMA; LMA = 0x100000 |
| Kernel heap | 0xFFFFFFFF90000000 | 16 MB free-list allocator |
| User code | 0x400000 | ELF PT_LOAD segments |
| User stack | 0x7FFFFFF00000 | 16 KB, grows down |

`PHYS_TO_VIRT(pa) = pa + 0xFFFFFFFF80000000`
`VIRT_TO_PHYS(va) = va − 0xFFFFFFFF80000000`

### PMM (`pmm.c`)

Bitmap frame allocator. 1 M bits (128 KB BSS), 0 = free, 1 = used. All bits start used; `pmm_init` marks `EfiConventionalMemory` free then re-marks reserved regions. First 2 MB is always reserved (null guard + identity-map 2 MB page boundary). Returns zeroed frames.

### VMM (`vmm.c`)

4-level page tables. `vmm_map_page(pml4_phys, virt, phys, flags)` walks/creates tables via the identity map. `vmm_create_user_address_space` copies PML4[256–511] (kernel higher-half) but NOT PML4[0] (identity map conflicts with 4 KB user mappings).

### Heap (`heap.c`)

Free-list `kmalloc`/`kfree` over 16 MB (4096 frames) at `0xFFFFFFFF90000000`. Size was increased from 1 MB to accommodate the GFX back buffer (~3 MB at 1024×768×4 bytes).

### GDT / TSS (`gdt.c`, `tss.c`)

| Index | Selector | Descriptor |
|-------|----------|-----------|
| 0 | 0x00 | null |
| 1 | 0x08 | kernel code |
| 2 | 0x10 | kernel data |
| 3 | 0x18 | user data |
| 4 | 0x20 | user code |
| 5 | 0x28 | TSS (16 bytes) |

User selectors with RPL=3: data = `0x1B`, code = `0x23`. TSS IST1 = 16 KB double-fault stack.

### IDT / ISR (`idt.c`, `isr.c`, `isr_stubs.asm`)

256 ISR stubs. `isr_stub_table` (array of 256 fn ptrs) dispatches to `g_handlers[256]`. Vectors 32–47 = IRQs (call handler then EOI). Unregistered IRQ vectors: silent EOI + return. Unregistered exceptions 0–31: panic screen.

`InterruptFrame` layout (low → high): r15..rax, vector, error_code, rip, cs, rflags, rsp, ss.
Vectors with error codes: 8, 10, 11, 12, 13, 14, 17, 21, 29, 30.

### PIC / PIT / Keyboard (`pic.c`, `pit.c`, `keyboard.c`)

- PIC: ICW1–4 init, IRQ0–15 remapped to vectors 32–47, all masked after init; EOI: slave first if IRQ ≥ 8, then master.
- PIT: channel 0, mode 3, divisor = 1193 → 1000 Hz. `g_ticks` (volatile uint64_t) incremented in IRQ0 handler.
- Keyboard: scan code set 1, 89-entry normal/shifted tables. Port 0x60 read on EVERY IRQ1. Shift tracked via 0x2A/0x36. Break = bit 7. Output to `ring_buffer`.
- `STI` comes **after** PIC + PIT + keyboard init in `main.c`.

### Mouse (`mouse.c`)

PS/2 mouse driver. Handles standard 3-byte PS/2 packets. Mouse events (dx, dy, buttons) stored in a ring buffer via IRQ 12. Initialized after keyboard. API: `mouse_init()`, `mouse_read_event(mouse_event_t*)`, `mouse_has_event()`.

### ATA / GPT / FAT32 / VFS

- ATA PIO driver targets primary IDE channel (0x1F0–0x1F7).
- GPT partition table parsing (`gpt.c`).
- FAT32 read+write driver (`fat32.c`): supports subdirectories, mkdir, rename, move, copy.
- VFS layer (`vfs.c`): path resolution via `fat32_resolve_dir`, full path support for open/create/delete/list/mkdir/rename/copy.

### Multitasking (`process.c`, `scheduler.c`, `context_switch.asm`)

- `task_t`: id, name, state, kernel_rsp, stack, entry, time_slice_remaining, pml4_phys, heap fields, parent_pid, exit_status, doubly-linked list pointers.
- Round-robin scheduler with preemption (`TIME_SLICE_TICKS = 20`).
- `context_switch.asm`: saves/restores 6 callee-saved registers + RSP swap.
- Kernel threads start via `kernel_thread_start` (EOI, switch CR3, STI, call entry).
- User processes start via `user_process_start` (EOI, switch CR3, IRETQ to ring 3, RFLAGS = 0x202).
- TSS RSP0 updated on every context switch (`tss_set_rsp0`).
- Per-process page tables: user code at 0x400000 (R+X), user stack at 0x7FFFFFF00000 (RW+NX).

### Syscall Interface (`syscall.c`, `syscall_entry.asm`)

Fast path via SYSCALL/SYSRET (STAR/LSTAR/FMASK MSRs).
- STAR: `(0x10 << 48) | (0x08 << 32)`.
- FMASK: 0x600 (clears IF + DF on entry).
- `syscall_entry.asm` saves **all** caller-saved registers (rdi/rsi/rdx/r8/r9/r10) before calling C handler. This is required because GCC's inline `syscall` clobber list only declares rcx/r11/memory.
- `g_syscall_rsp0` global mirrors TSS RSP0 for asm access.

Key syscalls:

| # | Name | Description |
|---|------|-------------|
| 0 | `read` | Read from stdin (keyboard) |
| 1 | `write` | Write to stdout (serial) |
| 2 | `exit` | Terminate process |
| 3 | `getpid` | Get current PID |
| 4 | `yield` | Yield CPU to scheduler |
| 5 | `sbrk` | Grow user heap |
| 6 | `waitpid` | Wait for child process |
| 7 | `spawn` | Spawn new ELF process |
| 8 | `readdir` | List directory entries |
| 9 | `pidof` | Find PID by name |
| 10 | `kill` | Kill process by PID |
| 11 | `proclist` | List all processes |
| 12 | `getcwd` | Get current working directory |
| 13 | `chdir` | Change working directory |
| 14 | `fsstat` | Filesystem statistics |
| 15–20 | `fopen/fread/fclose/fsize/fseek/ftell` | File descriptor I/O |
| 21–23 | `fwrite/fcreate/fdelete` | File write/create/delete |
| 24 | `mkdir` | Create directory |
| 25 | `rename` | Rename file/directory |
| 26 | `copy` | Copy file |
| 27 | `rmdir` | Remove directory |
| 28 | `gfx_draw` | Submit GfxCmd to back buffer |
| 29 | `gfx_flush` | Compose windows + blit back buffer to framebuffer |
| 30 | `gfx_info` | Query display dimensions (w, h) |
| 31 | `win_create` | Create a WM window (title, x, y, w, h) → win_id |
| 32 | `win_update` | Upload pixel buffer to window client area |
| 33 | `key_poll` | Non-blocking keyboard read → char or -1 |

### GFX Library (`gfx.c`, `gfx.h`, `shared/gfx.h`)

Double-buffered 2D graphics library. A back buffer is allocated via `kmalloc` at init time; drawing operations target the back buffer; `gfx_flush` runs `wm_compose()` then blits row-by-row to the GOP linear framebuffer (handles pitch ≠ width×4).

- **`shared/gfx.h`**: `GfxCmd` struct + `GFX_OP_*` constants shared between kernel and user space.
- **Operations**: `GFX_OP_FILL_RECT`, `GFX_OP_DRAW_RECT`, `GFX_OP_PUT_PIXEL`, `GFX_OP_HLINE` (length in `w`), `GFX_OP_VLINE` (length in `h`), `GFX_OP_DRAW_STRING` (`w` = bg_color), `GFX_OP_CLEAR`, `GFX_OP_BLIT` (x, y, src_w, src_h, ptr, ptr_len).
- **Font**: 8×16 VGA bitmap font.
- **Color**: stored as BGRA; RGB input swaps R↔B at draw time if framebuffer uses BGR layout.
- **Syscall flow**: user calls `gfx_draw(cmd)` → `SYS_GFX_DRAW(28)` copies cmd to kernel and renders into back buffer. `gfx_flush()` → `SYS_GFX_FLUSH(29)` runs compositor then copies back buffer to framebuffer (requires kernel CR3 for identity-mapped framebuffer access). `gfx_info(w,h)` → `SYS_GFX_INFO(30)` returns display dimensions.
- **User pointer validation**: ptr < 0x7FFFFFFFFFFF and size ≤ 4 MB checked before kernel dereference.
- No CR3 switch needed for `sys_gfx_draw` (kernel heap accessible with any CR3); CR3 switch required in `sys_gfx_flush` only.

### Window Manager (`wm.c`, `wm.h`)

Layered compositor and window manager. Manages up to 16 windows (slot 0 = root/wallpaper, slots 1–15 = user windows).

- **`window_t`**: id, x, y, w, h, title[64], buffer (kernel-heap pixel buffer), flags, focused, in_use.
- **Flags**: `WM_FLAG_NO_MOVE`, `WM_FLAG_NO_CLOSE`, `WM_FLAG_BOTTOM`.
- **API**: `wm_init()`, `wm_create(title, x, y, w, h)` → win_id, `wm_destroy(win_id)`, `wm_update_window(win_id, pixels)`, `wm_handle_mouse(x, y, clicked)`, `wm_compose()`.
- **Compositor layer order** (Painter's Algorithm): root window (wallpaper, 0x223344) → normal windows back-to-front (title bar 20 px + close button + client area) → taskbar (28 px at top: tabs + system clock) → mouse cursor (8×8).
- **Taskbar**: active tab = 0x3A6DB5 (blue), inactive = 0x3C3C3C; system clock HH:MM:SS right-aligned.
- **Focus**: new windows steal focus; `wm_handle_mouse` hit-tests top-to-bottom (highest slot first), handles close button and drag.
- **Syscall handlers**: `sys_win_create` validates title ptr + copies 64 bytes; `sys_win_update` validates win_id + pixel buf, copies w×h×4 bytes; `sys_key_poll` non-blocking keyboard drain.
- `wm_compose()` is called automatically inside `sys_gfx_flush` before blitting.

### ELF Loader (`elf.c`)

Validates ELF64 header, maps PT_LOAD segments into per-process page tables via identity map. Page flags from ELF `p_flags`: `PF_W` → `PTE_WRITABLE`, `!PF_X` → `PTE_NO_EXEC`. BSS handled automatically (PMM returns zeroed frames).

## User Space

### libasos (`user/libc/`)

Freestanding static library. Headers in `user/libc/include/`. Key modules:
- `stdio`: `printf`/`vsnprintf` (width, zero-pad, %d/%u/%x/%p/%s/%c/%%/`%l`), `getchar`, `gets_s`.
- `stdlib`: `malloc`/`free` (free-list, 16-byte aligned, split+coalesce), `exit`.
- `string`, `ctype`, `unistd` (spawn, waitpid, read, write, mkdir, rename, copy), `errno`.
- `gfx`: `gfx_draw(GfxCmd*)` → `SYS_GFX_DRAW`; `gfx_flush_display()` → `SYS_GFX_FLUSH`; `gfx_screen_info(w,h)` → `SYS_GFX_INFO`; inline helpers in `gfx.h` (gfx_clear, gfx_fill_rect, gfx_draw_rect, gfx_put_pixel, gfx_hline, gfx_vline, gfx_puts). Color constants in `gfx_colors.h`.
- `wm`: `win_create(title, x, y, w, h)` → `SYS_WIN_CREATE`; `win_update(win_id, pixels)` → `SYS_WIN_UPDATE`; `key_poll()` → `SYS_KEY_POLL`. Inline wrappers declared directly in user programs (not yet in libasos).
- `malloc` backed by `sbrk` syscall; heap grows from top of highest ELF PT_LOAD segment up to user stack.

### User programs

All linked at 0x400000 with `user.ld`. Start via `user/libc/start.asm` (_start: zero BSS, align stack, call main, sys_exit).

String literals work in ELF programs (`.rodata` is mapped). String literals do **not** work in in-kernel user programs (M7A style) — use stack-allocated char arrays there.

### User Programs

- `hello.c` — Hello world demo
- `echo.c` — Echo arguments to stdout
- `cat.c` — Print file contents
- `gfxtest.c` — GFX demo (draws shapes + text, runs as `GFXTEST.ELF`)
- `loop.c` — Repeatedly prints a string (scheduler stress test)
- `desktop.c` — Init process (PID 2); creates Terminal WM window (600×380), spawns `SHELL.ELF` (PID 3), drives compositor via `gfx_flush_display()` render loop. Static pixel buffer (~891 KB BSS). Does NOT use `key_poll` (would steal keys from shell).
- `win_test.c` — WM test app; bouncing orange square (20×20) over teal gradient background in a 200×150 window; exits on 'Q' via `key_poll`.

### Shell (`user/shell.c`)

Built-in commands: `help`, `ls`/`l`, `cd`/`go`, `pwd`/`path`, `mkdir`/`md`, `touch`/`new`, `cp`/`copy`, `mv`/`move`, `rm`/`del`, `cat`/`show`, `head`/`top`, `tail`/`bottom`, `echo`/`say`, `kill`/`end`, `df`/`disk`, `clear`/`clean`, `pid`, `exec`, `halt`, `exit`, `pidof`, `proc`.

External programs: shell uppercases the name and appends `.ELF` if no extension, then calls `spawn()`.

## Critical Implementation Notes

1. **RSP switch + function call**: `kernel_main` switches RSP to `g_kstack` (BSS) then immediately calls `kernel_main2` (`__attribute__((noinline))`). Do NOT continue in the same function after switching RSP — with `-O2`, GCC uses RSP-relative addressing for locals, and the inline switch corrupts all subsequent local variable access.

2. **CR3 before PMM/VMM**: Any syscall that touches PMM or VMM (e.g., `sys_sbrk`, `sys_spawn`) **must** switch to the kernel CR3 first. The user page tables lack the identity map needed to walk/allocate physical frames. Switch back to the user CR3 before returning.

3. **User helper functions**: Must be `__attribute__((always_inline))` if defined in in-kernel user programs. Code is copied to 0x400000; relative calls would jump to wrong addresses.

4. **Kernel heap size**: 16 MB (4096 frames at `0xFFFFFFFF90000000`). Increased from 256 KB → 1 MB (for multiple processes) → 16 MB (for GFX back buffer at ~3 MB for 1024×768).

5. **Identity map gap**: First 2 MB is unmapped (null guard). PMM never allocates below frame 512 (2 MB). `vmm_init` skips the first 2 MB when building the identity map.

6. **PML4 copying**: `vmm_create_user_address_space` copies PML4[256–511] only. PML4[0] (identity map) is intentionally excluded — 1 GB pages in the identity map conflict with 4 KB user page mappings.

7. **NASM stack note**: Every `.asm` file needs `section .note.GNU-stack noalloc noexec nowrite progbits` or the linker will mark the stack executable.

8. **VirtualBox IDE only**: The ATA PIO driver targets 0x1F0–0x1F7. A SATA/AHCI controller will not be detected.

9. **EOI ordering**: For IRQ ≥ 8 (slave PIC), send EOI to slave (0xA0) first, then to master (0x20).

## Next Milestones

- [x] Graphics framebuffer library
- [x] Window manager and compositor
- [x] Desktop environment + GUI toolkit
- [ ] PCI bus enumeration
- [ ] Network driver + TCP/IP stack
- [ ] Buddy allocator (replace bitmap PMM)
- [ ] AHCI driver
- [ ] Slab allocator
- [ ] APIC support (disable PIC)
- [ ] Microkernel refactor
