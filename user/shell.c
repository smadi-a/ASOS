/*
 * user/shell.c — ASOS interactive command shell.
 *
 * Reads commands, runs built-ins or spawns ELF programs from the
 * filesystem and waits for them to complete.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#define MAX_CMD_LEN  256
#define MAX_PATH_LEN 256

/* ── Built-in commands ──────────────────────────────────────────────── */

static void cmd_help(void)
{
    printf("ASOS Shell - Built-in commands:\n");
    printf("  help          Show this help message\n");
    printf("  echo [text]   Print text\n");
    printf("  pid           Show shell PID\n");
    printf("  exec [file]   Run an ELF program\n");
    printf("  halt          Shut down the system\n");
    printf("\n");
    printf("Or type the name of an ELF file to run it.\n");
    printf("Example: hello\n");
}

static void cmd_echo(const char *args)
{
    if (args && *args)
        printf("%s\n", args);
    else
        printf("\n");
}

static void cmd_pid(void)
{
    printf("Shell PID: %d\n", getpid());
}

/* ── Program execution ──────────────────────────────────────────────── */

static void run_program(const char *name)
{
    char path[MAX_PATH_LEN];

    /* Build path: prepend / if needed. */
    if (name[0] == '/') {
        strncpy(path, name, MAX_PATH_LEN - 1);
    } else {
        path[0] = '/';
        strncpy(path + 1, name, MAX_PATH_LEN - 2);
    }
    path[MAX_PATH_LEN - 1] = '\0';

    /* Convert to uppercase for FAT32 8.3 compatibility. */
    for (int i = 0; path[i]; i++)
        path[i] = (char)toupper(path[i]);

    /* Append .ELF if no extension present. */
    if (!strchr(path + 1, '.')) {
        size_t len = strlen(path);
        if (len + 4 < MAX_PATH_LEN)
            strcat(path, ".ELF");
    }

    long pid = spawn(path);
    if (pid < 0) {
        printf("shell: %s: command not found\n", name);
        return;
    }

    /* Wait for the child to finish. */
    int status;
    long waited = waitpid(pid, &status);
    if (waited > 0 && status != 0)
        printf("shell: process %ld exited with status %d\n", waited, status);
}

/* ── Command parsing ────────────────────────────────────────────────── */

static void trim(char *str)
{
    /* Leading whitespace. */
    char *start = str;
    while (*start && isspace(*start)) start++;
    if (start != str) memmove(str, start, strlen(start) + 1);

    /* Trailing whitespace. */
    size_t len = strlen(str);
    while (len > 0 && isspace(str[len - 1]))
        str[--len] = '\0';
}

static void process_command(char *cmd)
{
    trim(cmd);
    if (cmd[0] == '\0') return;

    /* Split into command and arguments at first space. */
    char *args = strchr(cmd, ' ');
    if (args) {
        *args = '\0';
        args++;
        while (*args && isspace(*args)) args++;
        if (*args == '\0') args = NULL;
    }

    if (strcmp(cmd, "help") == 0) {
        cmd_help();
    } else if (strcmp(cmd, "echo") == 0) {
        cmd_echo(args);
    } else if (strcmp(cmd, "pid") == 0) {
        cmd_pid();
    } else if (strcmp(cmd, "exec") == 0) {
        if (args)
            run_program(args);
        else
            printf("Usage: exec <program>\n");
    } else if (strcmp(cmd, "halt") == 0) {
        printf("System halting.\n");
        exit(0);
    } else if (strcmp(cmd, "exit") == 0) {
        printf("Type 'halt' to shut down the system.\n");
    } else {
        run_program(cmd);
    }
}

/* ── Main loop ──────────────────────────────────────────────────────── */

int main(void)
{
    printf("\n");
    printf("  ___   ___  ___  ___\n");
    printf(" / _ | / __// _ \\/ __/\n");
    printf("/ __ |\\__ \\/ , _/\\ \\\n");
    printf("/_/ |_/___//_/|_/___/\n");
    printf("\n");
    printf("ASOS Shell v0.1.0\n");
    printf("Type 'help' for available commands.\n\n");

    char cmd[MAX_CMD_LEN];

    while (1) {
        printf("asos> ");

        char *result = gets_s(cmd, MAX_CMD_LEN);
        if (!result) {
            printf("\n");
            continue;
        }

        process_command(cmd);
    }

    return 0;
}
