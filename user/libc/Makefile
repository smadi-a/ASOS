# user/libc/Makefile — Build libasos.a static library.

CC       := gcc
ASM      := nasm
AR       := ar

CFLAGS   := -ffreestanding -nostdlib -nostdinc -fno-builtin \
            -fno-stack-protector -mno-red-zone \
            -Wall -Wextra -O2 -Iinclude

ASMFLAGS := -f elf64

SRCS     := src/string.c src/ctype.c src/errno.c src/unistd.c \
            src/stdio.c src/stdlib.c src/gfx.c
OBJS     := $(SRCS:.c=.o)
START_OBJ := start.o

LIB      := libasos.a

.PHONY: all clean

all: $(LIB) $(START_OBJ)

$(LIB): $(OBJS)
	$(AR) rcs $@ $(OBJS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(START_OBJ): start.asm
	$(ASM) $(ASMFLAGS) start.asm -o $(START_OBJ)

clean:
	rm -f $(OBJS) $(START_OBJ) $(LIB)
