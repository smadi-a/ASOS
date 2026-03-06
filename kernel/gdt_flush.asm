; kernel/gdt_flush.asm — LGDT, CS reload, segment register reload, LTR.
;
; After LGDT the CPU's segment descriptor caches are NOT automatically
; updated.  We must reload every segment register so the CPU fetches the
; fresh descriptors from our new GDT.
;
; Reloading CS is special: it cannot be done with a plain MOV.  Instead
; we use a 64-bit far return (RETFQ) to atomically pop RIP and CS off the
; stack, which causes the CPU to reload the CS descriptor cache.
;
; Calling convention: System V AMD64 ABI
;   gdt_flush(GDTDescriptor *desc)  — desc in RDI
;   tss_flush(uint16_t selector)    — selector in DI (low 16 bits of RDI)

bits 64

section .note.GNU-stack noalloc noexec nowrite progbits

section .text

; ── gdt_flush ─────────────────────────────────────────────────────────────
; void gdt_flush(GDTDescriptor *desc);
global gdt_flush
gdt_flush:
    lgdt    [rdi]           ; load new GDTR from the descriptor struct

    ; Reload CS via far return:
    ;   push the new code selector (kernel code = 0x08)
    ;   push the return address
    ;   RETFQ pops RIP first, then CS
    push    qword 0x08      ; new CS (kernel code selector)
    lea     rax, [rel .reload_cs]
    push    rax
    retfq                   ; far return: pops RIP then CS

.reload_cs:
    ; Reload the data segment registers with the kernel data selector (0x10).
    ; In 64-bit mode DS, ES, FS, GS, SS are mostly unused (bases/limits ignored)
    ; but the descriptor cache must still hold a valid present descriptor.
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    ret

; ── tss_flush ─────────────────────────────────────────────────────────────
; void tss_flush(uint16_t selector);
;
; Loads the Task Register with the given GDT selector, making the TSS
; descriptor described by that selector the active task state segment.
; Must be called AFTER gdt_flush so the new GDT is already installed.
global tss_flush
tss_flush:
    ltr     di              ; DI holds the selector (low 16 bits of RDI)
    ret
