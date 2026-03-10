; user/start.asm — Minimal C runtime entry point for user programs.
;
; The ELF entry point is _start, which calls main() and then exits
; with main's return value via SYS_EXIT.

section .note.GNU-stack noalloc noexec nowrite progbits

global _start
extern main

section .text
_start:
    ; Clear rbp to mark the end of stack frames for debuggers.
    xor rbp, rbp

    ; Align stack to 16 bytes.
    and rsp, -16

    ; Call main().
    call main

    ; main() returned — exit with its return value.
    ; RAX = main's return value → arg1 (RDI) for SYS_EXIT.
    mov rdi, rax
    mov rax, 2          ; SYS_EXIT
    syscall

    ; Should never reach here.
.hang:
    jmp .hang
