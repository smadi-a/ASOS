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
    printf("ASOS Shell v0.1.0\n\n");
    printf("Navigation:\n");
    printf("  l [dir]              List directory contents\n");
    printf("  go [dir]             Change directory (default: /)\n");
    printf("  path                 Show current directory\n");
    printf("\n");
    printf("File viewing:\n");
    printf("  show <file>          Display file contents\n");
    printf("  top [n] <file>       Display first n lines (default 10)\n");
    printf("  bottom [n] <file>    Display last n lines (default 10)\n");
    printf("\n");
    printf("File operations:\n");
    printf("  new <file>           Create an empty file\n");
    printf("  copy <src> <dst>     Copy a file\n");
    printf("  move <src> <dst>     Move or rename a file\n");
    printf("  delete <file>        Delete a file\n");
    printf("  md <name>            Create a directory\n");
    printf("  deletedir <name>     Remove an empty directory\n");
    printf("\n");
    printf("System:\n");
    printf("  say <text>           Print text\n");
    printf("  clean                Clear the screen\n");
    printf("  pid                  Show shell PID\n");
    printf("  end <pid>            Terminate a process\n");
    printf("  disk                 Show filesystem usage\n");
    printf("  help                 Show this help\n");
    printf("  halt                 Shut down the system\n");
    printf("\n");
    printf("Type any program name to run it (e.g., hello)\n");
}

static void cmd_say(const char *args)
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

static void cmd_l(const char *args)
{
    const char *path;
    char resolved[256];

    if (args && *args) {
        path = args;
    } else {
        getcwd(resolved, sizeof(resolved));
        path = resolved;
    }

    dirent_t entries[64];
    int count = readdir(path, entries, 64);

    if (count < 0) {
        printf("l: cannot list '%s'\n", path);
        return;
    }

    if (count == 0) {
        printf("(empty)\n");
        return;
    }

    for (int i = 0; i < count; i++) {
        if (entries[i].is_directory) {
            printf("  [DIR]       %s\n", entries[i].name);
        } else {
            printf("  %10d  %s\n", entries[i].size, entries[i].name);
        }
    }

    printf("\n%d item%s\n", count, count == 1 ? "" : "s");
}

static void cmd_go(const char *args)
{
    if (!args || !*args) {
        if (chdir("/") != 0)
            printf("go: failed to change to /\n");
        return;
    }
    if (chdir(args) != 0)
        printf("go: '%s': no such directory\n", args);
}

static void cmd_path(const char *args)
{
    (void)args;
    char cwd[256];
    if (getcwd(cwd, sizeof(cwd)) == 0) {
        printf("%s\n", cwd);
    } else {
        printf("path: unable to determine current directory\n");
    }
}

static void cmd_md(const char *args)
{
    if (!args || !*args) {
        printf("Usage: md <directory>\n");
        return;
    }
    if (mkdir(args) != 0)
        printf("md: cannot create '%s'\n", args);
}

static void cmd_new(const char *args)
{
    if (!args || !*args) {
        printf("Usage: new <filename>\n");
        return;
    }
    if (fcreate(args) != 0)
        printf("new: cannot create '%s'\n", args);
}

static void cmd_copy(const char *args)
{
    if (!args || !*args) {
        printf("Usage: copy <source> <destination>\n");
        return;
    }

    char argbuf[512];
    strncpy(argbuf, args, sizeof(argbuf) - 1);
    argbuf[sizeof(argbuf) - 1] = '\0';

    char *dst = strchr(argbuf, ' ');
    if (!dst) {
        printf("Usage: copy <source> <destination>\n");
        return;
    }
    *dst = '\0';
    dst++;
    while (*dst == ' ') dst++;

    if (*dst == '\0') {
        printf("Usage: copy <source> <destination>\n");
        return;
    }

    if (copy(argbuf, dst) != 0)
        printf("copy: failed to copy '%s' to '%s'\n", argbuf, dst);
}

static void cmd_move(const char *args)
{
    if (!args || !*args) {
        printf("Usage: move <source> <destination>\n");
        return;
    }

    char argbuf[512];
    strncpy(argbuf, args, sizeof(argbuf) - 1);
    argbuf[sizeof(argbuf) - 1] = '\0';

    char *dst = strchr(argbuf, ' ');
    if (!dst) {
        printf("Usage: move <source> <destination>\n");
        return;
    }
    *dst = '\0';
    dst++;
    while (*dst == ' ') dst++;

    if (*dst == '\0') {
        printf("Usage: move <source> <destination>\n");
        return;
    }

    if (rename(argbuf, dst) != 0)
        printf("move: failed to move '%s' to '%s'\n", argbuf, dst);
}

static void cmd_delete(const char *args)
{
    if (!args || !*args) {
        printf("Usage: delete <filename>\n");
        return;
    }
    if (fdelete(args) != 0)
        printf("delete: cannot delete '%s'\n", args);
}

