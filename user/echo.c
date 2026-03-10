/*
 * user/echo.c — Simple test program.
 */

#include <stdio.h>
#include <unistd.h>

int main(void)
{
    printf("echo: running (no argv support yet)\n");
    printf("echo: my PID is %d\n", getpid());
    return 0;
}
