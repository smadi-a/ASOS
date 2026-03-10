/*
 * user/hello.c — Standalone user program demonstrating libasos.
 *
 * Uses printf, malloc/free, strlen, and other libc functions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    printf("=========================\n");
    printf("Hello from an ELF binary!\n");
    printf("My PID is: %d\n", getpid());

    /* Test malloc + snprintf. */
    char *buf = malloc(128);
    if (buf) {
        snprintf(buf, 128, "malloc returned %p\n", (void *)buf);
        printf("%s", buf);

        /* Test strlen. */
        printf("strlen(buf) = %lu\n", (unsigned long)strlen(buf));

        free(buf);
        printf("free() succeeded\n");
    } else {
        printf("malloc() failed!\n");
    }

    /* Test counting with yield. */
    printf("Counting: ");
    for (int i = 1; i <= 5; i++) {
        printf("%d ", i);
        yield();
    }
    printf("\n");

    printf("ELF program exiting.\n");
    printf("=========================\n");
    return 0;
}
