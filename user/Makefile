# user/Makefile — Build standalone user-space ELF programs.
#
# Links against libasos.a (user/libc) for libc functions.

CC       := gcc
LD       := ld

LIBC_DIR := libc
LIBC_INC := $(LIBC_DIR)/include
LIBC_LIB := $(LIBC_DIR)/libasos.a
LIBC_CRT := $(LIBC_DIR)/start.o

CFLAGS   := -ffreestanding -nostdlib -nostdinc -fno-builtin \
            -fno-stack-protector -mno-red-zone \
            -Wall -Wextra -O2 -I$(LIBC_INC)

LDFLAGS  := -T user.ld -nostdlib

PROGRAMS := hello.elf shell.elf echo.elf cat.elf loop.elf gfxtest.elf win_test.elf desktop.elf terminal.elf calc.elf pencil.elf

.PHONY: all clean libc

all: libc $(PROGRAMS)

libc:
	$(MAKE) -C $(LIBC_DIR)

%.elf: %.o $(LIBC_CRT) $(LIBC_LIB)
	$(LD) $(LDFLAGS) -o $@ $(LIBC_CRT) $< $(LIBC_LIB)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o *.elf
	$(MAKE) -C $(LIBC_DIR) clean