static void cmd_show(const char *args)
{
    if (!args || !*args) {
        printf("Usage: show <filename>\n");
        return;
    }

    int fd = fopen(args);
    if (fd < 0) {
        printf("show: '%s': file not found\n", args);
        return;
    }

    char buf[512];
    long n;
    while ((n = fread(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        printf("%s", buf);
    }
    printf("\n");
    fclose(fd);
}

static void cmd_top(const char *args)
{
    if (!args || !*args) {
        printf("Usage: top [n] <filename>\n");
        return;
    }

    int lines = 10;
    const char *filename = args;

    if (isdigit(args[0])) {
        lines = atoi(args);
        if (lines <= 0) lines = 10;
        filename = strchr(args, ' ');
        if (!filename) {
            printf("Usage: top [n] <filename>\n");
            return;
        }
        filename++;
        while (*filename == ' ') filename++;
    }

    int fd = fopen(filename);
    if (fd < 0) {
        printf("top: '%s': file not found\n", filename);
        return;
    }

    int line_count = 0;
    char buf[512];
    long n;
    while (line_count < lines && (n = fread(fd, buf, sizeof(buf) - 1)) > 0) {
        for (long i = 0; i < n && line_count < lines; i++) {
            putchar(buf[i]);
            if (buf[i] == '\n') line_count++;
        }
    }
    if (line_count == 0) printf("\n");
    fclose(fd);
}

static void cmd_bottom(const char *args)
{
    if (!args || !*args) {
        printf("Usage: bottom [n] <filename>\n");
        return;
    }

    int lines = 10;
    const char *filename = args;

    if (isdigit(args[0])) {
        lines = atoi(args);
        if (lines <= 0) lines = 10;
        filename = strchr(args, ' ');
        if (!filename) {
            printf("Usage: bottom [n] <filename>\n");
            return;
        }
        filename++;
        while (*filename == ' ') filename++;
    }

    int fd = fopen(filename);
    if (fd < 0) {
        printf("bottom: '%s': file not found\n", filename);
        return;
    }

    long size = fsize(fd);
    if (size <= 0) {
        printf("\n");
        fclose(fd);
        return;
    }

    /* Cap at reasonable size to avoid huge allocations. */
    if (size > 65536) {
        fseek(fd, -65536, SEEK_END);
        size = 65536;
    }

    char *data = malloc((size_t)size + 1);
    if (!data) {
        printf("bottom: out of memory\n");
        fclose(fd);
        return;
    }

    long n = fread(fd, data, (size_t)size);
    fclose(fd);
    data[n] = '\0';

    /* Count newlines from the end to find where to start printing. */
    int count = 0;
    long start = n;
    for (long i = n - 1; i >= 0; i--) {
        if (data[i] == '\n') {
            count++;
            if (count == lines + 1) {
                start = i + 1;
                break;
            }
        }
    }
    if (count <= lines) start = 0;

    printf("%s", data + start);
    if (n > 0 && data[n - 1] != '\n') printf("\n");

    free(data);
}

static void cmd_deletedir(const char *args)
{
    if (!args || !*args) {
        printf("Usage: deletedir <directory>\n");
        return;
    }
    if (rmdir(args) != 0)
        printf("deletedir: cannot remove '%s' (not found or not empty)\n", args);
}

static void cmd_end(const char *args)
{
    if (!args || !*args) {
        printf("Usage: end <pid>\n");
        return;
    }

    long pid = atol(args);
    if (pid <= 0) {
        printf("end: invalid PID\n");
        return;
    }

    if (kill(pid) != 0)
        printf("end: process %ld not found or cannot be killed\n", pid);
    else
        printf("end: process %ld terminated\n", pid);
}

static void cmd_disk(const char *args)
{
    (void)args;
    fs_stat_t stat;
    if (fsstat(&stat) != 0) {
        printf("disk: cannot read filesystem stats\n");
        return;
    }

    uint64_t total_kb = stat.total_bytes / 1024;
    uint64_t used_kb = stat.used_bytes / 1024;
    uint64_t free_kb = stat.free_bytes / 1024;
    uint64_t percent = 0;
    if (stat.total_bytes > 0) {
        percent = (stat.used_bytes * 100) / stat.total_bytes;
    }

    printf("Filesystem      Total     Used     Free   Use%%\n");
    printf("/            %6ld KB %6ld KB %6ld KB   %ld%%\n",
           total_kb, used_kb, free_kb, percent);
}

static void cmd_clean(const char *args)
{
    (void)args;
    for (int i = 0; i < 30; i++) printf("\n");
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

    if (strcmp(cmd, "help") == 0)         cmd_help();
    else if (strcmp(cmd, "halt") == 0)    { printf("System halting.\n"); exit(0); }
    else if (strcmp(cmd, "out") == 0)     printf("Type 'halt' to shut down.\n");
    else if (strcmp(cmd, "say") == 0)     cmd_say(args);
    else if (strcmp(cmd, "clean") == 0)   cmd_clean(args);
    else if (strcmp(cmd, "l") == 0)       cmd_l(args);
    else if (strcmp(cmd, "go") == 0)      cmd_go(args);
    else if (strcmp(cmd, "path") == 0)    cmd_path(args);
    else if (strcmp(cmd, "show") == 0)    cmd_show(args);
    else if (strcmp(cmd, "top") == 0)     cmd_top(args);
    else if (strcmp(cmd, "bottom") == 0)  cmd_bottom(args);
    else if (strcmp(cmd, "new") == 0)     cmd_new(args);
    else if (strcmp(cmd, "copy") == 0)    cmd_copy(args);
    else if (strcmp(cmd, "move") == 0)    cmd_move(args);
    else if (strcmp(cmd, "delete") == 0)  cmd_delete(args);
    else if (strcmp(cmd, "md") == 0)      cmd_md(args);
    else if (strcmp(cmd, "deletedir") == 0) cmd_deletedir(args);
    else if (strcmp(cmd, "pid") == 0)     cmd_pid();
    else if (strcmp(cmd, "disk") == 0)    cmd_disk(args);
    else if (strcmp(cmd, "end") == 0)     cmd_end(args);
    else if (strcmp(cmd, "exec") == 0) {
        if (args) run_program(args);
        else printf("Usage: exec <program>\n");
    }
    else run_program(cmd);
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
        char cwd[256];
        getcwd(cwd, sizeof(cwd));
        printf("asos:%s> ", cwd);

        char *result = gets_s(cmd, MAX_CMD_LEN);
        if (!result) {
            printf("\n");
            continue;
        }

        process_command(cmd);
    }

    return 0;
}
