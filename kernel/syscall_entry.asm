; kernel/syscall_entry.asm — Fast-path ring 3 → ring 0 via syscall.
;
; On entry from the syscall instruction:
;   RCX = user RIP (saved by CPU)
;   R11 = user RFLAGS (saved by CPU)
;   RAX = syscall number
;   RDI = arg1, RSI = arg2, RDX = arg3, R10 = arg4, R8 = arg5, R9 = arg6
;   RSP = user RSP (CPU does NOT switch stacks)
;   IF  = 0 (FMASK cleared it)
;
; We must save the user RSP ourselves, switch to the kernel stack,
; call the C dispatcher, then sysretq back to user space.
;
; All caller-saved registers (rdi, rsi, rdx, r8, r9, r10) are preserved
; across the syscall.  RAX carries the return value.  RCX and R11 are
; used by the CPU for RIP and RFLAGS (documented syscall convention).
;
; user_rsp_scratch is safe as a single global because interrupts are
; disabled and we are single-CPU.

; Mark the stack as non-executable (suppresses GNU ld warning).
section .note.GNU-stack noalloc noexec nowrite progbits

section .data
global user_rsp_scratch
user_rsp_scratch: dq 0

section .text
global syscall_entry
extern syscall_dispatch
extern g_syscall_rsp0

syscall_entry:
    ; ── Save user RSP and switch to kernel stack ──────────────────────
    mov [rel user_rsp_scratch], rsp
    mov rsp, [rel g_syscall_rsp0]

    ; ── Build a frame so we can restore state after the C call ────────
    push qword [rel user_rsp_scratch]   ; saved user RSP
    push r11                             ; saved user RFLAGS
    push rcx                             ; saved user RIP

    ; Save callee-saved registers (must survive the C call)
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; Save caller-saved registers that user code depends on.
    ; The inline asm clobber list only declares rcx, r11, memory,
    ; so GCC may keep values in rdi/rsi/rdx/r8/r9/r10 across syscalls.
    push rdi
    push rsi
    push rdx
    push r8
    push r9
    push r10

    ; ── Prepare arguments for the C dispatcher ────────────────────────
    ;   syscall_dispatch(num, arg1, arg2, arg3, arg4, arg5)
    ;   C convention: rdi, rsi, rdx, rcx, r8, r9
    ;
    ; Current: RAX=num, RDI=a1, RSI=a2, RDX=a3, R10=a4, R8=a5
    ; Target:  RDI=num, RSI=a1, RDX=a2, RCX=a3, R8=a4,  R9=a5
    mov r9, r8              ; arg5 -> r9
    mov r8, r10             ; arg4 -> r8
    mov rcx, rdx            ; arg3 -> rcx
    mov rdx, rsi            ; arg2 -> rdx
    mov rsi, rdi            ; arg1 -> rsi
    mov rdi, rax            ; syscall number -> rdi

    ; Stack alignment: RSP0 was 16-aligned (stack top). We pushed
    ; 15 values (120 bytes). 120 mod 16 = 8, so RSP mod 16 = 8.
    ; call pushes 8 more → RSP mod 16 = 0 inside callee. Correct.
    call syscall_dispatch

    ; RAX = return value for user space

    ; ── Restore registers ────────────────────────────────────────────
    pop  r10
    pop  r9
    pop  r8
    pop  rdx
    pop  rsi
    pop  rdi

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp

    ; ── Restore user context and return ───────────────────────────────
    pop rcx             ; user RIP  (sysretq loads RIP from RCX)
    pop r11             ; user RFLAGS (sysretq loads RFLAGS from R11)

    ; Use a register we've already restored to hold user RSP.
    ; r10 is safe — its user value was restored above, and sysret
    ; does not use r10.  We immediately overwrite RSP, then sysret
    ; restores r10's "purpose" by returning to user code which
    ; never sees the brief overwrite.
    ;
    ; Wait — we need r10 intact for the user.  Use the stack:
    ; RSP currently points at the saved user RSP (last value).
    ; Pop into rsp directly would lose the kernel stack — but we
    ; are done with it.  Instead, load and switch:
    mov rsp, [rsp]      ; load saved user RSP and switch in one step

    ; sysretq: loads RIP←RCX, RFLAGS←R11, CS/SS from STAR MSR.
    ; R11 has IF set, so interrupts are re-enabled in user space.
    o64 sysret
