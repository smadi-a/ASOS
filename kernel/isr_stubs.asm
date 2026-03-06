; kernel/isr_stubs.asm — 256 interrupt/exception entry stubs + common handler.
;
; Each of the 256 CPU vectors gets its own tiny stub that:
;   1. Pushes a dummy error code (0) for vectors where the CPU does NOT
;      push one automatically, so the stack layout is always identical.
;   2. Pushes its vector number.
;   3. Jumps to the common stub.
;
; Vectors that the CPU pushes an error code for:
;   8  #DF  Double Fault
;   10 #TS  Invalid TSS
;   11 #NP  Segment Not Present
;   12 #SS  Stack-Segment Fault
;   13 #GP  General Protection Fault
;   14 #PF  Page Fault
;   17 #AC  Alignment Check
;   21 #CP  Control Protection Exception
;   29 #VC  VMM Communication Exception (AMD)
;   30 #SX  Security Exception
;
; All other vectors: stub pushes dummy 0 before the vector number.
;
; Stack layout entering isr_common_stub (see isr.h for the full diagram):
;
;   [CPU pushed if ring change or always-in-64bit]: SS, RSP, RFLAGS, CS, RIP
;   [Stub pushed]:                                  error_code, vector
;   [Common stub pushes]:                           rax..r15 (see below)

bits 64

; Mark the stack as non-executable (suppresses GNU ld warning).
section .note.GNU-stack noalloc noexec nowrite progbits

section .text

extern isr_handler          ; C function defined in isr.c

; ── Macros ────────────────────────────────────────────────────────────────

; ISR_NOERR vec — stub for a vector where the CPU does NOT push an error code
%macro ISR_NOERR 1
global isr_stub_%1
isr_stub_%1:
    push    qword 0         ; dummy error code
    push    qword %1        ; vector number
    jmp     isr_common_stub
%endmacro

; ISR_ERR vec — stub for a vector where the CPU already pushed an error code
%macro ISR_ERR 1
global isr_stub_%1
isr_stub_%1:
                            ; error code already on stack (pushed by CPU)
    push    qword %1        ; vector number
    jmp     isr_common_stub
%endmacro

; ── Per-vector stubs ───────────────────────────────────────────────────────

; Vectors 0–7: no error code
%assign i 0
%rep 8
    ISR_NOERR i
    %assign i i+1
%endrep

ISR_ERR   8    ; #DF  Double Fault          (error code = 0, CPU still pushes it)

ISR_NOERR 9    ; Coprocessor Segment Overrun (no error code, legacy)

ISR_ERR   10   ; #TS  Invalid TSS
ISR_ERR   11   ; #NP  Segment Not Present
ISR_ERR   12   ; #SS  Stack-Segment Fault
ISR_ERR   13   ; #GP  General Protection Fault
ISR_ERR   14   ; #PF  Page Fault

ISR_NOERR 15   ; Reserved

ISR_NOERR 16   ; #MF  x87 FPU Floating-Point Error

ISR_ERR   17   ; #AC  Alignment Check

ISR_NOERR 18   ; #MC  Machine Check
ISR_NOERR 19   ; #XM  SIMD Floating-Point Exception
ISR_NOERR 20   ; #VE  Virtualisation Exception

ISR_ERR   21   ; #CP  Control Protection Exception

; Vectors 22–28: reserved, no error code
%assign i 22
%rep 7
    ISR_NOERR i
    %assign i i+1
%endrep

ISR_ERR   29   ; #VC  VMM Communication Exception (AMD SVM)
ISR_ERR   30   ; #SX  Security Exception

ISR_NOERR 31   ; Reserved

; Vectors 32–255: hardware IRQs and user-defined interrupts (no error code)
%assign i 32
%rep 224
    ISR_NOERR i
    %assign i i+1
%endrep

; ── Common stub ───────────────────────────────────────────────────────────
;
; At entry, the stack contains (top = lowest address):
;   [rsp+0]   vector number (pushed by stub above)
;   [rsp+8]   error code    (CPU-pushed or dummy 0)
;   [rsp+16]  rip     ─┐
;   [rsp+24]  cs       │  CPU-pushed hardware frame
;   [rsp+32]  rflags   │  (always 5 × 8 = 40 bytes in 64-bit mode)
;   [rsp+40]  rsp      │
;   [rsp+48]  ss      ─┘
;
; We save all GPRs, call isr_handler(InterruptFrame *), then restore.
;
isr_common_stub:
    ; Save GPRs in the order that matches InterruptFrame (isr.h).
    ; First pushed = highest address in the frame.
    push    rax
    push    rbx
    push    rcx
    push    rdx
    push    rsi
    push    rdi
    push    rbp
    push    r8
    push    r9
    push    r10
    push    r11
    push    r12
    push    r13
    push    r14
    push    r15             ; last pushed → lowest address = RSP

    ; Load kernel data segments in case we interrupted user-mode code
    ; (irrelevant for Milestone 2, but correct for later).
    mov     ax, 0x10        ; SEL_KERNEL_DATA
    mov     ds, ax
    mov     es, ax

    ; First argument to isr_handler: pointer to the InterruptFrame.
    ; RSP currently points to the saved r15 (start of the frame).
    mov     rdi, rsp

    ; Align the stack to 16 bytes before the call (SysV AMD64 ABI).
    ; Save our frame pointer in RBP; restore after the call.
    ; (The saved RBP in the frame is at [rsp+64]; this usage of the
    ; register is fine because we restore RSP—not RBP—from our saved value.)
    mov     rbp, rsp
    and     rsp, -16        ; clear low 4 bits (stack grows downward, so
                            ; this can only decrease RSP, never corrupt frame)
    call    isr_handler
    mov     rsp, rbp        ; restore RSP to point at saved r15

    ; Restore GPRs in reverse push order.
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rbp
    pop     rdi
    pop     rsi
    pop     rdx
    pop     rcx
    pop     rbx
    pop     rax

    add     rsp, 16         ; discard vector (8) + error_code (8)
    iretq                   ; restore RIP, CS, RFLAGS, RSP, SS

; ── Stub pointer table ────────────────────────────────────────────────────
;
; isr_stub_table[i] is the address of isr_stub_i.
; idt.c reads this array to populate the IDT.

section .rodata
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 256
    dq isr_stub_%+i
    %assign i i+1
%endrep
