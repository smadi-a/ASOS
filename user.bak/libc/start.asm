; user/libc/start.asm — C runtime entry point with BSS zeroing.
;
; _start: zero BSS, align stack, call main(), exit with return value.

section .note.GNU-stack noalloc noexec nowrite progbits

global _start
extern main
extern __bss_start
extern __bss_end

section .text
_start:
    ; Clear rbp to mark end of stack frames.
    xor rbp, rbp

    ; Zero BSS section.
    lea rdi, [rel __bss_start]
    lea rsi, [rel __bss_end]
.zero_bss:
    cmp rdi, rsi
    jge .bss_done
    mov byte [rdi], 0
    inc rdi
    jmp .zero_bss
.bss_done:

    ; Align stack to 16 bytes.
    and rsp, -16

    ; Call main().
    call main

    ; main() returned — exit with its return value.
    mov rdi, rax
    mov rax, 2          ; SYS_EXIT
    syscall

    ; Should never reach here.
.hang:
    jmp .hang
