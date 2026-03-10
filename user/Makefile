# user/Makefile — Build standalone user-space ELF programs.
#
# Uses the same compiler as the kernel (host gcc on x86_64 Linux).
# All programs are fully freestanding — no libc, no dynamic linking.

CC       := gcc
ASM      := nasm
LD       := ld

CFLAGS   := -ffreestanding -nostdlib -nostdinc -fno-builtin \
            -fno-stack-protector -mno-red-zone \
            -Wall -Wextra -O2

ASMFLAGS := -f elf64
LDFLAGS  := -T user.ld -nostdlib

PROGRAMS := hello.elf

.PHONY: all clean inspect

all: $(PROGRAMS)

hello.elf: start.o hello.o
	$(LD) $(LDFLAGS) -o $@ start.o hello.o

hello.o: hello.c syscall.h
	$(CC) $(CFLAGS) -c hello.c -o hello.o

start.o: start.asm
	$(ASM) $(ASMFLAGS) start.asm -o start.o

inspect: hello.elf
	readelf -h hello.elf
	readelf -l hello.elf
	readelf -S hello.elf

clean:
	rm -f *.o *.elf
