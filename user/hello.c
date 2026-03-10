/*
 * user/hello.c — Standalone user program loaded from an ELF binary.
 *
 * Demonstrates syscalls: write, getpid, yield, exit.
 * Compiled freestanding — no libc, no kernel headers.
 */

#include "syscall.h"

static int strlen(const char *s)
{
    int len = 0;
    while (s[len]) len++;
    return len;
}

static void print(const char *s)
{
    write(1, s, (uint64_t)strlen(s));
}

static void print_num(int64_t n)
{
    if (n < 0) {
        write(1, "-", 1);
        n = -n;
    }
    char buf[20];
    int i = 0;
    if (n == 0) {
        buf[i++] = '0';
    } else {
        while (n > 0) {
            buf[i++] = (char)('0' + (int)(n % 10));
            n /= 10;
        }
    }
    /* Reverse and print. */
    for (int j = i - 1; j >= 0; j--)
        write(1, &buf[j], 1);
}

int main(void)
{
    print("=========================\n");
    print("Hello from an ELF binary!\n");
    print("My PID is: ");
    print_num(getpid());
    print("\n");

    print("Counting: ");
    for (int i = 1; i <= 5; i++) {
        print_num((int64_t)i);
        print(" ");
        yield();
    }
    print("\n");

    print("ELF program exiting.\n");
    print("=========================\n");
    return 0;
}
