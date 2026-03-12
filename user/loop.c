/*
 * loop.c — Infinite loop test program for Ctrl+C testing.
 *
 * Prints a counter every ~500 iterations (with yields in between)
 * so the user can verify Ctrl+C terminates the process.
 */

#include <stdio.h>
#include <unistd.h>

int main(void)
{
    printf("Loop running (press Ctrl+C to stop)...\n");

    unsigned long count = 0;
    while (1) {
        count++;
        if ((count % 500) == 0) {
            printf("loop: iteration %lu\n", count);
        }
        yield();
    }

    return 0;
}
