/*
 * user/cat.c — Read from stdin and echo to stdout.
 */

#include <stdio.h>
#include <unistd.h>

int main(void)
{
    printf("cat: reading from stdin (Ctrl-Q to exit)\n");
    while (1) {
        int c = getchar();
        if (c == EOF) break;
        if (c == ('q' - 'a' + 1)) break;  /* Ctrl-Q */
        putchar(c);
    }
    printf("\n");
    return 0;
}
