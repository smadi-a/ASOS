/*
 * stdio.h — Formatted output.
 */

#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdarg.h>

#define EOF (-1)

int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int sprintf(char *buf, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
int snprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int vsprintf(char *buf, const char *fmt, va_list ap);
int sscanf(const char *str, const char *fmt, ...);

int putchar(int c);
int puts(const char *s);
int getchar(void);
char *gets_s(char *buf, size_t size);

#endif /* _STDIO_H */
