; kernel/context_switch.asm — Save/restore callee-saved registers and RSP.
;
; void context_switch(uint64_t *old_rsp_ptr, uint64_t new_rsp)
;   rdi = pointer to where we store the current RSP (old task's kernel_rsp)
;   rsi = the RSP value to load (new task's kernel_rsp)
;
; Called as a normal C function.  The C compiler already saved any
; caller-saved registers it needs.  We preserve the six callee-saved
; GPRs (SysV AMD64 ABI: rbp rbx r12 r13 r14 r15).
;
; For an existing task, 'ret' returns into scheduler_yield() where
; that task previously called context_switch.
; For a brand-new task, 'ret' jumps to the entry_point placed on
; its initial stack by task_create_kernel().

; Mark the stack as non-executable (suppresses GNU ld warning).
section .note.GNU-stack noalloc noexec nowrite progbits

section .text
bits 64

global context_switch
context_switch:
    ; Save callee-saved registers onto the CURRENT stack.
    push    rbp
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15

    ; Save current stack pointer into *old_rsp_ptr.
    mov     [rdi], rsp

    ; Load new task's stack pointer.
    mov     rsp, rsi

    ; Restore callee-saved registers from the NEW stack.
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    pop     rbp

    ; Return into the new task's context.
    ret
